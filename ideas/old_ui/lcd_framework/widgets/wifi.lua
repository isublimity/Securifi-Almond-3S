-- widgets/wifi.lua — WiFi clients list
local B = require("lcd.base")
local M = {title = "WiFi"}

function M.draw()
    local cl = B.wifi_clients()

    B.clear()
    B.header("WiFi Clients: " .. #cl, B.BLUE)

    if #cl == 0 then
        B.text(40, 100, "No clients", B.GRAY, 2)
    else
        for i, c in ipairs(cl) do
            if i > 8 then break end
            local y = 24 + (i - 1) * 25
            local sig_color = c.signal > -50 and B.GREEN or c.signal > -70 and B.YELLOW or B.RED
            -- Signal bar
            local bar_w = math.max(2, math.min(80, (c.signal + 90) * 2))
            B.rect(10, y + 2, bar_w, 12, sig_color)
            -- Info
            local short_mac = c.mac:sub(-8)
            B.text(100, y, short_mac, B.WHITE, 1)
            B.text(170, y, c.signal .. "dBm", sig_color, 1)
            B.text(230, y, c.band, B.CYAN, 1)
            B.text(260, y, string.format("%.0fM", c.tx_mbps), B.GRAY, 1)
        end
    end

    B.status_bar("Tap to go back")
    B.flush()
end

function M.on_tap(x, y) return "menu" end

return M
