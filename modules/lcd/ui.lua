#!/usr/bin/lua
--[[
  lcd/ui.lua — Main UI loop

  Boot: kernel shows random demoscene splash
  Start: Lua loads → dashboard plasma with real data
  Tap: → menu (6 buttons)
  Tap button: → widget screen
  30s idle: → back to dashboard

  Widgets auto-discovered from lcd/widgets/*.lua
]]

package.path = "/usr/lib/lua/?.lua;/usr/lib/lua/?/init.lua;" .. package.path

local B = require("lcd.base")
if not B.init() then print("ERROR: /dev/lcd"); os.exit(1) end

-- === Load widgets ===
local widgets = {}
local widget_names = {"dashboard", "vpn", "lte", "wifi", "ip", "info"}

for _, name in ipairs(widget_names) do
    local ok, w = pcall(require, "lcd.widgets." .. name)
    if ok then
        widgets[name] = w
        print("lcd_ui: loaded widget: " .. name)
    else
        print("lcd_ui: FAILED to load " .. name .. ": " .. tostring(w))
    end
end

-- === Menu definition ===
local menu_items = {
    {name = "vpn",   label = "VPN",  color = B.GREEN},
    {name = "lte",   label = "LTE",  color = B.CYAN},
    {name = "wifi",  label = "WiFi", color = B.BLUE},
    {name = "ip",    label = "IP",   color = B.YELLOW},
    {name = "info",  label = "Info", color = B.GRAY},
    {name = "back",  label = "Back", color = B.RED},
}

local function draw_menu()
    B.scene("off")
    B.clear()
    B.header("Almond 3S", B.BLUE)
    local cols, rows = 2, 3
    local bw = B.W / cols
    local bh = (B.H - 22) / rows
    for i, item in ipairs(menu_items) do
        local col = (i - 1) % cols
        local row = math.floor((i - 1) / cols)
        local x = col * bw
        local y = 22 + row * bh
        B.button(x, y, bw, bh, item.label, item.color)
    end
    B.flush()
end

local function find_menu_item(x, y)
    if y < 22 then return nil end
    local cols, rows = 2, 3
    local bw = B.W / cols
    local bh = (B.H - 22) / rows
    local col = math.floor(x / bw)
    local row = math.floor((y - 22) / bh)
    local idx = row * cols + col + 1
    if idx >= 1 and idx <= #menu_items then return menu_items[idx] end
    return nil
end

-- === State machine ===
local state = "dashboard"
local current_widget = nil
local last_touch = os.time()
local last_update = 0
local was_pressed = false
local MENU_TIMEOUT = 30
local DASHBOARD_INTERVAL = 5

-- Start with dashboard
print("lcd_ui: starting")
if widgets.dashboard then
    widgets.dashboard.draw()
end

while true do
    local x, y, p = B.touch()

    -- Touch handler
    if p == 1 and not was_pressed then
        last_touch = os.time()

        if state == "dashboard" then
            state = "menu"
            draw_menu()

        elseif state == "menu" then
            local item = find_menu_item(x, y)
            if item then
                if item.name == "back" then
                    state = "dashboard"
                    if widgets.dashboard then widgets.dashboard.draw() end
                    last_update = os.time()
                elseif widgets[item.name] then
                    state = "widget"
                    current_widget = widgets[item.name]
                    local ok, err = pcall(current_widget.draw)
                    if not ok then
                        B.clear(); B.text(10, 100, "Error: " .. tostring(err), B.RED, 1); B.flush()
                    end
                end
            end

        elseif state == "widget" then
            local next_state = nil
            if current_widget and current_widget.on_tap then
                local ok, result = pcall(current_widget.on_tap, x, y)
                if ok then next_state = result end
            end
            if next_state == "menu" then
                state = "menu"
                draw_menu()
            elseif next_state == "dashboard" then
                state = "dashboard"
                if widgets.dashboard then widgets.dashboard.draw() end
                last_update = os.time()
            end
        end
    end
    was_pressed = (p == 1)

    -- Auto return to dashboard
    if state ~= "dashboard" and (os.time() - last_touch) > MENU_TIMEOUT then
        state = "dashboard"
        if widgets.dashboard then widgets.dashboard.draw() end
        last_update = os.time()
    end

    -- Periodic dashboard update
    if state == "dashboard" and (os.time() - last_update) >= DASHBOARD_INTERVAL then
        if widgets.dashboard then
            pcall(widgets.dashboard.draw)
        end
        last_update = os.time()
    end

    B.usleep(50000)
end
