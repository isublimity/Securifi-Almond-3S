# LCD Display Stack — mmap Architecture (A+C)

## Overview

The display stack uses a direct mmap architecture. Lua scripts access the framebuffer and hardware through `lcdlib.so` — a Lua C module that mmaps `/dev/lcd` directly. No intermediate processes or unix sockets are needed.

## Architecture

```
+---------------------------------------------+
|  Lua scripts (lcd_ui.lua, status.lua, etc.)  |
|  require("lcdlib") -- direct C calls         |
+------------------+--------------------------+
                   | lcdlib.so (Lua C module)
                   | mmap /dev/lcd framebuffer
                   | ioctl for touch, backlight
+------------------+--------------------------+
|  lcd_drv.ko (kernel module)                  |
|  - Framebuffer 153600 bytes (mmap)           |
|  - fps=0: manual flush only                  |
|  - GPIO bit-bang to ILI9341                  |
|  - Touch polling (SX8650 via palmbus I2C)    |
|  - SM0 save/restore for I2C coexistence      |
+----------------------------------------------+
```

## Components

| Component | Type | Description |
|-----------|------|-------------|
| lcd_drv.ko | kernel module | Framebuffer, GPIO bit-bang, touch, backlight, mmap support, fps=0 |
| lcdlib.so | Lua C module | mmap framebuffer + rect/text/line/clear/flush/touch/backlight |
| lcd_ui.lua | Lua script | UI: buttons, graphs, screensaver, state machine |

## lcdlib.so API

```lua
local lcd = require("lcdlib")

-- Drawing
lcd.clear(color)                    -- fill screen
lcd.rect(x, y, w, h, color)        -- filled rectangle
lcd.text(x, y, str, color, bg, scale) -- text with 5x7 font
lcd.line(x1, y1, x2, y2, color)    -- line
lcd.flush()                         -- push framebuffer to display

-- Hardware
local x, y, pressed = lcd.touch()  -- read touch coordinates
lcd.backlight(0|1)                  -- backlight on/off
lcd.splash()                        -- show built-in splash bitmap
lcd.usleep(us)                      -- microsecond delay
```

## lcd_drv.ko Interface

### /dev/lcd

- **mmap**: 153600 bytes, RGB565, 320x240. Userspace writes pixels directly
- **write()**: alternative to mmap — write full/partial framebuffer
- **ioctl(0)**: flush framebuffer to display
- **ioctl(1, int[3])**: read touch {x, y, pressed}
- **ioctl(2, u8[17])**: read PIC battery (blocked without calibration)
- **ioctl(3, u8[17])**: raw PIC read
- **ioctl(4, 0)**: backlight OFF
- **ioctl(4, 1)**: backlight ON
- **ioctl(4, 2)**: show splash bitmap

### fps=0 Mode

The kernel render thread is disabled (fps=0). Display updates happen only on explicit flush (ioctl 0). This saves CPU and gives full control to userspace.

## Building

```bash
# lcdlib.so — cross-compile on Mac
zig cc -target mipsel-linux-musleabi -O2 -shared -o lcdlib.so lcdlib.c

# Deploy to router
scp -O lcdlib.so root@192.168.11.1:/usr/lib/lua/

# Run UI
ssh root@192.168.11.1 "lua /usr/share/lcd/lcd_ui.lua &"
```

## Color Format

Colors can be specified as:

| Format | Example | Description |
|--------|---------|-------------|
| Name | `"red"` | Predefined: red, green, blue, white, black, yellow, cyan |
| Hex RGB | `"#FF0000"` | Full RGB888, converted to RGB565 |
| Raw RGB565 | `0xF800` | Direct 16-bit value |

## Font

Built-in 5x7 pixel bitmap font covering ASCII 32-126. Each character is 5 pixels wide, 7 pixels tall, with 1 pixel spacing.

| Scale | Char size | Chars per line | Lines |
|-------|-----------|---------------|-------|
| 1 | 6x8 px | 53 | 30 |
| 2 | 12x16 px | 26 | 15 |
| 3 | 18x24 px | 17 | 10 |

## Screensaver / Burn-in Protection

lcd_ui.lua implements a state machine:

```
active (UI)  --30 sec-->  screensaver (splash)  --30 sec-->  off (backlight off)
     ^                         |                                     |
     |                         v                                     v
     +--------  touch  --------+----------  touch  ------------------+
```

## Performance

| Operation | Time | Note |
|-----------|------|------|
| Full screen fill | ~1.5s | 153600 bytes via GPIO bit-bang |
| Text line (scale 2) | ~50ms | Depends on length |
| Framebuffer mmap write | <0.1ms | Direct memory access |
| Touch poll | 50ms | Kernel thread interval |
| Flush (ioctl 0) | ~1.5s | Full frame GPIO transfer |

---

## Legacy: lcd_render (JSON socket protocol)

`lcd_render` is the old userspace renderer that accepts JSON commands via unix socket. It still works for compatibility but is no longer the recommended approach.

### Running (legacy)

```bash
/tmp/lcd_render &
# Listens on /tmp/lcd.sock
```

### JSON Commands (legacy)

```bash
# From shell
echo '{"cmd":"clear","color":"black"}' | nc -U /tmp/lcd.sock

# From Python
import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect("/tmp/lcd.sock")
s.send(b'{"cmd":"text","x":10,"y":10,"text":"Hello","color":"white","size":2}\n')
s.close()
```

#### Commands

| Command | Fields | Description |
|---------|--------|-------------|
| clear | color | Fill entire screen |
| text | x, y, text, color, bg, size | Draw text (5x7 font, scale 1-5) |
| rect | x, y, w, h, color | Filled rectangle |
| flush | — | Force display update |
| fps | value | Set refresh rate (0=manual) |
