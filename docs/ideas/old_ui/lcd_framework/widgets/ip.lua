-- widgets/ip.lua — IP addresses
local B = require("lcd.base")
local M = {title = "IP"}

function M.draw()
    B.clear()
    B.header("IP Addresses", B.YELLOW)

    local lan = B.iface_ip("br-lan")
    local wan = B.iface_ip("wwan0")
    local tun = B.iface_ip("tun0")
    local wg = B.iface_ip("wg0")
    local ext = B.sh("wget -qO- ifconfig.me"):gsub("%s", "")

    local y = 30
    B.text(10, y, "LAN:", B.GRAY, 1); B.text(80, y, lan, B.WHITE, 2); y = y + 22
    B.text(10, y, "WAN:", B.GRAY, 1); B.text(80, y, wan ~= "" and wan or "---", B.WHITE, 2); y = y + 22
    if tun ~= "" then
        B.text(10, y, "OVPN:", B.GRAY, 1); B.text(80, y, tun, B.GREEN, 2); y = y + 22
    end
    if wg ~= "" then
        B.text(10, y, "WG:", B.GRAY, 1); B.text(80, y, wg, B.GREEN, 2); y = y + 22
    end
    y = y + 10
    B.text(10, y, "External:", B.GRAY, 1); y = y + 14
    B.text(10, y, ext ~= "" and ext or "checking...", B.CYAN, 2)

    B.status_bar("Tap to go back")
    B.flush()
end

function M.on_tap(x, y) return "menu" end

return M
