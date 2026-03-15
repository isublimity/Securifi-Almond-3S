#!/usr/bin/lua
--
-- lcd_ui.lua — Скрипт-движок для LCD дисплея Almond 3S
--
-- Сканирует /etc/lcd_scripts/*.lua, показывает как кнопки на дисплее.
-- Тач нажатие на кнопку → вызывает action()
-- Периодически вызывает status() для обновления текста
--
-- Отправляет JSON команды в lcd_render через pipe (echo | nc -U)
-- Читает тач через /tmp/lcd_touch_read
--
-- Без зависимостей кроме стандартного Lua 5.1
--

-- LCD dimensions
local LCD_W = 320
local LCD_H = 240

-- Colors
local C_BG     = "#000000"
local C_HEADER = "#001F"
local C_WHITE  = "white"
local C_GREEN  = "#07E0"
local C_RED    = "red"
local C_YELLOW = "yellow"
local C_CYAN   = "cyan"
local C_GRAY   = "#4208"
local C_BTN    = "#1082"

local SOCK = "/tmp/lcd.sock"

-- === LCD commands via nc -U ===

local function lcd_send(json_str)
    os.execute("echo '" .. json_str .. "' | socat - UNIX-CONNECT:" .. SOCK .. " 2>/dev/null")
end

local function lcd_clear(color)
    lcd_send(string.format('{"cmd":"clear","color":"%s"}', color or C_BG))
end

local function lcd_text(x, y, text, color, bg, size)
    lcd_send(string.format(
        '{"cmd":"text","x":%d,"y":%d,"text":"%s","color":"%s","bg":"%s","size":%d}',
        x, y, text, color or C_WHITE, bg or C_BG, size or 2
    ))
end

local function lcd_rect(x, y, w, h, color)
    lcd_send(string.format(
        '{"cmd":"rect","x":%d,"y":%d,"w":%d,"h":%d,"color":"%s"}',
        x, y, w, h, color
    ))
end

-- === Touch reader ===

local function read_touch()
    local p = io.popen("/tmp/lcd_touch_read 2>/dev/null", "r")
    if not p then return nil end
    local line = p:read("*l")
    p:close()
    if not line then return nil end
    local x, y, pressed = line:match("(%d+) (%d+) (%d+)")
    if pressed == "1" then
        return tonumber(x), tonumber(y)
    end
    return nil
end

-- === Script loader ===

local SCRIPTS_DIR = "/etc/lcd_scripts"
local scripts = {}

local function load_scripts()
    scripts = {}
    local p = io.popen("ls " .. SCRIPTS_DIR .. "/*.lua 2>/dev/null")
    if not p then return end
    for path in p:lines() do
        local ok, s = pcall(dofile, path)
        if ok and s and s.name then
            table.insert(scripts, s)
        else
            io.stderr:write("lcd_ui: failed to load " .. path .. "\n")
        end
    end
    p:close()
    table.sort(scripts, function(a, b) return (a.order or 99) < (b.order or 99) end)
end

-- === UI Layout ===

local COLS = 2
local BTN_W = 150
local BTN_H = 50
local BTN_PAD = 8
local HEADER_H = 28
local START_Y = HEADER_H + 6

local function btn_rect(idx)
    local col = (idx - 1) % COLS
    local row = math.floor((idx - 1) / COLS)
    local x = BTN_PAD + col * (BTN_W + BTN_PAD)
    local y = START_Y + row * (BTN_H + BTN_PAD)
    return x, y, BTN_W, BTN_H
end

local function draw_header()
    lcd_rect(0, 0, LCD_W, HEADER_H, C_HEADER)
    lcd_text(8, 6, "Almond 3S", C_WHITE, C_HEADER, 2)
    local t = os.date("%H:%M")
    lcd_text(LCD_W - 70, 6, t, C_CYAN, C_HEADER, 2)
end

local function draw_button(idx, script)
    local x, y, w, h = btn_rect(idx)
    local status_text = ""
    local status_color = C_GRAY

    if script.status then
        local ok, st = pcall(script.status)
        if ok and st then
            status_text = st.text or ""
            status_color = st.color or C_GREEN
        end
    end

    lcd_rect(x, y, w, h, C_BTN)
    lcd_text(x + 4, y + 4, script.name or "?", script.color or C_WHITE, C_BTN, 2)
    if status_text ~= "" then
        lcd_text(x + 4, y + 28, status_text, status_color, C_BTN, 1)
    end
end

local function draw_all()
    lcd_clear(C_BG)
    draw_header()
    for i, s in ipairs(scripts) do
        draw_button(i, s)
    end
end

local function handle_touch(tx, ty)
    for i, s in ipairs(scripts) do
        local x, y, w, h = btn_rect(i)
        if tx >= x and tx <= x + w and ty >= y and ty <= y + h then
            -- Flash button white
            lcd_rect(x, y, w, h, C_WHITE)
            lcd_text(x + 4, y + 4, s.name, C_BG, C_WHITE, 2)
            lcd_text(x + 4, y + 28, "...", C_GRAY, C_WHITE, 1)

            if s.action then
                pcall(s.action)
            end

            os.execute("sleep 1")
            draw_all()
            return true
        end
    end
    return false
end

-- === Main loop ===

local function sleep_ms(ms)
    -- Busy-wait for sub-second timing (no luasocket)
    -- Use usleep via small C helper or just os.execute
    if ms >= 1000 then
        os.execute("sleep " .. math.floor(ms / 1000))
    else
        os.execute("usleep " .. (ms * 1000) .. " 2>/dev/null")
    end
end

local function main()
    print("lcd_ui: starting")
    load_scripts()
    print("lcd_ui: loaded " .. #scripts .. " scripts")

    -- Wait for lcd_render socket
    for i = 1, 10 do
        if os.execute("test -S " .. SOCK) == 0 then break end
        print("lcd_ui: waiting for " .. SOCK)
        os.execute("sleep 1")
    end

    draw_all()
    print("lcd_ui: UI drawn, entering main loop")

    local last_draw = os.time()
    local touch_cooldown = 0

    while true do
        -- Check touch
        local tx, ty = read_touch()
        if tx and os.time() > touch_cooldown then
            if handle_touch(tx, ty) then
                touch_cooldown = os.time() + 2
            end
        end

        -- Refresh every 10 seconds
        if os.time() - last_draw >= 10 then
            draw_all()
            last_draw = os.time()
        end

        sleep_ms(200)
    end
end

main()
