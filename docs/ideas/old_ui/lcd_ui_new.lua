#!/usr/bin/lua
-- lcd_ui.lua — Main LCD UI for Almond 3S
-- States: LOGO → DASHBOARD (with touch → MENU → timeout → DASHBOARD)
-- Reads /tmp/lcd_data.json from data_collector daemon

local LCD_W, LCD_H = 320, 240
local FB_SIZE = LCD_W * LCD_H * 2
local DEV_LCD = "/dev/lcd"
local DATA_FILE = "/tmp/lcd_data.json"
local SETTINGS_FILE = "/etc/lcd/settings.lua"

-- Load settings or defaults
local settings = loadfile(SETTINGS_FILE)
settings = settings and settings() or {
    dashboard = { update_interval=2, burnin_shift_interval=30, burnin_shift_px=2, touch_timeout=10 },
    logo = { duration=3 },
}

-- Read /tmp/lcd_data.json
local function read_data()
    local f = io.open(DATA_FILE, "r")
    if not f then return {} end
    local s = f:read("*a"); f:close()
    -- Minimal JSON parse via gsub → Lua table
    s = s:gsub('"([^"]+)"%s*:', '["%1"]=')
    s = s:gsub('%[%[', '{ [')
    s = s:gsub('true', '1'):gsub('false', '0'):gsub('null', 'nil')
    local fn = load("return " .. s)
    if fn then local ok, r = pcall(fn); if ok then return r end end
    return {}
end

-- Open framebuffer for writing
local fb = io.open(DEV_LCD, "r+b")
if not fb then print("Cannot open "..DEV_LCD); os.exit(1) end

-- RGB565 helpers
local function rgb(r,g,b) return bit32 and bit32.bor(bit32.lshift(r,11), bit32.lshift(g,5), b)
    or r*2048 + g*32 + b end
local function color_bytes(c) return string.char(c%256, math.floor(c/256)) end

-- Fill screen with color
local function fill(c)
    fb:seek("set", 0)
    fb:write(color_bytes(c):rep(LCD_W * LCD_H))
    fb:flush()
end

-- Put pixel
local function px(x, y, c)
    if x<0 or x>=LCD_W or y<0 or y>=LCD_H then return end
    fb:seek("set", (y*LCD_W+x)*2)
    fb:write(color_bytes(c))
end

-- Horizontal line
local function hline(x1, x2, y, c)
    if y<0 or y>=LCD_H then return end
    if x1<0 then x1=0 end; if x2>=LCD_W then x2=LCD_W-1 end
    fb:seek("set", (y*LCD_W+x1)*2)
    fb:write(color_bytes(c):rep(x2-x1+1))
end

-- Fill rect
local function rect(x1,y1,x2,y2,c)
    for y=y1,y2 do hline(x1,x2,y,c) end
end

-- 5x7 bitmap font (uppercase + digits + common chars)
local G = {
    [32]={0,0,0,0,0}, -- space
    [48]={0x3E,0x51,0x49,0x45,0x3E}, [49]={0,0x42,0x7F,0x40,0},
    [50]={0x42,0x61,0x51,0x49,0x46}, [51]={0x21,0x41,0x45,0x4B,0x31},
    [52]={0x18,0x14,0x12,0x7F,0x10}, [53]={0x27,0x45,0x45,0x45,0x39},
    [54]={0x3C,0x4A,0x49,0x49,0x30}, [55]={1,0x71,9,5,3},
    [56]={0x36,0x49,0x49,0x49,0x36}, [57]={6,0x49,0x49,0x29,0x1E},
    [65]={0x7E,0x11,0x11,0x11,0x7E}, [66]={0x7F,0x49,0x49,0x49,0x36},
    [67]={0x3E,0x41,0x41,0x41,0x22}, [68]={0x7F,0x41,0x41,0x22,0x1C},
    [69]={0x7F,0x49,0x49,0x49,0x41}, [70]={0x7F,9,9,9,1},
    [71]={0x3E,0x41,0x49,0x49,0x7A}, [72]={0x7F,8,8,8,0x7F},
    [73]={0,0x41,0x7F,0x41,0}, [75]={0x7F,8,0x14,0x22,0x41},
    [76]={0x7F,0x40,0x40,0x40,0x40}, [77]={0x7F,2,0xC,2,0x7F},
    [78]={0x7F,4,8,0x10,0x7F}, [79]={0x3E,0x41,0x41,0x41,0x3E},
    [80]={0x7F,9,9,9,6}, [82]={0x7F,9,0x19,0x29,0x46},
    [83]={0x46,0x49,0x49,0x49,0x31}, [84]={1,1,0x7F,1,1},
    [85]={0x3F,0x40,0x40,0x40,0x3F}, [86]={0x1F,0x20,0x40,0x20,0x1F},
    [87]={0x3F,0x40,0x38,0x40,0x3F},
    [58]={0,0x36,0x36,0,0}, [46]={0,0x60,0x60,0,0},
    [45]={8,8,8,8,8}, [37]={0x23,0x13,8,0x64,0x62},
}

