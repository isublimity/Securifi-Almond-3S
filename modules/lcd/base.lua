--[[
  lcd/base.lua — Core module: drawing, data collection, graph engine
  All widgets use this. No widget should call shell/lcdlib directly.
]]

local M = {}
local lcd = require("lcdlib")

-- === Constants ===
M.W = 320
M.H = 240
M.BLACK  = 0x0000
M.WHITE  = 0xFFFF
M.RED    = 0xF800
M.GREEN  = 0x07E0
M.BLUE   = 0x001F
M.YELLOW = 0xFFE0
M.CYAN   = 0x07FF
M.GRAY   = 0x7BEF
M.DKGRAY = 0x39E7
M.ORANGE = 0xFBE0

-- === LCD init ===
function M.init()
    return lcd.open()
end

-- === Drawing primitives (thin wrappers) ===
function M.clear(c) lcd.clear(c or M.BLACK) end
function M.rect(x, y, w, h, c) lcd.rect(math.floor(x), math.floor(y), math.floor(w), math.floor(h), math.floor(c)) end
function M.text(x, y, s, c, scale) lcd.text(math.floor(x), math.floor(y), s, math.floor(c or M.WHITE), math.floor(scale or 1)) end
function M.pixel(x, y, c) lcd.pixel(math.floor(x), math.floor(y), math.floor(c)) end
function M.line(x0, y0, x1, y1, c) lcd.line(math.floor(x0), math.floor(y0), math.floor(x1), math.floor(y1), math.floor(c)) end
function M.flush() lcd.flush() end

-- === Touch ===
function M.touch() return lcd.touch() end
function M.usleep(us) lcd.usleep(us) end

-- === Scene control ===
function M.scene(name) lcd.scene(name) end
function M.dashboard(params) lcd.dashboard(params) end
function M.backlight(on) lcd.backlight(on) end

-- === High-level drawing ===

function M.fill_rect(x, y, w, h, border_c, fill_c)
    M.rect(x, y, w, h, border_c)
    if fill_c then M.rect(x+1, y+1, w-2, h-2, fill_c) end
end

