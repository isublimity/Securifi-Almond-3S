# lcd_render — Userspace Display Renderer

## Overview

`lcd_render` is a userspace C program that provides a high-level drawing API for the ILI9341 display. It renders text, shapes, and images into the kernel framebuffer (`/dev/lcd`) and accepts commands via a Unix socket using JSON protocol.

## Architecture

```
┌─────────────────────────────────────────────┐
│  Applications (Go, Python, shell, LuCI)     │
│  Send JSON commands via unix socket          │
└──────────────┬──────────────────────────────┘
               │ /tmp/lcd.sock (unix stream)
┌──────────────▼──────────────────────────────┐
│  lcd_render (C, userspace)                   │
│  - JSON parser                               │
│  - 5×7 font renderer                         │
│  - Rectangle/fill operations                 │
│  - Local framebuffer (320×240 RGB565)        │
└──────────────┬──────────────────────────────┘
               │ write() to /dev/lcd
┌──────────────▼──────────────────────────────┐
│  lcd_drv.ko (kernel module)                  │
│  - Framebuffer 153600 bytes                  │
│  - Render thread (10 fps)                    │
│  - GPIO bit-bang to ILI9341                  │
│  - Touch polling (SX8650)                    │
└─────────────────────────────────────────────┘
```

## Running

```bash
# Start lcd_render in background
/tmp/lcd_render &

# It will:
# 1. Open /dev/lcd
# 2. Draw splash screen ("by sublimity / For OpenWRT")
# 3. Listen on /tmp/lcd.sock for JSON commands
```

## JSON Protocol

### Socket Connection

```bash
# From shell
echo '{"cmd":"clear","color":"black"}' | nc -U /tmp/lcd.sock

# From Python
import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect("/tmp/lcd.sock")
s.send(b'{"cmd":"text","x":10,"y":10,"text":"Hello","color":"white","size":2}\n')
s.close()

# From Go
conn, _ := net.Dial("unix", "/tmp/lcd.sock")
conn.Write([]byte(`{"cmd":"rect","x":0,"y":0,"w":320,"h":30,"color":"#0000FF"}`))
conn.Close()
```

### Commands

#### clear — Fill entire screen

```json
{"cmd": "clear", "color": "#000000"}
{"cmd": "clear", "color": "black"}
```

#### text — Draw text

```json
{
  "cmd": "text",
  "x": 10,
  "y": 20,
  "text": "Hello World",
  "color": "#FFFFFF",
  "bg": "#000000",
  "size": 2
}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| x | int | yes | X position (0-319) |
| y | int | yes | Y position (0-239) |
| text | string | yes | Text to render. Supports `\n` for newline |
| color | string | no | Text color (default: white) |
| bg | string | no | Background color (default: black) |
| size | int | no | Scale factor 1-5 (default: 1). Character size = 5×7 × scale |

#### rect — Draw filled rectangle

```json
{
  "cmd": "rect",
  "x": 0,
  "y": 0,
  "w": 320,
  "h": 30,
  "color": "#001F"
}
```

| Field | Type | Description |
|-------|------|-------------|
| x, y | int | Top-left corner |
| w, h | int | Width and height |
| color | string | Fill color |

#### flush — Force display update

```json
{"cmd": "flush"}
```

Normally, every draw command triggers an automatic flush. Use this after a batch of commands if auto-flush is disabled.

#### fps — Set refresh rate

```json
{"cmd": "fps", "value": 15}
```

Set to 0 for manual-only flush (better for batch operations).

### Color Format

Colors can be specified as:

| Format | Example | Description |
|--------|---------|-------------|
| Name | `"red"` | Predefined: red, green, blue, white, black, yellow, cyan |
| Hex RGB | `"#FF0000"` | Full RGB888, converted to RGB565 |
| Raw RGB565 | `"0xF800"` | Direct 16-bit value |

## One-Shot Mode

For simple operations without the socket server:

```bash
# Pass JSON as command line argument
/tmp/lcd_render '{"cmd":"clear","color":"blue"}'
/tmp/lcd_render '{"cmd":"text","x":10,"y":10,"text":"Status OK","color":"green","size":3}'
```

## Font

Built-in 5×7 pixel bitmap font covering ASCII 32-126. Each character is 5 pixels wide, 7 pixels tall, with 1 pixel spacing.

At different scales:
| Scale | Char size | Chars per line | Lines |
|-------|-----------|---------------|-------|
| 1 | 6×8 px | 53 | 30 |
| 2 | 12×16 px | 26 | 15 |
| 3 | 18×24 px | 17 | 10 |

## Example: Status Dashboard

```bash
#!/bin/sh
SOCK="/tmp/lcd.sock"
send() { echo "$1" | nc -U $SOCK -q0 2>/dev/null; }

