# Securifi Almond 3S — OpenWrt with Display & Touchscreen

Running OpenWrt on Securifi Almond 3S with full hardware support: ILI9341 LCD display, SX8650 touchscreen, LTE modem, and PIC16 power management.

## Hardware

| Component | Chip | Interface | Status |
|-----------|------|-----------|--------|
| SoC | MediaTek MT7621 (MIPS 1004Kc, 880MHz, 4 threads) | — | Working |
| RAM | 256 MB DDR | — | Working |
| Flash | 64 MB SPI-NOR | SPI | Working |
| WiFi 2.4GHz | MT7615 | PCIe | Working |
| WiFi 5GHz | MT7615 | PCIe | Working |
| LAN | 3x Gigabit (MT7530) | RGMII | Working |
| USB | 1x USB-A 2.0 | xHCI | Working |
| Display | 2.8" IPS 240x320, ILI9341 | 8-bit parallel 8080-II via GPIO | **Working** |
| Touchscreen | SX8650 resistive 4-wire | I2C bus 0, addr 0x48 | **Working** |
| LTE Modem | Fibocom L860-GL (Cat16) | miniPCIe USB MBIM | Working |
| Battery | 2S Li-Ion, BQ24133 charger | PIC16 I2C | WIP |
| Power MCU | PIC16LF1509 | I2C bus 0, addr 0x2A | WIP |

## LCD UI Architecture

```
[data_collector]  ── JSON ──>  /tmp/lcd_data.json
                                       |
[touch_poll]      ── file ──>  /tmp/.lcd_touch
                                       |
                               [lcd_ui.uc]  (ucode: uloop + ubus + uci)
                                       |
                               JSON via unix socket
                                       |
                               [lcd_render]  ── write() ──>  /dev/lcd
                                                                |
                                                         [lcd_drv.ko]
                                                    GPIO bit-bang → ILI9341
                                                    I2C → SX8650 touch
                                                    I2C → PIC16 battery
```

### Components

| Component | Language | Description |
|-----------|----------|-------------|
| `lcd_drv.ko` | C (kernel) | Framebuffer + GPIO bit-bang + touch + PIC battery + splash animation |
| `lcd_render` | C (static) | Unix socket server, receives JSON draw commands, renders to /dev/lcd |
| `lcd_ui.uc` | ucode | UI logic: dashboard, menu, pages. Uses uloop, ubus, uci natively |
| `touch_poll` | C (static) | Touch daemon: polls ioctl, writes press events to file (latch mode) |
| `data_collector` | C (static) | Background daemon: collects LTE/WiFi/VPN/system stats every 2 sec |
| `settings.lua` | Lua table | Button layout, timing, color configuration |

### UI Pages

- **Dashboard** — LTE quality (background color), VPN status, ping, WiFi clients, uptime/mem/cpu
- **Menu** — 6 buttons (2x3 grid): VPN, LTE, WiFi, Info, IP, MORE
- **VPN** — WireGuard status + ON/OFF buttons
- **LTE** — CSQ bar, operator, BER, ping stats
- **WiFi** — SSIDs (from UCI), connected clients with signal/traffic
- **Info** — System info, kernel version (from ubus), LTE details
- **IP** — External IP, VPN route status
- **Screensaver** — Bouncing clock (anti-burn-in), then backlight off

### ucode Features Used

- `uloop` — event-driven loop (4 timers: data 2s, touch 100ms, idle 1s, burn-in 30s)
- `ubus` — system info (uptime, memory, load, board), no shell commands needed
- `uci` — WiFi config (SSIDs, encryption) directly
- `fs` — file I/O, popen for socat
- `json()` — native JSON parsing
- `localtime()` — clock display in header
- `match()` — regex for touch coordinate parsing
- Optional chaining (`d?.lte?.csq`) + nullish coalescing (`??`)

## Display: ILI9341 via GPIO bit-bang

### GPIO Pin Mapping

| Signal | GPIO | Mask | Description |
|--------|------|------|-------------|
| D0 | 13 | 0x00002000 | Data bit 0 |
| WRX | 14 | 0x00004000 | Write strobe |
| RESET | 15 | 0x00008000 | Hardware reset |
| CSX | 16 | 0x00010000 | Chip Select (active LOW) |
| D/CX | 17 | 0x00020000 | Data/Command select |
| D1 | 18 | 0x00040000 | Data bit 1 |
| D2-D7 | 22-27 | 0x0FC00000 | Data bits 2-7 |
| Backlight | 31 | 0x80000000 | Backlight enable |

### Key Technique: DIR Register Bit-Bang