local function putch(x, y, ch, c, s)
    s = s or 1
    local g = G[ch] or G[32]
    for col=0,4 do
        local b = g[col+1]
        for row=0,6 do
            if b%2==1 then
                for dy=0,s-1 do for dx=0,s-1 do
                    px(x+col*s+dx, y+row*s+dy, c)
                end end
            end
            b = math.floor(b/2)
        end
    end
end

local function text(x, y, str, c, s)
    s = s or 1
    str = str:upper()
    for i=1,#str do putch(x+(i-1)*6*s, y, str:byte(i), c, s) end
end

-- Background color by LTE quality
local function lte_bg(csq)
    if csq > 25 then return rgb(0,40,0)      -- dark green
    elseif csq > 15 then return rgb(4,8,4)    -- dark gray-green
    elseif csq > 5 then return rgb(8,8,8)     -- dark gray
    else return rgb(12,0,0) end               -- dark red
end

-- VPN indicator
local function vpn_wave(active, t)
    if active then
        for x=0,LCD_W-1 do
            local y = 15 + math.floor(math.sin((x+t*3)/20)*4)
            if y>=0 and y<LCD_H then px(x,y,rgb(0,63,0)) end
        end
    else
        for i=0,2 do
            local cx = 50+i*110
            local r = 6+math.floor(math.sin(t/3+i)*3)
            for a=0,359,8 do
                local ax=cx+math.floor(math.cos(math.rad(a))*r)
                local ay=15+math.floor(math.sin(math.rad(a))*r)
                px(ax,ay,rgb(31,0,0))
            end
        end
    end
end

-- MAIN
print("lcd_ui: start")

-- Wait for logo (lcd_drv splash)
os.execute("sleep " .. (settings.logo.duration or 3))

-- Take over framebuffer (stop lcd_drv splash)
fb:seek("set",0); fb:write("\0\0"); fb:flush()

local frame = 0
local shift_x, shift_y = 0, 0
local last_shift = os.time()

while true do
    local now = os.time()
    local d = read_data()
    local csq = d.lte and d.lte.csq or 0

    -- Anti-burn-in
    local si = settings.dashboard.burnin_shift_interval or 30
    if now - last_shift >= si then
        local sp = settings.dashboard.burnin_shift_px or 2
        shift_x = math.random(-sp, sp)
        shift_y = math.random(-sp, sp)
        last_shift = now
    end

    local ox, oy = shift_x, shift_y

    -- Background
    fill(lte_bg(csq))

    -- VPN wave/circles
    local vpn_ok = d.vpn and d.vpn.active == 1
    vpn_wave(vpn_ok, frame)

    -- Text color
    local tc = csq > 15 and rgb(0,0,0) or rgb(31,63,31)
    local ac = rgb(31,63,0) -- accent yellow

    -- LTE line
    text(4+ox, 30+oy, string.format("LTE CSQ:%d %s", csq, d.lte and d.lte.operator or ""), tc, 2)

    -- VPN line
    local vt = vpn_ok and "VPN OK" or "VPN DOWN"
    text(4+ox, 55+oy, vt, vpn_ok and rgb(0,63,0) or rgb(31,0,0), 2)

    -- Ping
    local ping = d.ping and d.ping.google_ms or 0
    text(200+ox, 55+oy, string.format("%DMS", ping), tc, 2)

    -- WiFi clients
    local yy = 85+oy
    if d.wifi and type(d.wifi.clients)=="table" then
        for _, cl in ipairs(d.wifi.clients) do
            text(4+ox, yy, string.format("%s %DDB", cl.name or "?", cl.signal or 0), tc, 1)
            yy = yy + 12
            if yy > 210 then break end
        end
    end

    -- Bottom: uptime + mem
    local uh = d.uptime and math.floor(d.uptime/3600) or 0
    local mem = d.mem_free_mb or 0
    text(4+ox, 225+oy, string.format("UP:%DH MEM:%DM", uh, mem), ac, 1)

    fb:flush()
    frame = frame + 1
    os.execute("sleep 0.2") -- ~5fps
end
