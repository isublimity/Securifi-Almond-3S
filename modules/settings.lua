-- settings.lua — LCD UI configuration
-- Place at /etc/lcd/settings.lua on router

return {
    -- Menu buttons (touch)
    buttons = {
        { id = "vpn",      label = "VPN",      icon = "shield",  enabled = true,  order = 1 },
        { id = "wifi",     label = "WiFi",     icon = "signal",  enabled = true,  order = 2 },
        { id = "lte",      label = "LTE",      icon = "antenna", enabled = true,  order = 3 },
        { id = "clients",  label = "Clients",  icon = "users",   enabled = true,  order = 4 },
        { id = "info",     label = "Info",     icon = "info",    enabled = true,  order = 5 },
        { id = "reboot",   label = "Reboot",   icon = "power",   enabled = false, order = 6 },
    },

    -- Dashboard settings
    dashboard = {
        update_interval = 2,     -- seconds between data refresh
        burnin_shift_interval = 30,  -- seconds between anti-burn-in pixel shifts
        burnin_shift_px = 2,     -- pixels to shift
        touch_timeout = 10,      -- seconds of no touch → return to dashboard
    },

    -- Logo/splash settings
    logo = {
        duration = 3,            -- seconds to show logo at startup
        random_scene = true,     -- random demoscene from lcd_drv
    },

    -- Colors (RGB565)
    colors = {
        bg_excellent = 0x07E0,   -- green (CSQ > 25)
        bg_good      = 0xC618,   -- light gray (CSQ 15-25)
        bg_fair      = 0x8410,   -- gray (CSQ 5-15)
        bg_poor      = 0xF800,   -- red (CSQ < 5)
        vpn_ok       = 0x07E0,   -- green wave
        vpn_down     = 0xF800,   -- red circles
        text_white   = 0xFFFF,
        text_dark    = 0x0000,
        text_accent  = 0xFFE0,   -- yellow
    },
}
