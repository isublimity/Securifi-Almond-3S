-- widgets/info.lua — System information
local B = require("lcd.base")
local M = {title = "Info"}

function M.draw()
    B.clear()
    B.header("System Info", B.GRAY)

    local kern = B.sh("uname -r"):gsub("\n", "")
    local up = B.uptime()
    local mem = B.sh("free | grep Mem")
    local total = mem:match("(%d+)") or "?"
    local used = mem:match("%d+%s+(%d+)") or "?"
    local load = B.sh("cat /proc/loadavg"):match("([%d%.]+%s[%d%.]+%s[%d%.]+)") or "?"
    local cl = B.wifi_clients()

    local y = 28
    B.text(10, y, "Kernel: " .. kern, B.WHITE, 1); y = y + 16
    B.text(10, y, "Uptime: " .. up, B.WHITE, 1); y = y + 16
    B.text(10, y, "Load: " .. load, B.WHITE, 1); y = y + 16
    B.text(10, y, "RAM: " .. B.fmt_bytes(tonumber(used)*1024) .. " / " .. B.fmt_bytes(tonumber(total)*1024), B.WHITE, 1); y = y + 16
    B.text(10, y, "WiFi clients: " .. #cl, B.WHITE, 1); y = y + 16

    -- Components
    y = y + 10
    B.text(10, y, "Components:", B.YELLOW, 1); y = y + 14
    local components = {"LCD+Touch", "LAN", "WiFi 2.4+5G", "LTE EC21", "WireGuard", "OpenVPN", "Buzzer"}
    for _, c in ipairs(components) do
        B.text(20, y, "+ " .. c, B.GREEN, 1); y = y + 12
        if y > 220 then break end
    end

    B.status_bar("Tap to go back")
    B.flush()
end

function M.on_tap(x, y) return "menu" end

return M
