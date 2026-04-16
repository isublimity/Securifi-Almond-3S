# LCD Display Stack

## Architecture

```
+--------------------------------------------------+
|  lcd_ui.uc (ucode)                               |
|  uloop event loop + ubus + uci                   |
|  Dashboard, menu pages, touch handling            |
+-----+--------------------------------------------+
      | JSON commands via unix socket (batched per frame)
+-----v--------------------------------------------+
|  lcd_render (C, static binary)                    |
|  Unix socket server /tmp/lcd.sock                 |
|  5x7 bitmap font, rect, clear, text              |
|  Local fb[] → write() 153KB to /dev/lcd          |
+-----+--------------------------------------------+
      | write() 153600 bytes RGB565 (fb_writing flag)
+-----v--------------------------------------------+
|  lcd_drv.ko (kernel module)                       |
|  framebuffer[] → snapshot flush_snap[] → GPIO    |
|  Render thread: flush only when !fb_writing      |
|  Boot: demoscene 3s → dmesg → userspace          |
|  SX8650 touch + PIC16 battery via palmbus I2C    |
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

## Anti-tearing буфферизация

### Проблема

LCD обновляется через GPIO bit-bang — 76800 пикселей последовательно (~100ms).
Если userspace пишет следующий кадр пока kernel выводит текущий — на экране
видны полосы (tearing) или пропадающие элементы.

### Решение: двухуровневая защита в lcd_drv.ko

**1. Snapshot buffer** (`flush_snap[]`)

```c
static u16 flush_snap[LCD_W * LCD_H];  // 153KB static buffer

static void lcd_flush_fb(void) {
    memcpy(flush_snap, framebuffer, FB_SIZE);  // atomic snapshot <0.1ms
    // GPIO output из flush_snap, не из framebuffer
    for (i = 0; i < LCD_W * LCD_H; i++)
        lcd_write_16d(flush_snap[i]);
}
```

Kernel копирует framebuffer в отдельный буфер **перед** GPIO выводом.
Userspace может писать следующий кадр в framebuffer параллельно — flush_snap не меняется.

**2. Write guard** (`fb_writing` flag)

```c
static int fb_writing = 0;

// lcd_fb_write() — вызывается из userspace write():
if (pos == 0) fb_writing = 1;      // начало кадра → блокировать flush
if (pos + cnt >= FB_SIZE) {
    fb_writing = 0;                  // кадр записан полностью
    fb_dirty = 1;                    // разрешить flush
}

// render_fn() — kernel thread:
if (fb_dirty && !fb_writing)         // flush ТОЛЬКО когда write завершён
    lcd_flush_fb();
```

Render thread **пропускает** flush пока userspace пишет кадр (не блокируется, просто msleep(50) и проверяет снова). Никаких mutex/lock — только atomic int flag.

### Поток кадра

```
lcd_ui.uc
  ↓ JSON commands (batched)
lcd_render (userspace)
  ↓ clear → rect → text → ... → flush (все в local fb[])
  ↓ lseek(0) + write(fb, 153600) — один вызов, полный кадр
lcd_drv.ko (kernel)
  ↓ copy_from_user() → framebuffer[]     fb_writing=1 → 0
  ↓ render thread: fb_dirty && !fb_writing?
  ↓ memcpy(flush_snap, framebuffer)       snapshot <0.1ms
  ↓ GPIO bit-bang из flush_snap[]          ~100ms
ILI9341 LCD
```

### Boot console

При загрузке lcd_drv.ko работает в 3 фазах:

1. **console_phase=0**: Demoscene (3 секунды) — plasma/starfield/fire
2. **console_phase=1**: dmesg console — kernel log на LCD (шрифт 5x7 kfont)
3. **console_phase=2**: Userspace — первый write() ставит `splash_active=0`

Переход 1→2 по таймеру (3*HZ). Переход 2→3 по write() из lcd_render.

## Performance

| Operation | Time |
|-----------|------|
| Full frame flush (GPIO bit-bang) | ~100ms |
| Snapshot memcpy (153KB) | <0.1ms |
| Text render (scale 2, 20 chars) | <1ms |
| Framebuffer write (153KB) | <1ms |
| Touch poll | 50ms interval |
| Dashboard full redraw | ~50ms |
