-- WireGuard toggle button
return {
    name = "WireGuard",
    order = 1,
    color = "#07E0",

    status = function()
        local disabled = io.popen("uci -q get network.wgvpn.disabled 2>/dev/null"):read("*l") or "1"
        local running = io.popen("wg show wgvpn 2>/dev/null | head -1"):read("*l") or ""
        if disabled ~= "1" and running ~= "" then
            return {text = "ON", color = "#07E0"}
        end
        return {text = "OFF", color = "red"}
    end,

    action = function()
        local disabled = io.popen("uci -q get network.wgvpn.disabled 2>/dev/null"):read("*l") or "1"
        if disabled == "1" then
            os.execute("uci set openvpn.sirius.enabled=0; uci commit openvpn; /etc/init.d/openvpn stop 2>/dev/null")
            os.execute("uci set network.wgvpn.disabled=0; uci commit network; ifup wgvpn")
        else
            os.execute("ifdown wgvpn 2>/dev/null; uci set network.wgvpn.disabled=1; uci commit network")
        end
    end,
}
