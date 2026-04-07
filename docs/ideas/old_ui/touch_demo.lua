#!/usr/bin/lua
-- Touch demo: mmap framebuffer + direct rendering
-- No sockets, no lcd_render, no fork. Pure lcdlib.
-- flush() takes ~2s (GPIO bit-bang), so we batch updates

local lcd = require("lcdlib")

if not lcd.open() then
    print("ERROR: cannot open /dev/lcd (mmap)")
    os.exit(1)
end

-- Colors (RGB565)
local BLACK  = 0x0000
local WHITE  = 0xFFFF
local RED    = 0xF800
local GREEN  = 0x07E0
local BLUE   = 0x001F
local YELLOW = 0xFFE0
local CYAN   = 0x07FF

local count = 0
local dirty = false
local last_flush = 0

-- Draw initial screen
lcd.clear(BLACK)
lcd.text(30, 10, "TOUCH DEMO", YELLOW, 3)
lcd.text(20, 45, "Tap anywhere!", WHITE, 2)
lcd.text(10, 220, "LCD+Touch+LAN", GREEN, 1)
lcd.flush()

print("Touch demo running. Ctrl+C to exit.")

local was_pressed = false
local taps = {}  -- accumulate taps between flushes

while true do
    local x, y, pressed = lcd.touch()

    if pressed == 1 and not was_pressed then
        count = count + 1
        taps[#taps + 1] = {x = x, y = y, n = count}
        dirty = true
        print(string.format("TAP #%d x=%d y=%d", count, x, y))
    end

    was_pressed = (pressed == 1)

    -- Flush only when dirty AND no finger down (avoid blocking during drag)
    if dirty and pressed ~= 1 then
        -- Draw all accumulated taps
        lcd.rect(0, 65, 320, 150, BLACK)
        for _, t in ipairs(taps) do
            -- Crosshair
            lcd.rect(t.x - 15, t.y, 31, 1, RED)
            lcd.rect(t.x, t.y - 15, 1, 31, RED)
            -- Dot
            lcd.rect(t.x-2, t.y-2, 5, 5, CYAN)
            -- Brackets
            lcd.rect(t.x-10, t.y-10, 8, 1, GREEN)
            lcd.rect(t.x+3,  t.y-10, 8, 1, GREEN)
            lcd.rect(t.x-10, t.y+10, 8, 1, GREEN)
            lcd.rect(t.x+3,  t.y+10, 8, 1, GREEN)
            lcd.rect(t.x-10, t.y-10, 1, 8, GREEN)
            lcd.rect(t.x+10, t.y-10, 1, 8, GREEN)
            lcd.rect(t.x-10, t.y+3,  1, 8, GREEN)
            lcd.rect(t.x+10, t.y+3,  1, 8, GREEN)
        end
        -- Last tap coordinates
        local last = taps[#taps]
        lcd.rect(0, 200, 320, 40, BLACK)
        lcd.text(40, 205, string.format("X=%d Y=%d  #%d", last.x, last.y, last.n), WHITE, 2)
        lcd.flush()
        dirty = false
        -- Keep only last 5 taps for display
        if #taps > 5 then
            local new = {}
            for i = #taps - 4, #taps do new[#new+1] = taps[i] end
            taps = new
        end
    end

    lcd.usleep(30000)
end
