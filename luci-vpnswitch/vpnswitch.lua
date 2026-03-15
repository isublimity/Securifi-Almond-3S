module("luci.controller.vpnswitch", package.seeall)

local json = require "luci.jsonc"

function index()
	entry({"admin", "services", "vpnswitch"}, template("vpnswitch"), "VPN Switch", 10)
	entry({"admin", "services", "vpnswitch", "switch"}, call("action_switch"), nil).leaf = true
	entry({"admin", "services", "vpnswitch", "status"}, call("action_status"), nil).leaf = true
	entry({"admin", "services", "vpnswitch", "signal"}, call("action_signal"), nil).leaf = true
	entry({"admin", "services", "vpnswitch", "iperf"}, call("action_iperf"), nil).leaf = true
end

function get_signal()
	local sig = {csq = 0, rssi = 0, rsrp = 0, rsrq = 0, sinr = 0, bars = 0, tech = "N/A", band = 0}

	-- Single AT session via Quectel EC21-E on /dev/ttyUSB2
	local raw = luci.sys.exec(
		"(echo -e 'AT+CSQ\\r\\nAT+QENG=\"servingcell\"\\r\\n'; sleep 1) " ..
		"| socat -T 3 -t 3 STDIO /dev/ttyUSB2,crnl,nonblock 2>/dev/null"
	)

	-- Parse CSQ
	local csq = raw:match("%+CSQ:%s*(%d+)")
	if csq then
		sig.csq = tonumber(csq)
		sig.rssi = -113 + sig.csq * 2
	end

	-- Parse QENG servingcell
	-- +QENG: "servingcell","NOCONN","LTE","FDD",250,99,BCAEE67,218,525,1,4,4,9C29,-96,-16,-61,-1,26
	local qeng = raw:match("%+QENG:.-\n") or ""

	local tech = qeng:match('"servingcell","[^"]*","(%w+)"')
	if tech then sig.tech = tech end

	-- Extract all comma-separated fields after the hex CellID
	local after_hex = qeng:match(",(%x+,%-?%d+,%-?%d+,%-?%d+,%-?%d+,%-?%d+)%s")
	if not after_hex then
		-- Try matching RSRP directly: find sequence of negative numbers
		local rsrp, rsrq, rssi_val, sinr_val = qeng:match(",(%-?%d+),(%-?%d+),(%-?%d+),(%-?%d+),%d+%s")
		if rsrp then
			sig.rsrp = tonumber(rsrp)
			sig.rsrq = tonumber(rsrq)
			sig.rssi = tonumber(rssi_val)
			sig.sinr = tonumber(sinr_val)
		end
	end

	-- Fallback: find RSRP by scanning all numbers for range -140..-44
	if sig.rsrp == 0 then
		local nums = {}
		for n in qeng:gmatch(",(%-?%d+)") do
			table.insert(nums, tonumber(n))
		end
		for i = 1, #nums do
			if nums[i] >= -140 and nums[i] <= -44 then
				sig.rsrp = nums[i]
				if nums[i+1] then sig.rsrq = nums[i+1] end
				if nums[i+2] then sig.rssi = nums[i+2] end
				if nums[i+3] then sig.sinr = nums[i+3] end
				break
			end
		end
	end

	-- Band from QENG: after FDD, fields are MCC,MNC,CellID,PCID,EARFCN,Band
	local b = qeng:match('"FDD",%d+,%d+,%w+,%d+,%d+,(%d+)')
	if b then sig.band = tonumber(b) end

	-- Calculate bars from RSRP (preferred) or CSQ (fallback)
	if sig.rsrp ~= 0 then
		if sig.rsrp >= -80 then sig.bars = 5
		elseif sig.rsrp >= -90 then sig.bars = 4
		elseif sig.rsrp >= -100 then sig.bars = 3
		elseif sig.rsrp >= -110 then sig.bars = 2
		elseif sig.rsrp >= -120 then sig.bars = 1
		else sig.bars = 0 end
	else
		if sig.csq >= 20 then sig.bars = 5
		elseif sig.csq >= 15 then sig.bars = 4
		elseif sig.csq >= 10 then sig.bars = 3
		elseif sig.csq >= 5 then sig.bars = 2
		elseif sig.csq >= 1 then sig.bars = 1
		else sig.bars = 0 end
	end

	return sig
end

function get_ip_info()
	local result = {ip = "N/A", country = "", city = "", region = "", geo = "", checks = {}}

	luci.sys.exec("rm -f /tmp/ipcheck_*")
	luci.sys.exec(
		"(wget -qO- -T 5 'http://ip-api.com/json' > /tmp/ipcheck_ipapi 2>/dev/null &) ; " ..
		"(wget -qO- -T 5 'http://ifconfig.me/ip' > /tmp/ipcheck_ifcfg 2>/dev/null &) ; " ..
		"sleep 6; wait 2>/dev/null"
	)

	local raw2 = luci.sys.exec("cat /tmp/ipcheck_ipapi 2>/dev/null")
	if raw2 and raw2 ~= "" then
		local ok2, d2 = pcall(json.parse, raw2)
		if ok2 and d2 then
			local geo2 = (d2.country or "") .. " " .. (d2.city or "")
			table.insert(result.checks, {name = "ip-api.com", ip = d2.query or "N/A", geo = geo2, ok = true})
			if result.ip == "N/A" then
				result.ip = d2.query or "N/A"
				result.country = d2.country or ""
				result.city = d2.city or ""
			end
		end
	else
		table.insert(result.checks, {name = "ip-api.com", ip = "timeout", ok = false})
	end

	local raw3 = luci.sys.exec("cat /tmp/ipcheck_ifcfg 2>/dev/null"):gsub("%s+","")
	if raw3 ~= "" then
		table.insert(result.checks, {name = "ifconfig.me", ip = raw3, ok = true})
	else
		table.insert(result.checks, {name = "ifconfig.me", ip = "timeout", ok = false})
	end

	luci.sys.exec("rm -f /tmp/ipcheck_*")
	return result
