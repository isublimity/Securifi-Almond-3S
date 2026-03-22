# LCD UI — Architecture

## Data Flow

```
[data_collector] ──JSON──> /tmp/lcd_data.json ──> [lcd_ui.uc] ──JSON──> [lcd_render] ──> /dev/lcd
                                                       ↑
                                                  ubus (system info)
                                                  uci (WiFi config)
                                                       ↑
[touch_poll]     ──file──> /tmp/.lcd_touch ───────────>─┘
```

## UI States

```
BOOT → SPLASH (lcd_drv animation, 3 sec)
     → DASHBOARD (LTE/VPN/WiFi/system, auto-refresh 2s)
          ↓ touch
     → MENU (6 buttons, 2 pages)
          ↓ touch button
     → SUB-PAGE (VPN / LTE / WiFi / Info / IP)
          ↓ BACK button
     → MENU
          ↓ idle 4 min
     → SCREENSAVER (bouncing clock)
          ↓ idle 5 min
     → OFF (backlight off)
          ↓ touch
     → DASHBOARD
```

## Components

### data_collector (C daemon, /usr/bin/data_collector)

Every 2 seconds writes /tmp/lcd_data.json:

```json
{
  "ts": 1711036800,
  "lte": {"csq": 22, "ber": 0, "operator": "beeline", "ip": "10.137.61.178"},
  "vpn": {"active": true, "ping_ms": 38, "external_ip": "82.22.xxx.xxx"},
  "wifi": {"clients": [
    {"mac": "ec:a7:ad:fd:c7:71", "name": "Zeekr", "signal": -34, "rx_bytes": 1500, "tx_bytes": 800}
  ]},
  "ping": {"google_ms": 38},
  "uptime": 3600,
  "mem_free_mb": 180,
  "cpu_load": 0.15
}
```

### lcd_ui.uc (ucode script, /usr/bin/lcd_ui.uc)

- Reads /tmp/lcd_data.json via `fs.readfile()` + `json()` builtin
- Supplements with `ubus` calls (system info: uptime, memory, load, board)
- Reads WiFi SSIDs via `uci` module
- Event loop via `uloop` module (4 timers)
- Sends buffered JSON commands to lcd_render via `fs.popen("socat ...")`
- Touch via /tmp/.lcd_touch (latch file from touch_poll)
- Anti-burn-in: pixel shift +-2px every 30s
- Auto-restart on crash (try/catch wrapper)

### lcd_render (C binary, /usr/bin/lcd_render)

- Unix socket server on /tmp/lcd.sock
- Accepts JSON draw commands (one per line)
- 5x7 bitmap font, rect fill, clear
- Writes framebuffer to /dev/lcd

### touch_poll (C binary, /usr/bin/touch_poll)

- `touch_poll daemon` — background touch poller
- Reads ioctl(1) from /dev/lcd every 50ms
- Latch mode: writes /tmp/.lcd_touch only on press edge (0→1)
- UI reads and unlinks the file
- Also: `touch_poll bl 0/1/2` for backlight control

### settings.lua (/etc/lcd/settings.lua)

```lua
return {
  buttons = {
    { id = "vpn",  label = "VPN",  enabled = true, order = 1 },
    { id = "wifi", label = "WiFi", enabled = true, order = 2 },
    -- ...
  },
  dashboard = {
    update_interval = 2,
    burnin_shift_interval = 30,
    touch_timeout = 10,
  },
}
```

## Files on Router

- `/usr/bin/lcd_ui.uc` — main UI script (ucode)
- `/usr/bin/lcd_render` — renderer daemon
- `/usr/bin/touch_poll` — touch daemon
- `/usr/bin/data_collector` — data collection daemon
- `/etc/lcd/settings.lua` — configuration
- `/tmp/lcd_data.json` — shared sensor data (data_collector → lcd_ui)
- `/tmp/.lcd_touch` — touch events (touch_poll → lcd_ui)
- `/tmp/lcd.sock` — render socket (lcd_ui → lcd_render)
- `/lib/modules/*/lcd_drv.ko` — kernel module
