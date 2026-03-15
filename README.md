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
| LAN | 3× Gigabit (MT7530) | RGMII | Working |
| USB | 1× USB-A 2.0 | xHCI | Working |
| Display | 2.8" IPS 240×320, ILI9341 controller | 8-bit parallel 8080-II via GPIO | **Working** |
| Touchscreen | SX8650 resistive 4-wire | I2C bus 0, addr 0x48 | **Working (X+Y)** |
| LTE Modem | Quectel EC21-E (Cat1) | miniPCIe / USB | Working |
| Battery | 2S Li-Ion, BQ24133 charger | Analog (no digital interface) | Charging works |
| Power MCU | PIC16LF1509 | I2C bus 0, addr 0x2A | WIP |
| Zigbee | EM357 | UART3 (57600 baud) | Not supported (obsolete chip) |

## Display: ILI9341 via GPIO bit-bang

The display is connected via 8-bit parallel bus (Intel 8080-II protocol) using GPIO pins directly controlled through memory-mapped registers.

### GPIO Pin Mapping

| Signal | GPIO | Mask | Description |
|--------|------|------|-------------|
| D0 | 13 | 0x00002000 | Data bit 0 |
| WRX | 14 | 0x00004000 | Write strobe |
| RESET | 15 | 0x00008000 | Hardware reset (active LOW) |
| CSX | 16 | 0x00010000 | Chip Select (active LOW) |
| D/CX | 17 | 0x00020000 | Data/Command select |
| D1 | 18 | 0x00040000 | Data bit 1 |
| D2 | 22 | 0x00400000 | Data bit 2 |
| D3 | 23 | 0x00800000 | Data bit 3 |
| D4 | 24 | 0x01000000 | Data bit 4 |
| D5 | 25 | 0x02000000 | Data bit 5 |
| D6 | 26 | 0x04000000 | Data bit 6 |
| D7 | 27 | 0x08000000 | Data bit 7 |
| Backlight | 31 | 0x80000000 | Backlight enable |

### Key Technique: DIR Register Bit-Bang

The display driver uses a unique approach discovered through firmware reverse engineering:

1. GPIO DATA register (0x1E000600) is set to HIGH for all LCD pins **once** during init
2. Pin levels are controlled through the GPIO **DIR register** (0x1E000620):
   - DIR bit = 1 (output) → pin drives HIGH (since DATA=1)
   - DIR bit = 0 (input) → pin floats LOW

This bypasses the standard Linux GPIO subsystem which doesn't properly control these pins on MT7621.

### GPIOMODE

The GPIOMODE register must be set to `0x95A8` (matching the original bootloader) to enable GPIO mode for JTAG pins (GPIO 13-17).

## Touchscreen: SX8650

### Protocol

Two separate I2C transactions per reading:
- **SELECT(X) = 0x80** → read 2 bytes → X raw (0-4095)
- **SELECT(Y) = 0x81** → read 2 bytes → Y raw (0-4095)

### Calibration

```
screen_x = (4096 - raw_x) * 320 / 4096
screen_y = raw_y * 240 / 4096
```

### Init Registers

| Register | Address | Value | Description |
|----------|---------|-------|-------------|
| Reg 0 | 0x00 | 0x00 | |
| CTRL0 | 0x01 | 0x27 | 300 cps, 1.36ms powdly |
| CTRL1 | 0x02 | 0x00 | |
| CTRL2 | 0x03 | 0x2D | Settling + pen resistance |
| ChanMsk | 0x04 | 0xC0 | X + Y channels |

After register init, send PenTrg commands: `0x80` then `0x90`.

## LTE Modem

- Quectel EC21-E (LTE Cat1, max 10/5 Mbps)
- AT port: `/dev/ttyUSB2` at 115200 baud
- Bands: B1/B3/B5/B7/B8/B20 (compatible with EU operators)
- Hardware reset: GPIO 33 (active LOW)

### Useful AT Commands

```
AT+CSQ              # Signal strength
AT+CPIN?            # SIM status
AT+COPS?            # Operator
AT+QNWINFO          # Current band
AT+QCFG="band",0,44 # Lock to Band 3+7
```

