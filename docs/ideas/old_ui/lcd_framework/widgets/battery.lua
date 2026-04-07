-- widgets/battery.lua — Battery status
local B = require("lcd.base")
local M = {title = "Battery"}

function M.draw()
    local bat = B.battery()

    B.clear()
    B.header("Battery", B.YELLOW)

    local y = 30
    if bat.present then
        B.text(10, y, "Battery: CONNECTED", B.GREEN, 2); y = y + 24

        -- Battery icon
        B.rect(120, y, 80, 40, B.WHITE)
        B.rect(200, y + 10, 8, 20, B.WHITE)
        local fill_w = math.max(2, math.floor(bat.percent * 76 / 100))
        local fill_c = bat.percent > 60 and B.GREEN or bat.percent > 20 and B.YELLOW or B.RED
        B.rect(122, y + 2, fill_w, 36, fill_c)
        B.text(135, y + 12, bat.percent .. "%", B.BLACK, 2)
        y = y + 50

        B.text(10, y, "Raw data: " .. string.format("0x%04X", bat.raw), B.GRAY, 1); y = y + 16
        B.text(10, y, "Status: present, monitoring", B.WHITE, 1); y = y + 16

        -- Raw PIC bytes
        local raw_str = B.sh("dmesg | grep 'PIC raw' | tail -1"):match("%[17%]: (.+)")
        if raw_str then
            B.text(10, y, "PIC: " .. raw_str:sub(1, 40), B.GRAY, 1); y = y + 16
        end
    else
        B.text(10, y, "Battery: NOT CONNECTED", B.RED, 2); y = y + 24
        B.text(10, y, "Running on USB power", B.GRAY, 1); y = y + 16

        -- Empty battery icon
        B.rect(120, y, 80, 40, B.DKGRAY)
        B.rect(200, y + 10, 8, 20, B.DKGRAY)
        B.text(140, y + 12, "---", B.GRAY, 2)
    end

    B.status_bar("Tap to go back")
    B.flush()
end

function M.on_tap(x, y) return "menu" end

return M