# Header
send '{"cmd":"rect","x":0,"y":0,"w":320,"h":24,"color":"#001F"}'
send '{"cmd":"text","x":10,"y":4,"text":"Almond 3S","color":"white","bg":"#001F","size":2}'

# IP Address
IP=$(ip -4 addr show br-lan | grep inet | awk '{print $2}')
send "{\"cmd\":\"text\",\"x\":10,\"y\":40,\"text\":\"IP: $IP\",\"color\":\"#07E0\",\"size\":2}"

# Signal
CSQ=$(echo -e "AT+CSQ\r" > /dev/ttyUSB2; sleep 1; cat /dev/ttyUSB2 | grep CSQ | cut -d: -f2 | cut -d, -f1)
send "{\"cmd\":\"text\",\"x\":10,\"y\":70,\"text\":\"Signal: $CSQ\",\"color\":\"yellow\",\"size\":2}"

# Uptime
UP=$(uptime | awk -F'up ' '{print $2}' | awk -F, '{print $1}')
send "{\"cmd\":\"text\",\"x\":10,\"y\":100,\"text\":\"Up: $UP\",\"color\":\"white\",\"size\":2}"
```

## Example: Touch-Reactive UI

```bash
#!/bin/sh
# Read touch via ioctl from lcd_drv
# ioctl cmd=1 returns {x, y, pressed}

while true; do
    TOUCH=$(cat /proc/lcd_touch 2>/dev/null)  # future interface
    if [ "$TOUCH" = "pressed" ]; then
        send '{"cmd":"clear","color":"red"}'
    else
        send '{"cmd":"clear","color":"black"}'
    fi
    sleep 0.1
done
```

## Touch Integration

Touch data is available through the same `/dev/lcd` device:

```c
#include <sys/ioctl.h>
#include <fcntl.h>

int fd = open("/dev/lcd", O_RDWR);
int touch[3];  // {x, y, pressed}

// Poll touch
while (1) {
    ioctl(fd, 1, touch);
    if (touch[2]) {  // pressed
        printf("Touch at %d, %d\n", touch[0], touch[1]);
        // Draw marker at touch position
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
            "{\"cmd\":\"rect\",\"x\":%d,\"y\":%d,\"w\":10,\"h\":10,\"color\":\"red\"}",
            touch[0]-5, touch[1]-5);
        // send to lcd_render...
    }
    usleep(50000);
}
```

## Building

```bash
# Cross-compile with zig
zig cc -target mipsel-linux-musleabi -O2 -static -o lcd_render lcd_render.c

# Copy to router
scp -O lcd_render root@192.168.11.1:/tmp/

# Run
ssh root@192.168.11.1 "/tmp/lcd_render &"
```

## Performance

| Operation | Time | Note |
|-----------|------|------|
| Full screen fill | ~1.5s | 153600 bytes via GPIO bit-bang |
| Text line (scale 2) | ~50ms | Depends on length |
| Framebuffer write | ~1ms | Kernel buffer copy |
| Touch poll | 50ms | Kernel thread interval |

The GPIO bit-bang is the bottleneck. Full screen refresh at 10 fps uses ~15% of one CPU core.
