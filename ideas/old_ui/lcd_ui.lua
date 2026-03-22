#!/usr/bin/lua
--[[
  lcd_ui.lua — Main UI for Almond 3S
  States: DASHBOARD (plasma) → tap → MENU (buttons) → tap → INFO
  Auto-return to dashboard after 30s idle.
  Dashboard feeds real router data to kernel plasma scene.
]]

local lcd = require("lcdlib")
if not lcd.open() then print("ERROR: /dev/lcd"); os.exit(1) end

local BLACK,WHITE,RED,GREEN,BLUE,YELLOW,CYAN,GRAY,DKGRAY =
    0x0000,0xFFFF,0xF800,0x07E0,0x001F,0xFFE0,0x07FF,0x7BEF,0x39E7

local state = "dashboard"
local last_touch = os.time()
local last_update = 0
local was_pressed = false
local MENU_TIMEOUT = 30

-- Shell helper
local function sh(cmd)
    local f = io.popen(cmd .. " 2>/dev/null")
    if not f then return "" end
    local o = f:read("*a") or ""; f:close(); return o
end

-- === Data collection ===
local function get_wifi_clients()
    local cl = {}
    for _, iface in ipairs({"phy0-ap0","phy1-ap0"}) do
        local d = sh("iwinfo "..iface.." assoclist")
        for line in d:gmatch("[^\n]+") do
            local sig = line:match("(-?%d+) dBm")
            local tx = line:match("TX:%s+([%d%.]+)")
            if sig then
                cl[#cl+1] = {signal=tonumber(sig) or -70, kbps=(tonumber(tx) or 0)*100}
            end
        end
    end
    return cl
end

local function get_lte_rsrp()
    local s = sh("uqmi -d /dev/cdc-wdm0 --get-signal-info")
    return tonumber(s:match('"rsrp":%s*(-?%d+)')) or tonumber(s:match('"rssi":%s*(-?%d+)')) or -100
end

local function get_vpn_ms()
    if sh("ip addr show tun0"):match("inet") then
        local m = sh("ping -c1 -W2 10.8.0.1"):match("time=([%d%.]+)")
        return tonumber(m) or 80
    end
    if sh("wg show"):match("handshake") then
        local m = sh("ping -c1 -W2 10.66.66.1"):match("time=([%d%.]+)")
        return tonumber(m) or 50
    end
    return -1
end

local function update_dashboard()
    local cl = get_wifi_clients()
    local d = {lte=get_lte_rsrp(), vpn=get_vpn_ms(), clients={}}
    for _,c in ipairs(cl) do d.clients[#d.clients+1] = {kbps=c.kbps, signal=c.signal} end
    lcd.dashboard(d)
end

-- === Menu ===
local buttons = {
    {x=0,  y=0,  w=160,h=80, label="VPN",  color=GREEN},
    {x=160,y=0,  w=160,h=80, label="LTE",  color=CYAN},
    {x=0,  y=80, w=160,h=80, label="WiFi", color=BLUE},
    {x=160,y=80, w=160,h=80, label="IP",   color=YELLOW},
    {x=0,  y=160,w=160,h=80, label="Info", color=GRAY},
    {x=160,y=160,w=160,h=80, label="Back", color=RED},
}

local function draw_menu()
    lcd.scene("off")
    lcd.clear(BLACK)
    for _,b in ipairs(buttons) do
        lcd.rect(b.x+2, b.y+2, b.w-4, b.h-4, b.color)
        lcd.rect(b.x+4, b.y+4, b.w-8, b.h-8, DKGRAY)
        lcd.text(b.x+(b.w-#b.label*12)/2, b.y+(b.h-14)/2, b.label, b.color, 2)
    end
    lcd.flush()
end

local function show_info(title, lines)
    lcd.clear(BLACK)
    lcd.text(10, 5, title, YELLOW, 2)
    for i,l in ipairs(lines) do lcd.text(10, 25+i*16, l, WHITE, 1) end
    lcd.text(10, 220, "Tap to go back", GRAY, 1)
    lcd.flush()
end

local function handle_btn(idx)
    if idx==1 then -- VPN
        local tun = sh("ip addr show tun0 | grep 'inet '"):match("inet ([%d%.]+)") or "off"
        local wg = sh("wg show"):match("handshake") and "active" or "off"
        local ext = sh("wget -qO- ifconfig.me"):gsub("%s","")
        show_info("VPN", {"OVPN: "..tun, "WG: "..wg, "Exit: "..(ext~="" and ext or "?")})
        state="info"
    elseif idx==2 then -- LTE
        local rsrp = get_lte_rsrp()
        local reg = sh("uqmi -d /dev/cdc-wdm0 --get-serving-system"):match('"registration":%s*"(%w+)"') or "?"
        local ip = sh("ip addr show wwan0 | grep 'inet '"):match("inet ([%d%.]+)") or "no IP"
        show_info("LTE", {"Signal: "..rsrp.." dBm", "Status: "..reg, "IP: "..ip})
        state="info"
    elseif idx==3 then -- WiFi
        local cl = get_wifi_clients()
        local lines = {"Clients: "..#cl}
        for i,c in ipairs(cl) do if i<=5 then lines[#lines+1]=string.format("#%d %ddBm",i,c.signal) end end
        show_info("WiFi", lines)
        state="info"
    elseif idx==4 then -- IP
        local lan = sh("ip addr show br-lan | grep 'inet '"):match("inet ([%d%.]+)") or "?"
        local wan = sh("ip addr show wwan0 | grep 'inet '"):match("inet ([%d%.]+)") or "no LTE"
        local ext = sh("wget -qO- ifconfig.me"):gsub("%s","")
        show_info("IP", {"LAN: "..lan, "WAN: "..wan, "External: "..(ext~="" and ext or "?")})
        state="info"
    elseif idx==5 then -- Info
        local up = sh("uptime"):match("up (.-),%s") or "?"
        local kern = sh("uname -r"):gsub("\n","")
        show_info("System", {"Kernel: "..kern, "Uptime: "..up, "LCD+Touch+LAN+WiFi+LTE"})
        state="info"
    elseif idx==6 then -- Back
        state="dashboard"; update_dashboard()
    end
end

local function find_btn(x,y)
    for i,b in ipairs(buttons) do
        if x>=b.x and x<b.x+b.w and y>=b.y and y<b.y+b.h then return i end
    end
end

-- === Main loop ===
print("lcd_ui: dashboard starting")
update_dashboard()

while true do
    local x,y,p = lcd.touch()
    if p==1 and not was_pressed then
        last_touch = os.time()
        if state=="dashboard" then state="menu"; draw_menu()
        elseif state=="menu" then local i=find_btn(x,y); if i then handle_btn(i) end
        elseif state=="info" then state="menu"; draw_menu()
        end
    end
    was_pressed = (p==1)

    if state~="dashboard" and (os.time()-last_touch)>MENU_TIMEOUT then
        state="dashboard"; update_dashboard()
    end

    if state=="dashboard" and (os.time()-last_update)>=5 then
        update_dashboard(); last_update=os.time()
    end

    lcd.usleep(50000)
end
