-- External IP display
return {
    name = "Exit IP",
    order = 4,
    color = "white",

    status = function()
        -- Read cached IP (updated by action or background)
        local f = io.open("/tmp/lcd_ext_ip", "r")
        if f then
            local ip = f:read("*l") or "?"
            f:close()
            return {text = ip, color = "#07E0"}
        end
        return {text = "tap to check", color = "#4208"}
    end,

    action = function()
        os.execute("wget -qO- -T 5 http://ifconfig.me/ip > /tmp/lcd_ext_ip 2>/dev/null &")
    end,
}
