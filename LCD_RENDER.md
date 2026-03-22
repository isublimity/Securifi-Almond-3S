# LCD Display Stack

## Architecture

```
+--------------------------------------------------+
|  lcd_ui.uc (ucode)                               |
|  uloop event loop + ubus + uci                   |
|  Dashboard, menu pages, touch handling            |
+-----+--------------------------------------------+
      | JSON commands via socat → unix socket
+-----v--------------------------------------------+
|  lcd_render (C, static binary)                    |
|  Unix socket server /tmp/lcd.sock                 |
|  5x7 bitmap font, rect, clear, text              |
|  Local framebuffer → write() to /dev/lcd         |
+-----+--------------------------------------------+
      | write() 153600 bytes RGB565
+-----v--------------------------------------------+
|  lcd_drv.ko (kernel module)                       |
|  Framebuffer (mmap) + render thread (fps=10)     |
|  GPIO DIR bit-bang → ILI9341 display             |
|  SX8650 touch via palmbus I2C                    |
|  PIC16 battery via bit-bang I2C                  |
+--------------------------------------------------+
```

## Data Flow

```
data_collector (C daemon)
  └── AT commands (LTE CSQ, operator)
  └── wg show (VPN status)
  └── iw station dump (WiFi clients)
  └── ping (connectivity)
  └── sysinfo (uptime, memory)
  └── Writes /tmp/lcd_data.json every 2 sec

touch_poll (C daemon)
  └── ioctl(/dev/lcd, 1) every 50ms
  └── On press edge: writes /tmp/.lcd_touch (latch)
  └── UI reads and unlinks after processing

lcd_ui.uc (ucode script)
  └── uloop timer: reads /tmp/lcd_data.json every 2s
  └── uloop timer: reads /tmp/.lcd_touch every 100ms
  └── ubus calls: system info (uptime, memory, load)
  └── uci calls: wireless config (SSIDs)
  └── Sends JSON draw commands to lcd_render via socat
```

## lcd_render Commands

Send via unix socket `/tmp/lcd.sock`:

```bash
# Single command
echo '{"cmd":"clear","color":"#FF0000"}' | socat - UNIX-CONNECT:/tmp/lcd.sock

# Batch commands (one per line)
cat << 'EOF' | socat - UNIX-CONNECT:/tmp/lcd.sock
{"cmd":"clear","color":"black"}
{"cmd":"rect","x":0,"y":0,"w":320,"h":18,"color":"#001F"}
{"cmd":"text","x":4,"y":2,"text":"Hello World","color":"white","bg":"#001F","size":2}
{"cmd":"flush"}
EOF
```

### Command Reference

| Command | Required | Optional | Description |
|---------|----------|----------|-------------|
| `clear` | `color` | — | Fill entire screen |
| `rect` | `x`, `y`, `w`, `h`, `color` | — | Filled rectangle |
| `text` | `x`, `y`, `text` | `color`, `bg`, `size` | Text (5x7 font) |
| `flush` | — | — | Force display update |
| `fps` | `value` | — | Set kernel render FPS |

### Color Formats

| Format | Example | Description |
|--------|---------|-------------|
| Named | `"red"` | red, green, blue, white, black, yellow, cyan |
| RGB888 | `"#FF8000"` | Hex RGB, auto-converted to RGB565 |
| RGB565 | `"#F800"` | Raw 16-bit RGB565 |

## touch_poll

```bash
# Start daemon (latch mode — writes only on press edge)
touch_poll daemon

# Backlight control
touch_poll bl 0    # OFF
touch_poll bl 1    # ON
touch_poll bl 2    # Show splash

# Demo mode (foreground, draws crosshairs)
touch_poll
```

## Font

Built-in 5x7 pixel bitmap font, ASCII 32-126:

| Scale | Char size | Chars/line | Lines |
|-------|-----------|-----------|-------|
| 1 | 6x8 px | 53 | 30 |
| 2 | 12x16 px | 26 | 15 |
| 3 | 18x24 px | 17 | 10 |

## Performance

| Operation | Time |
|-----------|------|
| Full frame flush (GPIO bit-bang) | ~100ms at fps=10 |
| Text render (scale 2, 20 chars) | <1ms |
| Framebuffer write (153KB) | <1ms |
| Touch poll | 50ms interval |
| Dashboard full redraw | ~50ms (socat overhead) |
