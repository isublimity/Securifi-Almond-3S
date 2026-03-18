-- widgets/dashboard.lua — Plasma dashboard screensaver
-- Collects data async to avoid blocking
local B = require("lcd.base")
local M = {title = "Dashboard"}

local cached = {lte = -100, vpn = -1, clients = {}}
local last_collect = 0

local function collect_async()
    -- Only collect every 10 sec, non-blocking
    local now = os.time()
    if (now - last_collect) < 10 then return end
    last_collect = now

    -- WiFi clients (fast, local)
    local cl = B.wifi_clients()
    cached.clients = {}
    for _, c in ipairs(cl) do
        cached.clients[#cached.clients + 1] = {kbps = math.floor(c.tx_mbps * 100), signal = c.signal}
    end

    -- LTE signal (may hang — use cached file)
    local sig_file = io.open("/tmp/.lte_rsrp")
    if sig_file then
        cached.lte = tonumber(sig_file:read("*l")) or -100
        sig_file:close()
    end
    -- Update file in background
    os.execute("(uqmi -d /dev/cdc-wdm0 --get-signal-info 2>/dev/null | grep -oP '\"rssi\":\\s*\\K-?\\d+' > /tmp/.lte_rsrp) &")

    -- VPN (check interface only — no ping)
    if B.sh("ip link show tun0 2>/dev/null", 1):match("UP") then
        cached.vpn = 80
    elseif B.sh("ip link show wg0 2>/dev/null", 1):match("UP") then
        cached.vpn = 50
    else
        cached.vpn = -1
    end
end

function M.draw()
    collect_async()
    B.dashboard(cached)
end

function M.on_tap(x, y) return "menu" end

return M
