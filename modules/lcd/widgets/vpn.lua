-- widgets/vpn.lua — VPN status and controls
local B = require("lcd.base")
local M = {title = "VPN"}

local ping_hist = B.history_new(60)

function M.draw()
    local vpn = B.vpn_status()

    B.clear()
    B.header("VPN", B.GREEN)

    local y = 28
    if vpn.ovpn then
        local tun_ip = B.iface_ip("tun0")
        B.text(10, y, "OpenVPN: ACTIVE", B.GREEN, 2); y = y + 20
        B.text(10, y, "TUN: " .. tun_ip, B.WHITE, 1); y = y + 14
        local m = B.sh("ping -c1 -W1 10.8.0.1"):match("time=([%d%.]+)")
        local ms = tonumber(m) or 0
        B.history_push(ping_hist, ms)
        B.text(10, y, "Latency: " .. ms .. " ms", ms < 100 and B.GREEN or B.YELLOW, 1); y = y + 14
    else
        B.text(10, y, "OpenVPN: OFF", B.RED, 2); y = y + 20
    end

    if vpn.wg then
        B.text(10, y, "WireGuard: ACTIVE", B.GREEN, 2); y = y + 20
        local wg_ip = B.iface_ip("wg0")
        B.text(10, y, "WG: " .. wg_ip, B.WHITE, 1); y = y + 14
    else
        B.text(10, y, "WireGuard: OFF", B.GRAY, 1); y = y + 14
    end

    B.text(10, y, "Exit IP: " .. (vpn.ip ~= "" and vpn.ip or "checking..."), B.CYAN, 1)
    y = y + 20

    -- Latency graph
    if #ping_hist.data > 1 then
        B.text(10, y, "VPN Latency (ms):", B.GRAY, 1); y = y + 14
        B.graph_line(ping_hist.data, 10, y, 300, 60, B.GREEN)
    end

    B.status_bar("Tap to go back")
    B.flush()
end

function M.on_tap(x, y) return "menu" end

return M