end

function action_signal()
	local http = require "luci.http"
	http.prepare_content("application/json")
	http.write_json(get_signal())
end

function action_status()
	local http = require "luci.http"

	local wg_disabled = luci.sys.exec("uci -q get network.wgvpn.disabled"):gsub("%s+","")
	local ovpn_enabled = luci.sys.exec("uci -q get openvpn.sirius.enabled"):gsub("%s+","")
	local ovpn_running = (luci.sys.exec("pgrep -x openvpn"):gsub("%s+","") ~= "")
	local wg_running = (luci.sys.exec("wg show wgvpn 2>/dev/null | head -1"):gsub("%s+","") ~= "")

	local mode = "off"
	if wg_disabled ~= "1" and wg_running then
		mode = "wg"
	elseif ovpn_enabled == "1" and ovpn_running then
		mode = "ovpn"
	end

	local info = get_ip_info()
	local sig = get_signal()

	http.prepare_content("application/json")
	http.write_json({
		mode = mode,
		ip = info.ip, country = info.country, city = info.city,
		region = info.region, geo = info.geo, checks = info.checks,
		signal = sig
	})
end

function action_switch()
	local http = require "luci.http"
	local sys = require "luci.sys"
	local mode = http.formvalue("mode") or "off"

	if mode == "wg" then
		sys.exec("uci set openvpn.sirius.enabled=0; uci commit openvpn; /etc/init.d/openvpn stop 2>/dev/null")
		sys.exec("sleep 1")
		sys.exec("uci set network.wgvpn.disabled=0; uci commit network; ifup wgvpn")
	elseif mode == "ovpn" then
		sys.exec("ifdown wgvpn 2>/dev/null; uci set network.wgvpn.disabled=1; uci commit network")
		sys.exec("sleep 1")
		sys.exec("uci set openvpn.sirius.enabled=1; uci commit openvpn; /etc/init.d/openvpn restart")
	else
		sys.exec("ifdown wgvpn 2>/dev/null; uci set network.wgvpn.disabled=1; uci commit network")
		sys.exec("uci set openvpn.sirius.enabled=0; uci commit openvpn; /etc/init.d/openvpn stop 2>/dev/null")
		sys.exec("sleep 1; ifup lte")
	end

	sys.exec("sleep 3")

	local wg_disabled = sys.exec("uci -q get network.wgvpn.disabled"):gsub("%s+","")
	local ovpn_enabled = sys.exec("uci -q get openvpn.sirius.enabled"):gsub("%s+","")
	local cur = "off"
	if wg_disabled ~= "1" then cur = "wg"
	elseif ovpn_enabled == "1" then cur = "ovpn"
	end

	local sig = get_signal()
	http.prepare_content("application/json")
	http.write_json({mode = cur, ip = "checking...", checks = {}, signal = sig})
end

function action_iperf()
	local http = require "luci.http"
	local sys = require "luci.sys"
	local duration = tonumber(http.formvalue("t")) or 5
	local reverse = http.formvalue("r") == "1"

	if duration < 3 then duration = 3 end
	if duration > 30 then duration = 30 end

	sys.exec("killall iperf 2>/dev/null")

	local port = "5199"
	local rev = reverse and " -r" or ""

	-- iperf2: -c host -p port -t duration -f m (megabits) -e (enhanced)
	local cmd = string.format(
		"LD_LIBRARY_PATH=/tmp/usr/lib /tmp/usr/bin/iperf -c %s -p %s -t %d -f m -e%s 2>&1",
		host, port, duration, rev
	)

	local raw = sys.exec(cmd)
	local result = {ok = false, host = host, port = port, duration = duration, reverse = reverse, raw = ""}

	-- Parse iperf2 output
	-- Format: [  1] 0.0000-5.0000 sec  X.XX MBytes  X.XX Mbits/sec
	local transfer, bandwidth = raw:match("(%d+%.?%d*)%s+MBytes%s+(%d+%.?%d*)%s+Mbits/sec")
	if bandwidth then
		result.ok = true
		result.bandwidth_mbps = tonumber(bandwidth)
		result.transfer_mb = tonumber(transfer)
		-- Parse per-second intervals if available with -e
		result.intervals = {}
		for sec, bw in raw:gmatch("(%d+%.%d+)%-[%d%.]+%s+sec%s+[%d%.]+%s+%a+%s+(%d+%.?%d*)%s+Mbits") do
			table.insert(result.intervals, {sec = tonumber(sec), mbps = tonumber(bw)})
		end
		-- Remove last entry (it's the summary)
		if #result.intervals > 1 then
			table.remove(result.intervals)
		end
	else
		result.error = raw:match("connect failed") or raw:match("error.-[\r\n]") or "test failed"
		result.raw = raw:sub(1, 200)
	end

	http.prepare_content("application/json")
	http.write_json(result)
end
