#!/usr/bin/lua
--
-- lcd_ui.lua — LCD UI for Almond 3S
--
-- 2 страницы по 6 кнопок (3×2), стрелка ">" для переключения
-- Вложенные меню с кнопкой BACK
-- Скринсейвер + backlight off (burn-in protection)
--

local LCD_W = 320
local LCD_H = 240
local C_BG="#000000" local C_HEADER="#001F" local C_WHITE="white"
local C_GREEN="#07E0" local C_RED="red" local C_YELLOW="yellow"
local C_CYAN="cyan" local C_GRAY="#4208" local C_BTN="#1082" local C_BACK="#4000"
local SOCK = "/tmp/lcd.sock"
local HISTORY_LEN = 60
local IDLE_TO_SAVER = 240  -- 4 минуты
local IDLE_TO_OFF   = 60   -- + 1 минута

-- === Buffered LCD ===
local cmd_buf = {}
local function Q(j) cmd_buf[#cmd_buf+1]=j end
local function lcd_flush()
    if #cmd_buf==0 then return end
    local f=io.open("/tmp/.lcd_cmds","w")
    if f then f:write(table.concat(cmd_buf,"\n").."\n"); f:close()
        os.execute("socat -u FILE:/tmp/.lcd_cmds UNIX-CONNECT:"..SOCK.." 2>/dev/null")
    end; cmd_buf={}
end
local function lcd_clear(c) Q(string.format('{"cmd":"clear","color":"%s"}',c or C_BG)) end
local function lcd_rect(x,y,w,h,c) Q(string.format('{"cmd":"rect","x":%d,"y":%d,"w":%d,"h":%d,"color":"%s"}',x or 0,y or 0,w or 1,h or 1,c or C_BG)) end
local function lcd_text(x,y,t,c,bg,s) Q(string.format('{"cmd":"text","x":%d,"y":%d,"text":"%s","color":"%s","bg":"%s","size":%d}',x or 0,y or 0,t or "",c or C_WHITE,bg or C_BG,s or 2)) end

-- === lcdlib ===
local lcd = require("lcdlib"); lcd.open()
local function read_touch() local x,y,p=lcd.touch(); if p==1 then return x,y end; return nil end
local function sleep_ms(ms) lcd.usleep(ms*1000) end

-- === Helpers ===
local function read_file(p) local f=io.open(p); if not f then return "" end; local s=f:read("*a") or ""; f:close(); return s end
local function push(a,v) a[#a+1]=v; if #a>HISTORY_LEN then table.remove(a,1) end end
local function fmt(b) if b>1048576 then return string.format("%.1fM",b/1048576) end; if b>1024 then return string.format("%.0fK",b/1024) end; return tostring(b) end

-- === Data ===
local cache = {
    wg_on=false, ovpn_on=false,
    rsrp=0, rsrq=0, sinr=0, rssi=0, band="?", band_info="", modem_temp=0,
    modem_imei="", modem_fw="", modem_iccid="", balance="",
    rx_speed=0, tx_speed=0,
    sms_count=0, sms_list={},
    ext_ip="", ext_country="", ext_city="", ip_checks={},
    wifi_2g_clients=0, wifi_5g_clients=0, wifi_ssid_2g="", wifi_ssid_5g="",
    wifi_key_2g="", wifi_key_5g="", wifi_clients={},
}
local hist={rsrp={},sinr={},rx={},tx={},br_rx={},br_tx={}}
local last_net=nil

-- === Background collection ===
local function kick_bg_collect()
    os.execute("wg show wgvpn 2>/dev/null|head -1>/tmp/.lcd_wg &")
    os.execute("pgrep -x openvpn>/tmp/.lcd_ovpn 2>/dev/null &")
    os.execute("uqmi -d /dev/cdc-wdm0 --get-signal-info>/tmp/.lcd_sig 2>/dev/null &")
    os.execute("(echo -e 'AT+QNWINFO\\r';sleep 1)|socat -T2 -t2 STDIO /dev/ttyUSB2,crnl,nonblock>/tmp/.lcd_band 2>/dev/null &")
    os.execute("(echo -e 'AT+QTEMP\\r';sleep 1)|socat -T2 -t2 STDIO /dev/ttyUSB2,crnl,nonblock>/tmp/.lcd_temp 2>/dev/null &")
end

local last_ip_kick=0
local function kick_ip_bg()
    if os.time()-last_ip_kick<60 then return end; last_ip_kick=os.time()
    os.execute("wget -qO- -T 5 'http://ip-api.com/json'>/tmp/.lcd_ip_bg 2>/dev/null &")  -- bg: only ip-api (has GEO)
end

local function collect_cached()
    cache.wg_on=read_file("/tmp/.lcd_wg")~=""
    cache.ovpn_on=read_file("/tmp/.lcd_ovpn")~=""
    local raw=read_file("/tmp/.lcd_sig")
    cache.rsrp=tonumber(raw:match('"rsrp":%s*(%-?%d+)')) or cache.rsrp
    cache.rsrq=tonumber(raw:match('"rsrq":%s*(%-?%d+)')) or cache.rsrq
    cache.sinr=tonumber(raw:match('"snr":%s*(%-?[%d%.]+)')) or cache.sinr
    cache.rssi=tonumber(raw:match('"rssi":%s*(%-?%d+)')) or cache.rssi
    push(hist.rsrp,cache.rsrp); push(hist.sinr,cache.sinr)
    local br=read_file("/tmp/.lcd_band")
    cache.band=br:match('"FDD",(%w+)') or cache.band
    cache.band_info=br:match('QNWINFO: "([^"]+)"') or cache.band_info
    local tr=read_file("/tmp/.lcd_temp")
    cache.modem_temp=tonumber(tr:match('QTEMP:%s*(%d+)')) or cache.modem_temp
    -- IP bg
    raw=read_file("/tmp/.lcd_ip_bg")
    if raw~="" then
        cache.ext_ip=raw:match('"query"%s*:%s*"([^"]*)"') or cache.ext_ip
        cache.ext_country=raw:match('"country"%s*:%s*"([^"]*)"') or cache.ext_country
    end
    -- Traffic
    local f=io.open("/proc/net/dev"); if not f then return end; local now={}
    for line in f:lines() do
        local iface,rx,tx=line:match("^%s*(%S+):%s*(%d+)%s+%d+%s+%d+%s+%d+%s+%d+%s+%d+%s+%d+%s+%d+%s+(%d+)")
        if iface then now[iface]={rx=tonumber(rx),tx=tonumber(tx)} end
    end; f:close()
    if last_net then
        local function d(i,k) if now[i] and last_net[i] then local v=now[i][k]-last_net[i][k]; return v>=0 and v or 0 end; return 0 end
        cache.rx_speed=d("wwan0","rx"); cache.tx_speed=d("wwan0","tx")
        push(hist.rx,cache.rx_speed); push(hist.tx,cache.tx_speed)
        push(hist.br_rx,d("br-lan","rx")); push(hist.br_tx,d("br-lan","tx"))
    end; last_net=now
    -- WiFi client count (instant, from /tmp/dhcp.leases or iw)
    local w2=io.popen("iw dev phy0-ap0 station dump 2>/dev/null|grep Station|wc -l")
    cache.wifi_2g_clients=tonumber(w2:read("*l")) or 0; w2:close()
    local w5=io.popen("iw dev phy1-ap0 station dump 2>/dev/null|grep Station|wc -l")
    cache.wifi_5g_clients=tonumber(w5:read("*l")) or 0; w5:close()
end

-- === Blocking collectors ===
local function at_cmd(cmd)
    local p=io.popen("(echo -e '"..cmd.."\\r';sleep 1)|socat -T2 -t2 STDIO /dev/ttyUSB2,crnl,nonblock 2>/dev/null")
    local r=p and p:read("*a") or ""; if p then p:close() end; return r
end

local function collect_modem_info()
    cache.modem_imei=at_cmd("AT+GSN"):match("(%d%d%d%d%d%d%d+)") or "?"
    cache.modem_fw=at_cmd("AT+QGMR"):match("(EC%S+)") or "?"
    cache.modem_iccid=at_cmd("AT+QCCID"):match("QCCID:%s*(%S+)") or "?"
end

local function collect_balance()
    local p=io.popen("(echo -e 'AT+CUSD=1,\"*102#\",15\\r';sleep 5)|socat -T7 -t7 STDIO /dev/ttyUSB2,crnl,nonblock 2>/dev/null")
    local raw=p and p:read("*a") or ""; if p then p:close() end
    local hex=raw:match('+CUSD:%s*%d+,"(%x+)"')
    if hex then
        local d="" ; for i=1,#hex,4 do local cp=tonumber(hex:sub(i,i+3),16)
            if cp and cp<128 then d=d..string.char(cp) end
        end
        cache.balance=(d:match("(%d+%.?%d*)") or "?").." rub"
    else cache.balance="error" end
end

local function collect_sms()
    local raw=""
    local p=io.popen("(echo -e 'AT+CMGF=1\\r';sleep 1;echo -e 'AT+CMGL=\"ALL\"\\r';sleep 2)|socat -T4 -t4 STDIO /dev/ttyUSB2,crnl,nonblock 2>/dev/null")
    if p then raw=p:read("*a") or ""; p:close() end
    cache.sms_list={}; local idx,from,ts
    for line in raw:gmatch("[^\n]+") do
        local i,st,num,t=line:match('%+CMGL:%s*(%d+),"([^"]*)","([^"]*)",.-"([^"]*)"')
        if i then idx=i;from=num;ts=t
        elseif idx and not line:match("^%s*$") and not line:match("^OK") and not line:match("^AT") then
            table.insert(cache.sms_list,{idx=idx,from=from,time=ts,text=line:gsub("%s+$","")});idx=nil
        end
    end; cache.sms_count=#cache.sms_list
end

local ip_services = {
    {id="1", name="ip-api.com",  cmd="wget -qO- -T 5 'http://ip-api.com/json'",          json=true},
    {id="2", name="ipinfo.io",   cmd="wget -qO- -T 5 -U 'curl/7.0' 'http://ipinfo.io/json'", json=true},
    {id="3", name="ipify.org",   cmd="wget -qO- -T 5 'http://api.ipify.org'",             json=false},
}

-- === UI Layout (must be before draw functions) ===
local COLS=2; local BTN_W=155; local BTN_H=70; local BTN_PAD=3
local HDR_H=18; local START_Y=HDR_H+2
local BACK_Y=LCD_H-22

local function btn_pos(idx)
    local col=(idx-1)%COLS; local row=math.floor((idx-1)/COLS)
    return BTN_PAD+col*(BTN_W+BTN_PAD), START_Y+row*(BTN_H+BTN_PAD), BTN_W, BTN_H
end

local function draw_back()
    lcd_rect(0,BACK_Y,LCD_W,22,C_BACK)
    lcd_text(130,BACK_Y+3,"< BACK",C_WHITE,C_BACK,2)
end
local function is_back_touch(ty) return ty>=BACK_Y-10 end

local function lte_quality()
    if cache.rsrp==0 then return "No LTE",C_RED end
    if cache.rsrp>=-80 then return "Excellent",C_GREEN end
    if cache.rsrp>=-90 then return "Good",C_GREEN end
    if cache.rsrp>=-100 then return "Fair",C_YELLOW end
    if cache.rsrp>=-110 then return "Weak",C_YELLOW end
    return "Bad",C_RED
end

local function draw_header(t)
    lcd_rect(0,0,LCD_W,HDR_H,C_HEADER)
    if t then lcd_text(4,2,t,C_WHITE,C_HEADER,2)
    else local q,qc=lte_quality(); lcd_text(4,2,"LTE:"..q,qc,C_HEADER,2) end
    lcd_text(LCD_W-70,2,os.date("%H:%M"),C_CYAN,C_HEADER,2)
end

local function draw_ip_progress(checks)
    lcd_clear(C_BG); draw_header("Checking IP...")
    local y=28
    for _,svc in ipairs(ip_services) do
        local st=checks[svc.id]
        local col=C_GRAY; local txt="waiting..."
        if st then
            if st.ip then col=C_GREEN; txt=st.ip.." "..st.latency
            else col=C_RED; txt="timeout" end
        else col=C_YELLOW; txt="checking..." end
        lcd_text(4,y,svc.name,C_CYAN,C_BG,2); y=y+18
        lcd_text(4,y,txt,col,C_BG,1); y=y+16
    end
    draw_back(); lcd_flush()
end

local function collect_ip_full()
    cache.ip_checks={}
    local checks={}

    -- Show progress FIRST (before any fork)
    draw_ip_progress(checks)

    -- Then kick checks in parallel
    os.execute("rm -f /tmp/.lcd_ip_* 2>/dev/null")
    for _,svc in ipairs(ip_services) do
        os.execute("(t=$(date +%s);"..svc.cmd..">/tmp/.lcd_ip_"..svc.id.." 2>/dev/null;echo $(($(date +%s)-t))>/tmp/.lcd_ip_"..svc.id.."t) &")
    end
    for tick=1,8 do
        sleep_ms(1000)
        for _,svc in ipairs(ip_services) do
            if not checks[svc.id] then
                local r=read_file("/tmp/.lcd_ip_"..svc.id)
                local lat=read_file("/tmp/.lcd_ip_"..svc.id.."t"):gsub("%s","")
                if lat~="" then -- done
                    local ip,geo="timeout",""
                    if r~="" then
                        if svc.json then
                            ip=r:match('"query"%s*:%s*"([^"]*)"') or r:match('"ip"%s*:%s*"([^"]*)"') or "?"
                            local co=r:match('"country"%s*:%s*"([^"]*)"') or ""
                            local ci=r:match('"city"%s*:%s*"([^"]*)"') or ""
                            geo=co.." "..ci
                            if cache.ext_ip=="" then cache.ext_ip=ip; cache.ext_country=co end
                        else
                            ip=r:gsub("%s","")
                            if cache.ext_ip=="" then cache.ext_ip=ip end
                        end
                    end
                    checks[svc.id]={ip=ip,geo=geo,latency=lat.."s"}
                end
            end
        end
        draw_ip_progress(checks)
        -- All done?
        local all=true; for _,svc in ipairs(ip_services) do if not checks[svc.id] then all=false end end
        if all then break end
    end

    -- Store final results
    for _,svc in ipairs(ip_services) do
        local c=checks[svc.id] or {ip="timeout",geo="",latency="-"}
        table.insert(cache.ip_checks,{name=svc.name,ip=c.ip,geo=c.geo,latency=c.latency})
    end
end

local function collect_wifi_detail()
    -- SSIDs and keys
    cache.wifi_ssid_2g=io.popen("uci -q get wireless.default_radio0.ssid 2>/dev/null"):read("*l") or "?"
    cache.wifi_ssid_5g=io.popen("uci -q get wireless.default_radio1.ssid 2>/dev/null"):read("*l") or "?"
    cache.wifi_key_2g=io.popen("uci -q get wireless.default_radio0.key 2>/dev/null"):read("*l") or ""
    cache.wifi_key_5g=io.popen("uci -q get wireless.default_radio1.key 2>/dev/null"):read("*l") or ""
    -- Connected clients
    cache.wifi_clients={}
    for _,dev in ipairs({"phy0-ap0","phy1-ap0"}) do
        local band=dev:match("phy0") and "2G" or "5G"
        local p=io.popen("iw dev "..dev.." station dump 2>/dev/null")
        if p then
            for line in p:lines() do
                local mac=line:match("Station (%S+)")
                if mac then table.insert(cache.wifi_clients,{mac=mac,band=band,ip=""}) end
            end; p:close()
        end
    end
    -- Match MACs to IPs from DHCP leases
    local lf=io.open("/tmp/dhcp.leases")
    if lf then
        for line in lf:lines() do
            local mac,ip,name=line:match("%S+%s+(%S+)%s+(%S+)%s+(%S+)")
            if mac then
                for _,c in ipairs(cache.wifi_clients) do
                    if c.mac:lower()==mac:lower() then c.ip=ip; c.name=name end
                end
            end
        end; lf:close()
    end
end

-- === Graph ===
local function draw_bars(x,y,w,h,data,color,mn,mx)
    if #data<2 then return end; if not mx or mx<=mn then mx=mn+1 end
    local bw=math.max(1,math.floor(w/HISTORY_LEN))
    local st=math.max(1,#data-HISTORY_LEN+1)
    for i=st,#data do
        local v=data[i]-mn; local bh=math.max(1,math.floor(v/(mx-mn)*h))
        local bx=x+(i-st)*bw
        if bx+bw<=x+w then lcd_rect(bx,y+h-bh,bw,bh,color) end
    end
end

-- (layout moved to top)

-- === Pages ===
local page="main"
local main_page=1 -- 1 or 2

local function draw_main()
    lcd_clear(C_BG)
    draw_header()
    local x,y,w,h

    if main_page==1 then
        -- 1: VPN (combined)
        x,y,w,h=btn_pos(1); lcd_rect(x,y,w,h,C_BTN)
        lcd_text(x+4,y+4,"VPN",C_GREEN,C_BTN,2)
        local vpn_mode = cache.wg_on and "WireGuard" or (cache.ovpn_on and "OpenVPN" or "OFF")
        local vpn_col = (cache.wg_on or cache.ovpn_on) and C_GREEN or C_RED
        lcd_text(x+4,y+24,vpn_mode,vpn_col,C_BTN,1)
        -- 2: LTE
        x,y,w,h=btn_pos(2); lcd_rect(x,y,w,h,C_BTN)
        lcd_text(x+4,y+4,"LTE",C_CYAN,C_BTN,2)
        local sc=cache.rsrp>=-90 and C_GREEN or (cache.rsrp>=-110 and C_YELLOW or C_RED)
        lcd_text(x+44,y+4,tostring(cache.rsrp),sc,C_BTN,2)
        lcd_text(x+4,y+24,string.format("SINR:%.0f %dC",cache.sinr,cache.modem_temp),C_GRAY,C_BTN,1)
        -- 3: Traffic
        x,y,w,h=btn_pos(3); lcd_rect(x,y,w,h,C_BTN)
        lcd_text(x+4,y+4,"Traffic",C_WHITE,C_BTN,2)
        lcd_text(x+4,y+24,"R:"..fmt(cache.rx_speed).." T:"..fmt(cache.tx_speed),C_GREEN,C_BTN,1)
        -- 4: IP
        x,y,w,h=btn_pos(4); lcd_rect(x,y,w,h,C_BTN)
        lcd_text(x+4,y+4,"IP",C_WHITE,C_BTN,2)
        if cache.ext_ip~="" then
            lcd_text(x+30,y+4,cache.ext_country,C_GREEN,C_BTN,2)
            lcd_text(x+4,y+24,cache.ext_ip,C_GRAY,C_BTN,1)
        end
        -- 5: WiFi
        x,y,w,h=btn_pos(5); lcd_rect(x,y,w,h,C_BTN)
        lcd_text(x+4,y+4,"WiFi",C_CYAN,C_BTN,2)
        lcd_text(x+4,y+24,string.format("2G:%d 5G:%d",cache.wifi_2g_clients,cache.wifi_5g_clients),C_GREEN,C_BTN,1)
        -- 6: MORE >>>
        x,y,w,h=btn_pos(6); lcd_rect(x,y,w,h,C_HEADER)
        lcd_text(x+24,y+12,"MORE >>>",C_WHITE,C_HEADER,2)
    else
        -- Page 2
        -- 1: Band
        x,y,w,h=btn_pos(1); lcd_rect(x,y,w,h,C_BTN)
        lcd_text(x+4,y+4,"Band",C_CYAN,C_BTN,2)
        lcd_text(x+4,y+24,cache.band.." "..cache.band_info:sub(1,12),C_WHITE,C_BTN,1)
        -- 2: Modem
        x,y,w,h=btn_pos(2); lcd_rect(x,y,w,h,C_BTN)
        lcd_text(x+4,y+4,"Modem",C_CYAN,C_BTN,2)
        if cache.balance~="" then lcd_text(x+4,y+24,cache.balance,C_GREEN,C_BTN,1) end
        -- 3: SMS
        x,y,w,h=btn_pos(3); lcd_rect(x,y,w,h,C_BTN)
        lcd_text(x+4,y+4,"SMS",C_YELLOW,C_BTN,2)
        if cache.sms_count>0 then lcd_text(x+50,y+4,tostring(cache.sms_count),C_RED,C_BTN,2) end
        -- 4-7: (empty)
        -- 6: <<< BACK
        x,y,w,h=btn_pos(6); lcd_rect(x,y,w,h,C_HEADER)
        lcd_text(x+24,y+12,"<<< BACK",C_WHITE,C_HEADER,2)
    end
    lcd_flush()
end

-- === Sub-pages ===

local function draw_signal_page()
    lcd_clear(C_BG); draw_header("LTE Signal"); local y=24
    lcd_text(4,y,"RSRP:"..cache.rsrp.."dBm",C_GREEN,C_BG,1); y=y+10
    lcd_rect(4,y,LCD_W-8,40,"#0841"); draw_bars(4,y,LCD_W-8,40,hist.rsrp,C_GREEN,-140,-44); y=y+44
    lcd_text(4,y,"SINR:"..string.format("%.1f",cache.sinr).."dB",C_CYAN,C_BG,1); y=y+10
    lcd_rect(4,y,LCD_W-8,40,"#0841"); draw_bars(4,y,LCD_W-8,40,hist.sinr,C_CYAN,-20,30); y=y+44
    lcd_text(4,y,string.format("RSRQ:%d RSSI:%d Band:%s Temp:%dC",cache.rsrq,cache.rssi,cache.band,cache.modem_temp),C_GRAY,C_BG,1)
    draw_back(); lcd_flush()
end

local function draw_traffic_page()
    lcd_clear(C_BG); draw_header("Traffic"); local y=24
    for _,ifc in ipairs({{n="wwan0",rx=hist.rx,tx=hist.tx},{n="br-lan",rx=hist.br_rx,tx=hist.br_tx}}) do
        if #ifc.rx>1 then
            local mx=1; for _,v in ipairs(ifc.rx) do if v>mx then mx=v end end; for _,v in ipairs(ifc.tx) do if v>mx then mx=v end end
            lcd_text(4,y,ifc.n,C_WHITE,C_BG,1); lcd_text(70,y,"R:"..fmt(ifc.rx[#ifc.rx]),C_GREEN,C_BG,1); lcd_text(170,y,"T:"..fmt(ifc.tx[#ifc.tx]),C_RED,C_BG,1); y=y+10
            lcd_rect(4,y,LCD_W-8,36,"#0841"); draw_bars(4,y,LCD_W-8,36,ifc.rx,C_GREEN,0,mx); draw_bars(4,y,LCD_W-8,36,ifc.tx,C_RED,0,mx); y=y+40
        else lcd_text(4,y,ifc.n..":...",C_GRAY,C_BG,1); y=y+14 end
    end; draw_back(); lcd_flush()
end

local function draw_ip_page()
    lcd_clear(C_BG); draw_header("Exit IP"); local y=24
    for _,c in ipairs(cache.ip_checks) do
        lcd_text(4,y,c.name.." "..c.latency,C_CYAN,C_BG,1); y=y+10
        lcd_text(4,y,c.ip,C_WHITE,C_BG,2); y=y+18
        if c.geo and c.geo~="" and c.geo~=" " then lcd_text(4,y,c.geo,C_GREEN,C_BG,1); y=y+10 end; y=y+4
    end; draw_back(); lcd_flush()
end

local BAND_LIST={
    {id="3",mask="0x4",name="B3 1800"},{id="7",mask="0x40",name="B7 2600"},
    {id="20",mask="0x80000",name="B20 800"},{id="1",mask="0x1",name="B1 2100"},
    {id="ALL",mask="0x800d5",name="ALL bands"},
}
local function draw_band_page()
    lcd_clear(C_BG); draw_header("LTE Band"); local y=24
    lcd_text(4,y,"Now: "..cache.band_info.." RSRP:"..cache.rsrp,C_CYAN,C_BG,1); y=y+14
    for i,b in ipairs(BAND_LIST) do
        lcd_rect(4,y,LCD_W-8,22,C_BTN); lcd_text(8,y+3,"Lock "..b.name,C_CYAN,C_BTN,2); y=y+26
    end; draw_back(); lcd_flush()
end

local function handle_band_touch(tx,ty)
    if is_back_touch(ty) then page="main"; draw_main(); return true end
    local y=38
    for i,b in ipairs(BAND_LIST) do
        if ty>=y and ty<y+22 then
            lcd_clear(C_BG); draw_header("Switching..."); lcd_text(20,100,"Band "..b.name.."...",C_YELLOW,C_BG,3); lcd_flush()
            os.execute('(echo -e "AT+QCFG=\\"band\\",0,'..b.mask..',0\\r";sleep 1)|socat -T2 -t2 STDIO /dev/ttyUSB2,crnl,nonblock>/dev/null 2>&1')
            os.execute("(ifdown lte;sleep 3;ifup lte)>/dev/null 2>&1 &")
            sleep_ms(5000); kick_bg_collect(); sleep_ms(2000); collect_cached()
            draw_band_page(); return true
        end; y=y+26
    end; return false
end

-- VPN PAGE
local function draw_vpn_page()
    lcd_clear(C_BG); draw_header("VPN")
    local y=24
    local vpn_mode = cache.wg_on and "WireGuard" or (cache.ovpn_on and "OpenVPN" or "OFF")
    lcd_text(4,y,"Current: "..vpn_mode, cache.wg_on and C_GREEN or (cache.ovpn_on and C_YELLOW or C_RED), C_BG,2); y=y+24
    lcd_rect(4,y,LCD_W-8,30, cache.wg_on and "#0841" or C_BTN)
    lcd_text(8,y+6,"WireGuard", cache.wg_on and C_GREEN or C_WHITE, cache.wg_on and "#0841" or C_BTN,2)
    if cache.wg_on then lcd_text(LCD_W-40,y+6,"ON",C_GREEN,"#0841",2) end; y=y+34
    lcd_rect(4,y,LCD_W-8,30, cache.ovpn_on and "#0841" or C_BTN)
    lcd_text(8,y+6,"OpenVPN", cache.ovpn_on and C_YELLOW or C_WHITE, cache.ovpn_on and "#0841" or C_BTN,2)
    if cache.ovpn_on then lcd_text(LCD_W-40,y+6,"ON",C_GREEN,"#0841",2) end; y=y+34
    local off_on = not cache.wg_on and not cache.ovpn_on
    lcd_rect(4,y,LCD_W-8,30, off_on and "#4000" or C_BTN)
    lcd_text(8,y+6,"VPN OFF (direct)", off_on and C_RED or C_WHITE, off_on and "#4000" or C_BTN,2)
    draw_back(); lcd_flush()
end

local function handle_vpn_touch(tx,ty)
    if is_back_touch(ty) then page="main"; draw_main(); return true end
    if ty>=48 and ty<78 then -- WG
        os.execute("(uci set openvpn.sirius.enabled=0;uci commit openvpn;/etc/init.d/openvpn stop;uci set network.wgvpn.disabled=0;uci commit network;ifup wgvpn)>/dev/null 2>&1 &")
        cache.wg_on=true; cache.ovpn_on=false; cache.ext_ip=""; last_ip_kick=0
        kick_bg_collect(); sleep_ms(2000); draw_vpn_page(); return true
    elseif ty>=82 and ty<112 then -- OVPN
        os.execute("(ifdown wgvpn;uci set network.wgvpn.disabled=1;uci commit network;uci set openvpn.sirius.enabled=1;uci commit openvpn;/etc/init.d/openvpn restart)>/dev/null 2>&1 &")
        cache.wg_on=false; cache.ovpn_on=true; cache.ext_ip=""; last_ip_kick=0
        kick_bg_collect(); sleep_ms(2000); draw_vpn_page(); return true
    elseif ty>=116 and ty<146 then -- OFF
        os.execute("(ifdown wgvpn;uci set network.wgvpn.disabled=1;uci commit network;uci set openvpn.sirius.enabled=0;uci commit openvpn;/etc/init.d/openvpn stop)>/dev/null 2>&1 &")
        cache.wg_on=false; cache.ovpn_on=false; cache.ext_ip=""; last_ip_kick=0
        kick_bg_collect(); sleep_ms(1000); draw_vpn_page(); return true
    end; return false
end

local function draw_modem_page()
    lcd_clear(C_BG); draw_header("Modem Info"); local y=24
    lcd_text(4,y,"EC21-E "..cache.modem_fw,C_WHITE,C_BG,1); y=y+12
    lcd_text(4,y,"IMEI:"..cache.modem_imei,C_GRAY,C_BG,1); y=y+12
    lcd_text(4,y,"ICCID:"..cache.modem_iccid,C_GRAY,C_BG,1); y=y+16
    lcd_text(4,y,string.format("RSRP:%d SINR:%.0f Band:%s",cache.rsrp,cache.sinr,cache.band),C_GREEN,C_BG,1); y=y+12
    local tc=cache.modem_temp>=60 and C_RED or (cache.modem_temp>=45 and C_YELLOW or C_GREEN)
    lcd_text(4,y,"Temp:"..cache.modem_temp.."C",tc,C_BG,1); y=y+16
    lcd_text(4,y,"Balance: "..cache.balance,C_YELLOW,C_BG,2); y=y+22
    lcd_rect(4,y,120,20,"#0841"); lcd_text(8,y+2,"Check Bal",C_YELLOW,"#0841",2)
    draw_back(); lcd_flush()
end

local function handle_modem_touch(tx,ty)
    if is_back_touch(ty) then page="main"; draw_main(); return true end
    if ty>=130 and ty<=150 and tx<130 then
        lcd_clear(C_BG); draw_header("USSD *102#..."); lcd_text(40,100,"...",C_YELLOW,C_BG,2); lcd_flush()
        collect_balance(); draw_modem_page(); return true
    end; return false
end

local function draw_sms_page()
    lcd_clear(C_BG); draw_header("SMS ("..cache.sms_count..")"); local y=24
    if cache.sms_count==0 then lcd_text(80,100,"No messages",C_GRAY,C_BG,2)
    else for _,s in ipairs(cache.sms_list) do
        if y>BACK_Y-20 then lcd_text(4,y,"...",C_GRAY,C_BG,1); break end
        lcd_text(4,y,s.from:sub(-10),C_CYAN,C_BG,1); lcd_text(120,y,s.time:sub(-8),C_GRAY,C_BG,1); y=y+10
        lcd_text(4,y,s.text:sub(1,50),C_WHITE,C_BG,1); y=y+12
    end end; draw_back(); lcd_flush()
end

local function draw_wifi_page()
    lcd_clear(C_BG); draw_header("WiFi"); local y=24
    -- 2.4 GHz
    lcd_text(4,y,"2.4 GHz: "..cache.wifi_ssid_2g,C_GREEN,C_BG,2); y=y+18
    lcd_text(4,y,"Key: "..cache.wifi_key_2g,C_YELLOW,C_BG,1); y=y+12
    -- 5 GHz
    lcd_text(4,y,"5 GHz: "..cache.wifi_ssid_5g,C_CYAN,C_BG,2); y=y+18
    lcd_text(4,y,"Key: "..cache.wifi_key_5g,C_YELLOW,C_BG,1); y=y+16
    -- Clients
    lcd_text(4,y,"Clients ("..#cache.wifi_clients.."):",C_WHITE,C_BG,1); y=y+12
    for _,c in ipairs(cache.wifi_clients) do
        if y>BACK_Y-12 then lcd_text(4,y,"...",C_GRAY,C_BG,1); break end
        local info=c.band.." "..c.ip
        if c.name and c.name~="*" then info=info.." "..c.name end
        lcd_text(4,y,info,C_GRAY,C_BG,1); y=y+10
    end
    draw_back(); lcd_flush()
end

-- === Touch handler ===

local function handle_main_touch(tx,ty)
    for i=1,6 do
        local x,y,w,h=btn_pos(i)
        if tx>=x and tx<=x+w and ty>=y and ty<=y+h then
            if i==6 then main_page=main_page==1 and 2 or 1; draw_main(); return true end

            -- Flash button (combined with first submenu draw to avoid double flush)
            lcd_rect(x,y,w,h,C_WHITE); lcd_text(x+4,y+4,"...",C_BG,C_WHITE,2); lcd_flush()

            if main_page==1 then
                if i==1 then page="vpn"; draw_vpn_page(); return true
                elseif i==2 then page="signal"; return true
                elseif i==3 then page="traffic"; return true
                elseif i==4 then collect_ip_full(); page="ip"; draw_ip_page(); return true
                elseif i==5 then collect_wifi_detail(); page="wifi"; draw_wifi_page(); return true
                end
            else
                if i==1 then page="band"; draw_band_page(); return true
                elseif i==2 then
                    collect_modem_info(); collect_balance()
                    page="modem"; draw_modem_page(); return true
                elseif i==3 then collect_sms(); page="sms"; draw_sms_page(); return true
                end
            end
            draw_main(); return true
        end
    end; return false
end

-- === Screen state machine ===
local screen_state="active"; local last_touch_time=os.time()

local function set_screen_state(s)
    if s==screen_state then return end; screen_state=s
    if s=="active" then lcd.backlight(true); page="main"; main_page=1; draw_main()
    elseif s=="screensaver" then lcd.backlight(true); lcd.splash()
    elseif s=="off" then lcd_clear(C_BG); lcd_flush(); lcd.backlight(false) end
end

local function on_touch()
    last_touch_time=os.time()
    if screen_state~="active" then set_screen_state("active"); return true end; return false
end

-- === Main ===
local function main()
    io.stdout:setvbuf("no"); print("lcd_ui: starting")
    kick_bg_collect(); sleep_ms(2000); collect_cached(); draw_main()
    print("lcd_ui: ready (pages, idle:"..IDLE_TO_SAVER.."s+"..IDLE_TO_OFF.."s)")

    local last_bg=os.time(); local last_draw=os.time(); local tcd=0
    last_touch_time=os.time()

    while true do
        local tx,ty=read_touch()
        if tx and os.time()>tcd then
            local consumed=on_touch()
            if not consumed and screen_state=="active" then
                if page=="main" then handle_main_touch(tx,ty)
                elseif page=="vpn" then handle_vpn_touch(tx,ty)
                elseif page=="band" then handle_band_touch(tx,ty)
                elseif page=="modem" then handle_modem_touch(tx,ty)
                else if is_back_touch(ty) then page="main"; draw_main() end end
            end
            tcd=os.time()+1; last_draw=os.time()
        end
        local now=os.time()
        if screen_state=="active" and now-last_touch_time>=IDLE_TO_SAVER then set_screen_state("screensaver") end
        if screen_state=="screensaver" and now-last_touch_time>=IDLE_TO_SAVER+IDLE_TO_OFF then set_screen_state("off") end
        if screen_state=="active" then
            if now-last_bg>=3 then kick_bg_collect(); kick_ip_bg(); last_bg=now end
            collect_cached()
            if page=="main" then if now-last_draw>=5 then draw_main(); last_draw=now end
            elseif page=="signal" or page=="traffic" then if now-last_draw>=1 then
                if page=="signal" then draw_signal_page() else draw_traffic_page() end; last_draw=now
            end end
        end
        sleep_ms(screen_state=="off" and 200 or 50)
    end
end

-- Auto-restart on crash: red screen + retry
while true do
    local ok, err = pcall(main)
    if not ok then
        print("lcd_ui: CRASH: " .. tostring(err))
        pcall(function()
            lcd_clear(C_RED)
            lcd_text(10, 40, "UI CRASH", C_WHITE, C_RED, 3)
            lcd_text(10, 80, tostring(err):sub(1,45), C_WHITE, C_RED, 1)
            lcd_text(10, 100, "Restarting...", C_YELLOW, C_RED, 2)
            lcd_flush()
        end)
        lcd.usleep(3000000)
        page = "main"; main_page = 1; screen_state = "active"
    end
end
