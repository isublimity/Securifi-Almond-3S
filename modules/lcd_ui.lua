#!/usr/bin/lua
--
-- lcd_ui.lua — LCD UI engine for Almond 3S
--
-- Architecture:
--   Fast loop: read touch (1ms) → handle tap → sleep 200ms
--   Slow timer: collect status data (uqmi, wg, net) every 5 sec
--   Render: only when data changed or touch event, all buffered
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
local HISTORY_LEN = 60

-- === Buffered LCD ===

local cmd_buf = {}

local function Q(json_str) cmd_buf[#cmd_buf + 1] = json_str end

local function lcd_flush()
    if #cmd_buf == 0 then return end
    local f = io.open("/tmp/.lcd_cmds", "w")
    if f then
        f:write(table.concat(cmd_buf, "\n") .. "\n")
        f:close()
        os.execute("socat -u FILE:/tmp/.lcd_cmds UNIX-CONNECT:" .. SOCK .. " 2>/dev/null")
    end
    cmd_buf = {}
end

local function lcd_clear(c) Q(string.format('{"cmd":"clear","color":"%s"}', c or C_BG)) end
local function lcd_rect(x,y,w,h,c) Q(string.format('{"cmd":"rect","x":%d,"y":%d,"w":%d,"h":%d,"color":"%s"}',x,y,w,h,c)) end
local function lcd_text(x,y,t,c,bg,s) Q(string.format('{"cmd":"text","x":%d,"y":%d,"text":"%s","color":"%s","bg":"%s","size":%d}',x,y,t,c or C_WHITE,bg or C_BG,s or 2)) end

-- === Touch + sleep via lcdlib.so (zero fork, native ioctl) ===

local lcd = require("lcdlib")
lcd.open()

local function read_touch()
    local x, y, pressed = lcd.touch()
    if pressed == 1 then return x, y end
    return nil
end

local function sleep_ms(ms)
    lcd.usleep(ms * 1000)
end

-- === Data collection (cached, updated every N sec) ===

local cache = {
    wg_on = false, ovpn_on = false,
    rsrp = 0, rsrq = 0, sinr = 0, rssi = 0,
    rx_speed = 0, tx_speed = 0,  -- bytes/sec on wwan0
}

-- History arrays
local hist = {
    rsrp = {}, rsrq = {}, sinr = {},
    rx = {}, tx = {},           -- wwan0 traffic
    br_rx = {}, br_tx = {},     -- br-lan traffic
}

local last_net = nil

-- Background data collection: kick off async, read results from temp files
local function kick_bg_collect()
    os.execute("wg show wgvpn 2>/dev/null | head -1 > /tmp/.lcd_wg &")
    os.execute("pgrep -x openvpn > /tmp/.lcd_ovpn 2>/dev/null &")
    os.execute("uqmi -d /dev/cdc-wdm0 --get-signal-info > /tmp/.lcd_sig 2>/dev/null &")
end

local function read_file(path)
    local f = io.open(path)
    if not f then return "" end
    local s = f:read("*a") or ""; f:close(); return s
end

local function collect_cached()
    -- VPN status (from bg files, instant read)
    cache.wg_on = read_file("/tmp/.lcd_wg") ~= ""
    cache.ovpn_on = read_file("/tmp/.lcd_ovpn") ~= ""

    -- Signal (from bg file)
    local raw = read_file("/tmp/.lcd_sig")
    cache.rsrp = tonumber(raw:match('"rsrp":%s*(%-?%d+)')) or cache.rsrp
    cache.rsrq = tonumber(raw:match('"rsrq":%s*(%-?%d+)')) or cache.rsrq
    cache.sinr = tonumber(raw:match('"snr":%s*(%-?[%d%.]+)')) or cache.sinr
    cache.rssi = tonumber(raw:match('"rssi":%s*(%-?%d+)')) or cache.rssi

    local function push(arr, v) arr[#arr+1] = v; if #arr > HISTORY_LEN then table.remove(arr,1) end end
    push(hist.rsrp, cache.rsrp)
    push(hist.sinr, cache.sinr)

    -- Traffic (direct read from /proc, instant)
    local f = io.open("/proc/net/dev")
    if not f then return end
    local now = {}
    for line in f:lines() do
        local iface, rx, tx = line:match("^%s*(%S+):%s*(%d+)%s+%d+%s+%d+%s+%d+%s+%d+%s+%d+%s+%d+%s+%d+%s+(%d+)")
        if iface then now[iface] = {rx=tonumber(rx), tx=tonumber(tx)} end
    end
    f:close()

    if last_net then
        local function delta(iface, key)
            if now[iface] and last_net[iface] then
                local d = now[iface][key] - last_net[iface][key]
                return d >= 0 and d or 0
            end
            return 0
        end
        cache.rx_speed = delta("wwan0", "rx")
        cache.tx_speed = delta("wwan0", "tx")
        push(hist.rx, cache.rx_speed)
        push(hist.tx, cache.tx_speed)
        push(hist.br_rx, delta("br-lan", "rx"))
        push(hist.br_tx, delta("br-lan", "tx"))
    end
    last_net = now
end

-- === Helpers ===

local function fmt(b)
    if b > 1048576 then return string.format("%.1fM", b/1048576) end
    if b > 1024 then return string.format("%.0fK", b/1024) end
    return tostring(b)
end

local function draw_bars(x, y, w, h, data, color, min_val, max_val)
    if #data < 2 then return end
    if not max_val or max_val <= min_val then max_val = min_val + 1 end
    local bw = math.max(1, math.floor(w / HISTORY_LEN))
    local start = math.max(1, #data - HISTORY_LEN + 1)
    for i = start, #data do
        local v = data[i] - min_val
        local range = max_val - min_val
        local bh = math.max(1, math.floor(v / range * h))
        local bx = x + (i - start) * bw
        if bx + bw <= x + w then
            lcd_rect(bx, y + h - bh, bw, bh, color)
        end
    end
end

-- === Pages ===

local page = "main"  -- "main", "signal", "traffic"

-- Button layout
local COLS = 2
local BTN_W = 150
local BTN_H = 55
local BTN_PAD = 8
local HDR_H = 24
local START_Y = HDR_H + 4

local function btn_pos(idx)
    local col = (idx-1) % COLS
    local row = math.floor((idx-1) / COLS)
    return BTN_PAD + col*(BTN_W+BTN_PAD), START_Y + row*(BTN_H+BTN_PAD), BTN_W, BTN_H
end

local function draw_main()
    lcd_clear(C_BG)
    lcd_rect(0, 0, LCD_W, HDR_H, C_HEADER)
    lcd_text(6, 4, "Almond 3S", C_WHITE, C_HEADER, 2)
    lcd_text(LCD_W-70, 4, os.date("%H:%M"), C_CYAN, C_HEADER, 2)

    -- Button 1: WireGuard
    local x,y,w,h = btn_pos(1)
    lcd_rect(x,y,w,h, C_BTN)
    lcd_text(x+4, y+4, "WireGuard", C_GREEN, C_BTN, 2)
    lcd_text(x+4, y+30, cache.wg_on and "ON" or "OFF", cache.wg_on and C_GREEN or C_RED, C_BTN, 2)

    -- Button 2: OpenVPN
    x,y,w,h = btn_pos(2)
    lcd_rect(x,y,w,h, C_BTN)
    lcd_text(x+4, y+4, "OpenVPN", C_YELLOW, C_BTN, 2)
    lcd_text(x+4, y+30, cache.ovpn_on and "ON" or "OFF", cache.ovpn_on and C_GREEN or C_RED, C_BTN, 2)

    -- Button 3: LTE Signal
    x,y,w,h = btn_pos(3)
    lcd_rect(x,y,w,h, C_BTN)
    lcd_text(x+4, y+4, "LTE", C_CYAN, C_BTN, 2)
    local sig_color = cache.rsrp >= -90 and C_GREEN or (cache.rsrp >= -110 and C_YELLOW or C_RED)
    if cache.rsrp ~= 0 then
        lcd_text(x+4, y+24, "RSRP:" .. cache.rsrp, sig_color, C_BTN, 1)
        lcd_text(x+4, y+34, "SINR:" .. string.format("%.0f", cache.sinr), C_GRAY, C_BTN, 1)
    else
        lcd_text(x+4, y+30, "no signal", C_RED, C_BTN, 1)
    end

    -- Button 4: Traffic
    x,y,w,h = btn_pos(4)
    lcd_rect(x,y,w,h, C_BTN)
    lcd_text(x+4, y+4, "Traffic", C_WHITE, C_BTN, 2)
    lcd_text(x+4, y+24, "RX:" .. fmt(cache.rx_speed) .. "/s", C_GREEN, C_BTN, 1)
    lcd_text(x+4, y+36, "TX:" .. fmt(cache.tx_speed) .. "/s", C_RED, C_BTN, 1)

    lcd_flush()
end

local function draw_signal_page()
    lcd_clear(C_BG)
    lcd_rect(0, 0, LCD_W, 16, C_HEADER)
    lcd_text(4, 0, "LTE Signal", C_WHITE, C_HEADER, 2)
    lcd_text(240, 0, "tap=exit", C_GRAY, C_HEADER, 1)

    local y = 20

    -- RSRP graph
    lcd_text(4, y, "RSRP:" .. cache.rsrp .. "dBm", C_GREEN, C_BG, 1)
    y = y + 10
    lcd_rect(4, y, LCD_W-8, 45, "#0841")
    -- RSRP range: -140 to -44
    draw_bars(4, y, LCD_W-8, 45, hist.rsrp, C_GREEN, -140, -44)
    lcd_text(4, y, "-44", C_GRAY, "#0841", 1)
    lcd_text(4, y+37, "-140", C_GRAY, "#0841", 1)
    y = y + 50

    -- SINR graph
    lcd_text(4, y, "SINR:" .. string.format("%.1f", cache.sinr) .. "dB", C_CYAN, C_BG, 1)
    y = y + 10
    lcd_rect(4, y, LCD_W-8, 45, "#0841")
    -- SINR range: -20 to 30
    draw_bars(4, y, LCD_W-8, 45, hist.sinr, C_CYAN, -20, 30)
    lcd_text(4, y, "30", C_GRAY, "#0841", 1)
    lcd_text(4, y+37, "-20", C_GRAY, "#0841", 1)
    y = y + 50

    -- Current values summary
    lcd_text(4, y, string.format("RSRQ:%d  RSSI:%d", cache.rsrq, cache.rssi), C_GRAY, C_BG, 1)

    lcd_flush()
end

local function draw_traffic_page()
    lcd_clear(C_BG)
    lcd_rect(0, 0, LCD_W, 16, C_HEADER)
    lcd_text(4, 0, "Traffic", C_WHITE, C_HEADER, 2)
    lcd_text(240, 0, "tap=exit", C_GRAY, C_HEADER, 1)

    local y = 20
    local ifaces = {
        {name="wwan0", rx=hist.rx, tx=hist.tx},
        {name="br-lan", rx=hist.br_rx, tx=hist.br_tx},
    }

    for _, iface in ipairs(ifaces) do
        if #iface.rx > 1 then
            local max_val = 1
            for _,v in ipairs(iface.rx) do if v > max_val then max_val = v end end
            for _,v in ipairs(iface.tx) do if v > max_val then max_val = v end end

            local rx_last = iface.rx[#iface.rx] or 0
            local tx_last = iface.tx[#iface.tx] or 0

            lcd_text(4, y, iface.name, C_WHITE, C_BG, 1)
            lcd_text(70, y, "RX:"..fmt(rx_last).."/s", C_GREEN, C_BG, 1)
            lcd_text(170, y, "TX:"..fmt(tx_last).."/s", C_RED, C_BG, 1)
            lcd_text(270, y, fmt(max_val), C_GRAY, C_BG, 1)
            y = y + 10

            lcd_rect(4, y, LCD_W-8, 42, "#0841")
            draw_bars(4, y, LCD_W-8, 42, iface.rx, C_GREEN, 0, max_val)
            draw_bars(4, y, LCD_W-8, 42, iface.tx, C_RED, 0, max_val)
            y = y + 48
        else
            lcd_text(4, y, iface.name .. ": waiting...", C_GRAY, C_BG, 1)
            y = y + 14
        end
    end

    lcd_flush()
end

-- === Touch handling ===

local function handle_main_touch(tx, ty)
    for i = 1, 4 do
        local x,y,w,h = btn_pos(i)
        if tx >= x and tx <= x+w and ty >= y and ty <= y+h then
            -- Flash
            lcd_rect(x,y,w,h, C_WHITE)
            lcd_text(x+4, y+4, "...", C_BG, C_WHITE, 2)
            lcd_flush()

            if i == 1 then -- WireGuard toggle
                if cache.wg_on then
                    os.execute("(ifdown wgvpn; uci set network.wgvpn.disabled=1; uci commit network) &")
                else
                    os.execute("(uci set openvpn.sirius.enabled=0; uci commit openvpn; /etc/init.d/openvpn stop; uci set network.wgvpn.disabled=0; uci commit network; ifup wgvpn) >/dev/null 2>&1 &")
                end
                cache.wg_on = not cache.wg_on  -- optimistic update
                kick_bg_collect()
            elseif i == 2 then -- OpenVPN toggle
                if cache.ovpn_on then
                    os.execute("(uci set openvpn.sirius.enabled=0; uci commit openvpn; /etc/init.d/openvpn stop) >/dev/null 2>&1 &")
                else
                    os.execute("(ifdown wgvpn; uci set network.wgvpn.disabled=1; uci commit network; uci set openvpn.sirius.enabled=1; uci commit openvpn; /etc/init.d/openvpn restart) >/dev/null 2>&1 &")
                end
                cache.ovpn_on = not cache.ovpn_on  -- optimistic update
                kick_bg_collect()
            elseif i == 3 then -- LTE → signal page
                page = "signal"; return true
            elseif i == 4 then -- Traffic → traffic page
                page = "traffic"; return true
            end
            draw_main(); return true
        end
    end
    return false
end

-- === Screen state machine (burn-in protection) ===
--
-- States:
--   "active"      — UI visible, backlight ON, normal touch handling
--   "screensaver" — 4PDA logo, backlight ON, touch → active
--   "off"         — backlight OFF, touch → active
--
-- Transitions:
--   active → screensaver:  IDLE_TO_SAVER seconds without touch
--   screensaver → off:     IDLE_TO_OFF seconds without touch
--   screensaver → active:  touch
--   off → active:          touch
--

local IDLE_TO_SAVER = 30   -- секунд до скринсейвера
local IDLE_TO_OFF   = 30   -- секунд от скринсейвера до выключения экрана

local screen_state = "active"
local last_touch_time = os.time()

-- Показывает 4PDA лого (bitmap из ядра, мгновенно)
local function call_logo()
    lcd.splash()
end

local function set_screen_state(new_state)
    if new_state == screen_state then return end
    screen_state = new_state

    if new_state == "active" then
        lcd.backlight(true)
        page = "main"
        draw_main()
        print("lcd_ui: screen ON")
    elseif new_state == "screensaver" then
        lcd.backlight(true)
        call_logo()
        print("lcd_ui: screensaver")
    elseif new_state == "off" then
        lcd_clear(C_BG)
        lcd_flush()
        lcd.backlight(false)
        print("lcd_ui: screen OFF (burn-in protection)")
    end
end

local function on_touch()
    last_touch_time = os.time()
    if screen_state ~= "active" then
        set_screen_state("active")
        return true  -- consume touch (don't pass to UI)
    end
    return false
end

-- === Main ===

local function main()
    io.stdout:setvbuf("no")
    print("lcd_ui: starting")

    kick_bg_collect()
    sleep_ms(2000)
    collect_cached()
    draw_main()
    print("lcd_ui: ready (idle: " .. IDLE_TO_SAVER .. "s saver, " .. IDLE_TO_OFF .. "s off)")

    local last_bg_kick = os.time()
    local last_draw = os.time()
    local touch_cd = 0
    last_touch_time = os.time()

    while true do
        -- 1. Touch check
        local tx, ty = read_touch()
        if tx and os.time() > touch_cd then
            local consumed = on_touch()  -- wake from screensaver/off
            if not consumed and screen_state == "active" then
                if page == "main" then
                    handle_main_touch(tx, ty)
                else
                    page = "main"
                    draw_main()
                end
            end
            touch_cd = os.time() + 1
            last_draw = os.time()
        end

        local now = os.time()

        -- 2. Screen state transitions (burn-in protection)
        if screen_state == "active" then
            if now - last_touch_time >= IDLE_TO_SAVER then
                set_screen_state("screensaver")
            end
        elseif screen_state == "screensaver" then
            if now - last_touch_time >= IDLE_TO_SAVER + IDLE_TO_OFF then
                set_screen_state("off")
            end
        end

        -- 3. Background data collection (only when active)
        if screen_state == "active" then
            if now - last_bg_kick >= 3 then
                kick_bg_collect()
                last_bg_kick = now
            end
            collect_cached()

            -- 4. Redraw
            if page == "main" then
                if now - last_draw >= 5 then
                    draw_main()
                    last_draw = now
                end
            else
                if now - last_draw >= 1 then
                    if page == "signal" then draw_signal_page()
                    elseif page == "traffic" then draw_traffic_page()
                    end
                    last_draw = now
                end
            end
        end

        -- 5. Sleep (longer when screen off to save CPU)
        if screen_state == "off" then
            sleep_ms(200)
        else
            sleep_ms(50)
        end
    end
end

main()
