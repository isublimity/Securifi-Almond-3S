-- OpenVPN toggle button
return {
    name = "OpenVPN",
    order = 2,
    color = "yellow",

    status = function()
        local enabled = io.popen("uci -q get openvpn.sirius.enabled 2>/dev/null"):read("*l") or "0"
        local running = io.popen("pgrep -x openvpn 2>/dev/null"):read("*l") or ""
        if enabled == "1" and running ~= "" then
            return {text = "ON", color = "#07E0"}
        end
        return {text = "OFF", color = "red"}
    end,

    action = function()
        local enabled = io.popen("uci -q get openvpn.sirius.enabled 2>/dev/null"):read("*l") or "0"
        if enabled ~= "1" then
            os.execute("ifdown wgvpn 2>/dev/null; uci set network.wgvpn.disabled=1; uci commit network")
            os.execute("uci set openvpn.sirius.enabled=1; uci commit openvpn; /etc/init.d/openvpn restart")
        else
            os.execute("uci set openvpn.sirius.enabled=0; uci commit openvpn; /etc/init.d/openvpn stop 2>/dev/null")
        end
    end,
}