GPIO DATA register is set HIGH once. Pin levels controlled through DIR register:
- DIR=1 (output) → pin HIGH
- DIR=0 (input) → pin floats LOW

Kernel GPIO pins are claimed via `gpio_request()` + `gpio_direction_output()` to prevent mt7621_gpio driver from overwriting DIR.

## lcd_drv.ko — /dev/lcd Interface

| Operation | Description |
|-----------|-------------|
| `write()` | Write framebuffer data (320x240 RGB565 = 153600 bytes) |
| `write "fps N"` | Set render thread FPS (0=manual flush) |
| `ioctl(0)` | Flush framebuffer to display, stop splash |
| `ioctl(1, int[3])` | Read touch: `{x, y, pressed}` (pixel coordinates) |
| `ioctl(2, u8[17])` | Read PIC battery data |
| `ioctl(4, 0/1/2)` | Backlight: OFF / ON / show splash |
| `ioctl(5, N)` | Scene select (0-5, 99=random, 100=stop) |

## lcd_render — JSON Socket Protocol

Listens on `/tmp/lcd.sock`. Send JSON commands via socat or netcat:

```bash
echo '{"cmd":"clear","color":"red"}' | socat - UNIX-CONNECT:/tmp/lcd.sock
echo '{"cmd":"text","x":10,"y":10,"text":"Hello","color":"white","size":2}' | socat - UNIX-CONNECT:/tmp/lcd.sock
```

| Command | Fields | Description |
|---------|--------|-------------|
| `clear` | color | Fill screen |
| `rect` | x, y, w, h, color | Filled rectangle |
| `text` | x, y, text, color, bg, size | Text (5x7 font, scale 1-5) |
| `flush` | — | Force display update |

Colors: `"red"`, `"green"`, `"blue"`, `"white"`, `"black"`, `"yellow"`, `"cyan"`, `"#RRGGBB"`, `"#XXXX"` (raw RGB565)

## Building

### Prerequisites

- [zig](https://ziglang.org) for cross-compiling userspace (on Mac/Linux)
- OpenWrt build system on a Linux server for kernel module
- SSH access to router (`root@192.168.11.1`)

### Quick Build

```bash
# Userspace only (on Mac)
zig cc -target mipsel-linux-musleabi -Os -static -o lcd_render modules/lcd_render.c
zig cc -target mipsel-linux-musleabi -Os -static -o touch_poll modules/touch_poll.c
zig cc -target mipsel-linux-musleabi -Os -static -o data_collector modules/data_collector.c

# Full build (kernel + userspace + deploy)
cp build_config.sh.example build_config.sh  # configure first
./build.sh all
./build.sh deploy-run
```

### Deploy Manually

```bash
scp -O lcd_render touch_poll data_collector root@192.168.11.1:/usr/bin/
scp -O modules/lcd_ui.uc root@192.168.11.1:/usr/bin/
ssh root@192.168.11.1 'lcd_render & data_collector & touch_poll daemon; ucode /usr/bin/lcd_ui.uc &'
```

## Flashing OpenWrt

### Via U-Boot Recovery

1. Hold Reset + power on
2. PC IP: `192.168.1.3`, open `http://192.168.1.1`
3. Upload `sysupgrade.bin`, wait ~10 min

### Via LuCI

System → Backup/Flash → Flash new firmware

## Known Issues

- **Reboot doesn't work** — PIC16 controls power. Use power button.
- **Watchcat dangerous** — can hang router with white screen. Disable.
- **First boot**: 11 min for jffs2 init on 64MB flash.
- **I2C shared bus** — lcd_drv uses mutex to share I2C between touch (SX8650) and battery (PIC16)

## Source Code

- **OpenWrt fork**: [openwrt_almond/almond-3s](https://github.com/isublimity/openwrt_almond/tree/almond-3s)
- **This repo**: [Securifi-Almond-3S](https://github.com/isublimity/Securifi-Almond-3S)

## Documentation

- [LCD.md](LCD.md) — Display driver: ILI9341 init, GPIO bit-bang
- [TOUCH.md](TOUCH.md) — Touchscreen: SX8650 protocol, calibration
- [Fibocom_Setup.md](Fibocom_Setup.md) — LTE modem setup (Fibocom L860-GL)
- [modules/lcd_ui_design.md](modules/lcd_ui_design.md) — UI architecture

## Credits

- Display and touch protocol reverse engineered from original firmware (kernel 3.10.14)
- U-Boot by [a43/fildunsky](https://github.com/fildunsky/openwrt)
- Community: [4PDA forum](https://4pda.to/)
- OpenWrt project

## License

GPL-2.0
