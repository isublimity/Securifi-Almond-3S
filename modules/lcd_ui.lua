#!/usr/bin/lua
--
-- lcd_ui.lua — Скрипт-движок для LCD дисплея Almond 3S
--
-- Буферный режим: все команды копятся, отправляются одним socat.
-- Один fork вместо десятков = быстрая отрисовка.
--

local LCD_W = 320
local LCD_H = 240

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

-- === Buffered LCD commands ===

local cmd_buf = {}

local function lcd_queue(json_str)
    cmd_buf[#cmd_buf + 1] = json_str
end

local function lcd_flush()
    if #cmd_buf == 0 then return end
    local all = table.concat(cmd_buf, "\n")
    cmd_buf = {}
    -- Write to temp file, pipe to socat (one fork for all commands)
    local f = io.open("/tmp/.lcd_cmds", "w")
    if f then
        f:write(all .. "\n")
        f:close()
        os.execute("socat -u FILE:/tmp/.lcd_cmds UNIX-CONNECT:" .. SOCK .. " 2>/dev/null")
    end
end

local function lcd_clear(color)
    lcd_queue(string.format('{"cmd":"clear","color":"%s"}', color or C_BG))
end

local function lcd_text(x, y, text, color, bg, size)
    lcd_queue(string.format(
        '{"cmd":"text","x":%d,"y":%d,"text":"%s","color":"%s","bg":"%s","size":%d}',
        x, y, text, color or C_WHITE, bg or C_BG, size or 2
    ))
end

local function lcd_rect(x, y, w, h, color)
    lcd_queue(string.format(
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

-- === Graph drawing ===

-- Draw a line graph: data = array of values, fills area between graph_y+graph_h and line
local function draw_graph(x, y, w, h, data, color, max_val)
    if #data == 0 then return end
    if not max_val or max_val == 0 then
        max_val = 1
        for _, v in ipairs(data) do
            if v > max_val then max_val = v end
        end
    end
    local bar_w = math.max(1, math.floor(w / #data))
    for i, v in ipairs(data) do
        local bar_h = math.floor(v / max_val * h)
        if bar_h < 1 then bar_h = 1 end
        local bx = x + (i - 1) * bar_w
        lcd_rect(bx, y + h - bar_h, bar_w, bar_h, color)
    end
end

-- === Network stats reader ===

local net_history = {}  -- {iface = {rx={}, tx={}}}
local HISTORY_LEN = 40

local function read_net_stats()
    local f = io.open("/proc/net/dev", "r")
    if not f then return end
    local now = {}
    for line in f:lines() do
        local iface, rx, tx = line:match("^%s*(%S+):%s*(%d+)%s+%d+%s+%d+%s+%d+%s+%d+%s+%d+%s+%d+%s+%d+%s+(%d+)")
        if iface and iface ~= "lo" then
            now[iface] = {rx = tonumber(rx), tx = tonumber(tx)}
        end
    end
    f:close()
    return now
end

local last_net = nil

local function update_net_history()
    local cur = read_net_stats()
    if not cur then return end
    if last_net then
        for iface, vals in pairs(cur) do
            if last_net[iface] then
                if not net_history[iface] then
                    net_history[iface] = {rx = {}, tx = {}}
                end
                local drx = vals.rx - last_net[iface].rx
                local dtx = vals.tx - last_net[iface].tx
                if drx < 0 then drx = 0 end
                if dtx < 0 then dtx = 0 end
                local h = net_history[iface]
                h.rx[#h.rx + 1] = drx
                h.tx[#h.tx + 1] = dtx
                if #h.rx > HISTORY_LEN then table.remove(h.rx, 1) end
                if #h.tx > HISTORY_LEN then table.remove(h.tx, 1) end
            end
        end
    end
    last_net = cur
end

-- === Full-screen graph page ===

local graph_mode = false

local function fmt_bytes(b)
    if b > 1048576 then return string.format("%.1fM", b/1048576) end
    if b > 1024 then return string.format("%.0fK", b/1024) end
    return tostring(b)
end

local function draw_graph_page()
    lcd_clear(C_BG)

    -- Header
    lcd_rect(0, 0, LCD_W, 18, C_HEADER)
    lcd_text(4, 1, "Traffic", C_WHITE, C_HEADER, 2)
    lcd_text(200, 1, "tap = exit", C_GRAY, C_HEADER, 1)

    -- Graph area starts below header
    local y_pos = 22
    local ifaces = {"wwan0", "br-lan"}
    local gh = 42  -- graph height per interface

    for _, iface in ipairs(ifaces) do
        local h = net_history[iface]
        if h and #h.rx > 1 then
            local max_val = 1
            for _, v in ipairs(h.rx) do if v > max_val then max_val = v end end
            for _, v in ipairs(h.tx) do if v > max_val then max_val = v end end

            local rx_last = h.rx[#h.rx] or 0
            local tx_last = h.tx[#h.tx] or 0

            -- Label row
            lcd_text(4, y_pos, iface, C_WHITE, C_BG, 1)
            lcd_text(80, y_pos, "RX:" .. fmt_bytes(rx_last), C_GREEN, C_BG, 1)
            lcd_text(180, y_pos, "TX:" .. fmt_bytes(tx_last), C_RED, C_BG, 1)
            lcd_text(270, y_pos, fmt_bytes(max_val) .. "/s", C_GRAY, C_BG, 1)
            y_pos = y_pos + 10

            -- Graph background + bars
            local gw = LCD_W - 8
            lcd_rect(4, y_pos, gw, gh, "#0841")
            draw_graph(4, y_pos, gw, gh, h.rx, C_GREEN, max_val)
            draw_graph(4, y_pos, gw, gh, h.tx, C_RED, max_val)

            y_pos = y_pos + gh + 8
        else
            lcd_text(4, y_pos, iface .. ": waiting...", C_GRAY, C_BG, 1)
            y_pos = y_pos + 16
        end
    end

    lcd_flush()
end

-- === Button UI ===

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

local function draw_buttons()
    lcd_clear(C_BG)
    draw_header()
    for i, s in ipairs(scripts) do
        draw_button(i, s)
    end
    lcd_flush()
end

local function handle_touch(tx, ty)
    if graph_mode then
        -- Any touch exits graph mode
        graph_mode = false
        draw_buttons()
        return true
    end

    for i, s in ipairs(scripts) do
        local x, y, w, h = btn_rect(i)
        if tx >= x and tx <= x + w and ty >= y and ty <= y + h then
            -- Flash button
            lcd_rect(x, y, w, h, C_WHITE)
            lcd_text(x + 4, y + 4, s.name, C_BG, C_WHITE, 2)
            lcd_text(x + 4, y + 28, "...", C_GRAY, C_WHITE, 1)
            lcd_flush()

            if s.action then
                pcall(s.action)
            end

            -- Check if script wants graph mode
            if s.graph then
                graph_mode = true
                return true
            end

            os.execute("sleep 1")
            draw_buttons()
            return true
        end
    end
    return false
end

-- === Main loop ===

local function main()
    print("lcd_ui: starting")
    load_scripts()
    print("lcd_ui: loaded " .. #scripts .. " scripts")

    for i = 1, 10 do
        if os.execute("test -S " .. SOCK) == 0 then break end
        print("lcd_ui: waiting for " .. SOCK)
        os.execute("sleep 1")
    end

    draw_buttons()
    print("lcd_ui: UI drawn, entering main loop")

    local last_draw = os.time()
    local touch_cooldown = 0
    -- Seed net history with first reading
    update_net_history()

    while true do
        local tx, ty = read_touch()
        if tx and os.time() > touch_cooldown then
            if handle_touch(tx, ty) then
                touch_cooldown = os.time() + 2
            end
        end

        -- Update network stats every cycle
        update_net_history()

        if graph_mode then
            -- Fast refresh in graph mode
            draw_graph_page()
            os.execute("usleep 500000 2>/dev/null || sleep 1")
        else
            -- Normal refresh every 10 seconds
            if os.time() - last_draw >= 10 then
                draw_buttons()
                last_draw = os.time()
            end
            os.execute("usleep 200000 2>/dev/null || sleep 1")
        end
    end
end

main()
