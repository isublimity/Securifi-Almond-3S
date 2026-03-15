#!/usr/bin/lua
--
-- lcd_ui.lua — Скрипт-движок для LCD дисплея Almond 3S
--
-- Сканирует /etc/lcd_scripts/*.lua, показывает как кнопки на дисплее.
-- Каждый скрипт — таблица: {name, icon, color, action(), status()}
-- Тач нажатие на кнопку → вызывает action()
-- Периодически вызывает status() для обновления текста
--
-- Использует lcd_render через unix socket /tmp/lcd.sock
-- Использует /dev/lcd ioctl(1) для чтения тача
--

local socket = require("socket")
local unix = require("socket.unix")

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

-- === LCD Render Socket ===

local sock = nil

local function lcd_connect()
    if sock then pcall(function() sock:close() end) end
    sock = unix()
    local ok, err = sock:connect("/tmp/lcd.sock")
    if not ok then
        sock = nil
        return false
    end
    return true
end

local function lcd_send(json_str)
    if not sock then
        if not lcd_connect() then return false end
    end
    local ok, err = sock:send(json_str .. "\n")
    if not ok then
        -- Reconnect and retry
        if lcd_connect() then
            sock:send(json_str .. "\n")
        end
    end
    return true
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

-- === Touch reader via ioctl ===

local ffi_ok, ffi = pcall(require, "ffi")

local function read_touch()
    -- Read touch via /dev/lcd ioctl cmd=1
    local f = io.open("/dev/lcd", "rb")
    if not f then return nil end

    -- Use C ioctl: we need to call ioctl(fd, 1, &data)
    -- For Lua without FFI, use a helper binary
    f:close()

    -- Fallback: use pic_test-style helper or direct read
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

-- Grid: 2 columns, rows adapt to script count
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

    -- Time
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

    -- Button background
    lcd_rect(x, y, w, h, "#1082")

    -- Name
    lcd_text(x + 4, y + 4, script.name or "?", script.color or C_WHITE, "#1082", 2)

    -- Status line
    if status_text ~= "" then
        lcd_text(x + 4, y + 28, status_text, status_color, "#1082", 1)
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
            -- Flash button
            lcd_rect(x, y, w, h, C_WHITE)
            lcd_text(x + 4, y + 4, s.name, C_BG, C_WHITE, 2)

            if s.action then
                pcall(s.action)
            end

            -- Redraw after 500ms
            socket.sleep(0.5)
            draw_all()
            return
        end
    end
end

-- === Main loop ===

local function main()
    print("lcd_ui: starting")
    load_scripts()
    print("lcd_ui: loaded " .. #scripts .. " scripts")

    if not lcd_connect() then
        io.stderr:write("lcd_ui: cannot connect to /tmp/lcd.sock, waiting...\n")
        for i = 1, 30 do
            socket.sleep(1)
            if lcd_connect() then break end
        end
    end

    draw_all()

    local last_draw = os.time()
    local touch_cooldown = 0

    while true do
        -- Check touch
        local tx, ty = read_touch()
        if tx and os.time() > touch_cooldown then
            handle_touch(tx, ty)
            touch_cooldown = os.time() + 1  -- 1 sec cooldown
        end

        -- Refresh every 10 seconds
        if os.time() - last_draw >= 10 then
            draw_all()
            last_draw = os.time()
        end

        socket.sleep(0.1)
    end
end

main()