function M.button(x, y, w, h, label, color, bg)
    bg = bg or M.DKGRAY
    M.rect(x, y, w, h, color)
    M.rect(x+2, y+2, w-4, h-4, bg)
    local tx = x + (w - #label * 12) / 2
    local ty = y + (h - 14) / 2
    M.text(tx, ty, label, color, 2)
end

function M.header(text, color)
    M.rect(0, 0, M.W, 22, color or M.BLUE)
    M.text(4, 3, text, M.WHITE, 2)
end

function M.status_bar(text, color)
    M.rect(0, M.H - 14, M.W, 14, M.DKGRAY)
    M.text(4, M.H - 12, text, color or M.GRAY, 1)
end

-- === Graph: Bar chart ===
-- data = array of numbers, x,y,w,h = area, color, max_val (auto if nil)
function M.graph_bars(data, x, y, w, h, color, max_val)
    if #data == 0 then return end
    max_val = max_val or 1
    for _, v in ipairs(data) do if v > max_val then max_val = v end end
    if max_val == 0 then max_val = 1 end

    local bar_w = math.floor(w / #data)
    if bar_w < 1 then bar_w = 1 end
    local gap = bar_w > 3 and 1 or 0

    -- Background
    M.rect(x, y, w, h, M.DKGRAY)

    for i, v in ipairs(data) do
        local bh = math.floor(v * (h - 2) / max_val)
        if bh < 1 and v > 0 then bh = 1 end
        local bx = x + (i - 1) * bar_w + gap
        local by = y + h - 1 - bh
        M.rect(bx, by, bar_w - gap * 2, bh, color or M.GREEN)
    end
end

-- === Graph: Line chart ===
-- data = array of numbers, x,y,w,h = area, color, max_val
function M.graph_line(data, x, y, w, h, color, max_val)
    if #data < 2 then return end
    max_val = max_val or 1
    for _, v in ipairs(data) do if v > max_val then max_val = v end end
    if max_val == 0 then max_val = 1 end

    -- Background + grid
    M.rect(x, y, w, h, M.DKGRAY)
    for gy = 1, 3 do
        local ly = y + math.floor(gy * h / 4)
        for gx = x, x + w - 1, 4 do M.pixel(gx, ly, M.GRAY) end
    end

    local step = w / (#data - 1)
    local prev_px, prev_py
    for i, v in ipairs(data) do
        local px = x + math.floor((i - 1) * step)
        local py = y + h - 1 - math.floor(v * (h - 2) / max_val)
        if py < y then py = y end
        if prev_px then
            M.line(prev_px, prev_py, px, py, color or M.CYAN)
        end
        prev_px, prev_py = px, py
    end
end

-- === Graph: Dual line (RX/TX) ===
function M.graph_dual(data1, data2, x, y, w, h, c1, c2, max_val)
    max_val = max_val or 1
    for _, v in ipairs(data1) do if v > max_val then max_val = v end end
    for _, v in ipairs(data2) do if v > max_val then max_val = v end end
    M.rect(x, y, w, h, M.DKGRAY)
    if #data1 >= 2 then
        local step = w / (#data1 - 1)
        local px, py
        for i, v in ipairs(data1) do
            local nx = x + math.floor((i-1)*step)
            local ny = y + h - 1 - math.floor(v*(h-2)/max_val)
            if px then M.line(px, py, nx, ny, c1 or M.GREEN) end
            px, py = nx, ny
        end
    end
    if #data2 >= 2 then
        local step = w / (#data2 - 1)
        local px, py
        for i, v in ipairs(data2) do
            local nx = x + math.floor((i-1)*step)
            local ny = y + h - 1 - math.floor(v*(h-2)/max_val)
            if px then M.line(px, py, nx, ny, c2 or M.RED) end
            px, py = nx, ny
        end
    end
end

-- === Data collection ===

function M.sh(cmd)
    local f = io.popen(cmd .. " 2>/dev/null")
    if not f then return "" end
    local o = f:read("*a") or ""
    f:close()
    return o
end

function M.wifi_clients()
    local cl = {}
    for _, iface in ipairs({"phy0-ap0", "phy1-ap0"}) do
        local d = M.sh("iwinfo " .. iface .. " assoclist")
        for line in d:gmatch("[^\n]+") do
            local mac = line:match("^(%S+)")
            local sig = line:match("(-?%d+) dBm")
            local tx = line:match("TX:%s+([%d%.]+)")
            local rx = line:match("RX:%s+([%d%.]+)")
            if mac and sig then
                cl[#cl+1] = {
                    mac = mac,
                    signal = tonumber(sig) or -70,
                    tx_mbps = tonumber(tx) or 0,
                    rx_mbps = tonumber(rx) or 0,
                    band = iface:match("phy0") and "5G" or "2G"
                }
            end
        end
    end
    return cl
end

function M.lte_signal()
    local s = M.sh("uqmi -d /dev/cdc-wdm0 --get-signal-info")
    return {
        rsrp = tonumber(s:match('"rsrp":%s*(-?%d+)')) or 0,
        rssi = tonumber(s:match('"rssi":%s*(-?%d+)')) or 0,
        rsrq = tonumber(s:match('"rsrq":%s*(-?%d+)')) or 0,
        snr  = tonumber(s:match('"snr":%s*(-?%d+)')) or 0,
    }
end

function M.lte_status()
    local s = M.sh("uqmi -d /dev/cdc-wdm0 --get-serving-system")
    return {
        reg = s:match('"registration":%s*"(%w+)"') or "unknown",
        mcc = s:match('"plmn_mcc":%s*(%d+)') or "",
        mnc = s:match('"plmn_mnc":%s*(%d+)') or "",
    }
end

function M.vpn_status()
    local r = {wg = false, ovpn = false, ip = ""}
    if M.sh("wg show"):match("latest handshake") then r.wg = true end
    if M.sh("ip addr show tun0"):match("inet") then r.ovpn = true end
    r.ip = M.sh("wget -qO- ifconfig.me"):gsub("%s", "")
    return r
end

function M.iface_ip(name)
    local d = M.sh("ip addr show " .. name)
    return d:match("inet (%d+%.%d+%.%d+%.%d+)") or ""
end

function M.traffic(iface)
    local rx = tonumber(M.sh("cat /sys/class/net/" .. iface .. "/statistics/rx_bytes")) or 0
    local tx = tonumber(M.sh("cat /sys/class/net/" .. iface .. "/statistics/tx_bytes")) or 0
    return rx, tx
end

function M.uptime()
    return M.sh("uptime"):match("up (.-),%s") or "?"
end

function M.fmt_bytes(b)
    if b > 1048576 then return string.format("%.1fM", b / 1048576) end
    if b > 1024 then return string.format("%.0fK", b / 1024) end
    return tostring(b)
end

-- === History buffer ===
function M.history_new(max_len)
    return {data = {}, max = max_len or 60}
end

function M.history_push(h, val)
    h.data[#h.data + 1] = val
    if #h.data > h.max then table.remove(h.data, 1) end
end

return M
