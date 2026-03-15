-- LTE Signal + Traffic graph
return {
    name = "LTE",
    order = 3,
    color = "cyan",
    graph = true,  -- pressing this opens graph page

    status = function()
        local raw = io.popen("uqmi -d /dev/cdc-wdm0 --get-signal-info 2>/dev/null"):read("*a") or ""
        local rsrp = raw:match('"rsrp":%s*(%-?%d+)')
        local rssi = raw:match('"rssi":%s*(%-?%d+)')
        if rsrp then
            local val = tonumber(rsrp)
            local color = val >= -90 and "#07E0" or (val >= -110 and "yellow" or "red")
            return {text = "RSRP:" .. rsrp .. "dBm", color = color}
        elseif rssi then
            return {text = "RSSI:" .. rssi .. "dBm", color = "yellow"}
        end
        return {text = "No signal", color = "red"}
    end,

    action = function()
        -- graph mode is activated by lcd_ui via graph=true flag
    end,
}
