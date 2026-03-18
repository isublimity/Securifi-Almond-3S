-- widgets/dashboard.lua — Plasma dashboard screensaver
local B = require("lcd.base")
local M = {title = "Dashboard"}

function M.draw()
    local cl = B.wifi_clients()
    local sig = B.lte_signal()
    local vpn = B.vpn_status()
    local vpn_ms = -1
    if vpn.ovpn then
        local m = B.sh("ping -c1 -W1 10.8.0.1"):match("time=([%d%.]+)")
        vpn_ms = tonumber(m) or 80
    elseif vpn.wg then
        local m = B.sh("ping -c1 -W1 10.66.66.1"):match("time=([%d%.]+)")
        vpn_ms = tonumber(m) or 50
    end
    local d = {lte = sig.rsrp ~= 0 and sig.rsrp or sig.rssi, vpn = vpn_ms, clients = {}}
    for _, c in ipairs(cl) do
        d.clients[#d.clients + 1] = {kbps = c.tx_mbps * 100, signal = c.signal}
    end
    B.dashboard(d)
end

function M.on_tap(x, y) return "menu" end  -- any tap → menu

return M
