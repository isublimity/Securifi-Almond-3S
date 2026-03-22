-- widgets/lte.lua — LTE signal info + RSRP graph
local B = require("lcd.base")
local M = {title = "LTE"}

local rsrp_hist = B.history_new(60)
local rssi_hist = B.history_new(60)

function M.draw()
    local sig = B.lte_signal()
    local st = B.lte_status()
    local ip = B.iface_ip("wwan0")

    B.history_push(rsrp_hist, math.abs(sig.rsrp))
    B.history_push(rssi_hist, math.abs(sig.rssi))

    B.clear()
    B.header("LTE Signal", B.CYAN)

    -- Info
    B.text(10, 28, "RSRP: " .. sig.rsrp .. " dBm", sig.rsrp > -90 and B.GREEN or sig.rsrp > -105 and B.YELLOW or B.RED, 2)
    B.text(10, 48, "RSSI: " .. sig.rssi .. "  SNR: " .. sig.snr, B.WHITE, 1)
    B.text(10, 62, "Reg: " .. st.reg .. "  MCC:" .. st.mcc .. "/" .. st.mnc, B.GRAY, 1)
    B.text(10, 76, "IP: " .. (ip ~= "" and ip or "no connection"), B.WHITE, 1)

    -- Quality bar
    local quality = math.max(0, math.min(100, (sig.rsrp + 140) * 100 / 80))
    local qcolor = quality > 60 and B.GREEN or quality > 30 and B.YELLOW or B.RED
    B.text(10, 94, "Quality:", B.GRAY, 1)
    B.rect(80, 94, 200, 10, B.DKGRAY)
    B.rect(80, 94, math.floor(quality * 2), 10, qcolor)

    -- RSRP graph
    B.text(10, 112, "RSRP history (60 samples):", B.GRAY, 1)
    B.graph_line(rsrp_hist.data, 10, 126, 300, 70, B.CYAN, 140)

    B.status_bar("Tap to go back")
    B.flush()
end

function M.on_tap(x, y) return "menu" end

return M