## Building

### Prerequisites

- OpenWrt build system
- Branch: `almond-3s` (based on `openwrt-24.10`, kernel 6.6.x)

**Note**: Kernel 6.6.x is tested and stable. Kernel 6.12.x (OpenWrt 25.12) may work with a different U-Boot (e.g. [DragonBluep](https://github.com/DragonBluep/uboot-mt7621)) but has not been verified by us.

### Quick Build

```bash
git clone https://github.com/isublimity/openwrt_almond.git
cd openwrt_almond
git checkout almond-3s
./build-almond-3s.sh
```

### Cross-compiling C for the Router

```bash
zig cc -target mipsel-linux-musleabi -O2 -static -o binary source.c
scp -O binary root@192.168.11.1:/tmp/
```

## Flashing

### Via U-Boot Recovery

1. Power off the router
2. Set PC IP to `192.168.1.3`, netmask `255.255.255.0`
3. Hold Reset + power on
4. Open `http://192.168.1.1`
5. Upload `sysupgrade.bin`
6. Wait ~10 min, display will flash → power cycle
7. Wait 11 min for jffs2 initialization

### Via Stock Firmware (USB Flash)

1. Format USB drive as FAT32, copy `sysupgrade.bin`
2. Factory reset stock firmware (don't touch screen)
3. Telnet to `10.10.10.254:23` (admin/admin)
4. Mount and flash:
```bash
mount /dev/sda1 /mnt/storage
cd /mnt/storage
mtd_write write openwrt-*-sysupgrade.bin Kernel
```

## Kernel Modules

### lcd_drv.ko

ILI9341 display driver with framebuffer support.

- Creates `/dev/lcd` device
- Framebuffer: 320×240 RGB565 (153600 bytes)
- Write full frame: `write()` to `/dev/lcd`
- Touch: ioctl(1) returns `{x, y, pressed}`
- Auto-loads at boot

### lcd_gpio.ko

Low-level GPIO mmap helper. Creates `/dev/lcd_gpio` for userspace direct register access.

## Architecture

```
lcd_drv.ko (kernel)     — GPIO bit-bang, ILI9341 init, framebuffer, touch polling
    ↕ /dev/lcd (write framebuffer, ioctl for touch)
lcd_render (C, userspace) — text/shapes rendering, JSON commands
    ↕ unix socket /tmp/lcd.sock
Applications (Go/shell)   — monitoring, UI logic
```

## Known Issues

- **Reboot doesn't work** — PIC16 controls power. Must use power button.
- **Watchcat dangerous** — if configured to reboot on connectivity loss, router hangs. Disable immediately.
- **First boot after sysupgrade**: 11 min 13 sec for jffs2 init
- **Auto-start on power**: requires soldering hack (VDD→RC5 on PIC16)

## IRQ Optimization

Add to `/etc/rc.local` before `exit 0`:
```bash
echo 2 > "/proc/irq/29/smp_affinity"   # USB → Core1t2
echo 4 > "/proc/irq/31/smp_affinity"   # WiFi 2.4GHz → Core2t1
echo 8 > "/proc/irq/32/smp_affinity"   # WiFi 5GHz → Core2t2
```

## Source Code

- **OpenWrt fork with Almond 3S support**: [openwrt_almond/almond-3s](https://github.com/isublimity/openwrt_almond/commits/almond-3s/)
- **This documentation**: [Securifi-Almond-3S](https://github.com/isublimity/Securifi-Almond-3S)

## Detailed Documentation

- [LCD.md](LCD.md) — Display driver deep dive: ILI9341 init, GPIO bit-bang, framebuffer
- [TOUCH.md](TOUCH.md) — Touchscreen driver: SX8650 protocol, calibration, coordinates
- [LCD_RENDER.md](LCD_RENDER.md) — Userspace renderer: JSON protocol, unix socket API, examples

## Credits

- Display and touch reverse engineering from original firmware (kernel 3.10.14)
- U-Boot by [a43/fildunsky](https://github.com/fildunsky/openwrt)
- Community support from [4PDA forum](https://4pda.to/)
- OpenWrt project

## License

GPL-2.0
