<img src="img/photo.png" width="250" alt="Securifi Almond 3S">

# Securifi Almond 3S — OpenWrt with Display & Touchscreen

Running OpenWrt 24.10.6 on Securifi Almond 3S with full hardware support: ILI9341 LCD display, SX8650 touchscreen, PIC16 battery management, LTE modem, and VPN.

## Hardware

| Component | Chip | Interface | Status |
|-----------|------|-----------|--------|
| SoC | MediaTek MT7621 (MIPS 1004Kc, 880MHz) | — | Working |
| RAM / Flash | 256 MB DDR3 / 64 MB NAND | — | Working |
| WiFi 5GHz | MT7662E | PCIe (mt7615e) | Working |
| WiFi 2.4GHz | MT7621 built-in | — | Working |
| LAN | 3x Gigabit (MT7530) | RGMII | Working |
| Display | 2.8" IPS 320x240, ILI9341 | 8-bit parallel GPIO | **Working** |
| Touchscreen | SX8650 resistive | I2C SM0, addr 0x48 | **Working** |
| LTE Modem | Fibocom L860-GL (Cat16) | miniPCIe USB MBIM | Working |
| Battery | 18650-251P 2S 3200mAh | PIC16LF1509 I2C 0x2A | **Working** |

## LCD UI

```
[data_collector]  ── JSON ──>  /tmp/lcd_data.json
[touch_poll]      ── file ──>  /tmp/.lcd_touch
                       ↓
               [lcd_ui.uc]  (ucode)
                       ↓
               [lcd_render]  ── write() ──>  /dev/lcd
                                               ↓
                                        [lcd_drv.ko]
                                   GPIO bit-bang → ILI9341
                                   I2C → SX8650 touch
                                   I2C → PIC16 battery
```

### Boot sequence

1. **Demoscene** — 3 seconds (plasma/starfield/fire)
2. **dmesg console** — kernel boot log on LCD
3. **UI** — dashboard with LTE/VPN/WiFi/Battery

### UI Pages

- **Dashboard** — LTE quality, VPN type, ping, WiFi clients, battery %, remaining time
- **Menu** — VPN, LTE, WiFi, Info, IP, MORE
- **VPN** — WireGuard / OpenVPN / L2TP selection
- **LTE** — RSRQ/Traffic/Ping graphs
- **WiFi** — connected clients with signal/traffic
- **Info** — system info, battery raw hex, kernel version
- **Screensaver** — bouncing clock + battery % + WWAN traffic

### Battery features

- Live ADC reading via PIC16 I2C (cmd 0x39 + 0x36)
- Percent from discharge curve: `bat_table_lookup(adc) * 100 / 170`
- Remaining time estimation (lookup table + linreg)
- Charge time estimation (CV-phase table)
- NO BAT detection (buf[5] bit5+bit6)
- Anti-tearing: snapshot buffer + fb_writing flag

## Building

### Firmware (OpenWrt 24.10.6 upstream, vermagic compatible)

```bash
git clone --branch v24.10.6 --depth 1 https://github.com/openwrt/openwrt.git
cd openwrt
git apply /path/to/openwrt-patch/almond3s.patch   # PR #22141

# Add lcd-gpio + lcd-ui packages (pull from this repo)
# Add uci-defaults for 192.168.11.1

wget https://downloads.openwrt.org/releases/24.10.6/targets/ramips/mt7621/config.buildinfo -O .config
# Configure single device + packages
make -j$(nproc)

# Verify vermagic matches official:
cat build_dir/target-*/linux-*/linux-*/.vermagic
# Expected: f31f6f85a36836e510d64a18a9a5f1bf
```

See [docs/BUILD.md](docs/BUILD.md) for full recipe.

### Pre-built firmware

Latest build: [out/26_04_16/](out/26_04_16/) — OpenWrt 24.10.6, vermagic compatible with upstream packages.

### Quick build (development)

```bash
./build.sh kernel      # lcd_drv.ko via build server
./build.sh userspace   # zig cc locally
./build.sh deploy      # scp to router
```

## Setup after flashing

```bash
# Flash:
sysupgrade -n -F /tmp/openwrt-24.10.6-almond3s-sysupgrade.bin
# Wait ~11 min for jffs2 init, then:

./install_to_router.sh              # full setup + VPN keys
./install_to_router.sh --public     # without private keys

# Install VPN via opkg (vermagic compatible):
opkg update
opkg install kmod-wireguard wireguard-tools luci-proto-wireguard
opkg install openvpn-openssl kmod-tun
```

## /dev/lcd Interface

| Operation | Description |
|-----------|-------------|
| `write()` | Framebuffer data (320x240 RGB565 = 153600 bytes) |
| `write "touch_start"` | Init SX8650 + start touch thread |
| `ioctl(0)` | Flush framebuffer, stop splash |
| `ioctl(1, int[3])` | Read touch: `{x, y, pressed}` |
| `ioctl(2, u8[17])` | Read PIC battery (last periodic read) |
| `ioctl(4, 0/1/2)` | Backlight: OFF / ON / splash |
| `ioctl(5, N)` | Scene select (0-5, 99=random, 100=stop) |

## Known Issues

- **Buzzer** — PIC melody player (Timer1 ISR), multi-byte SM0 write broken
- **Modem firmware** — DO NOT UPDATE (MBIM mode lost on new firmware)
- **Reboot** — PIC16 controls power, use power button

## Documentation

- [docs/BUILD.md](docs/BUILD.md) — Build system & recipe
- [docs/TODO.md](docs/TODO.md) — Roadmap
- [docs/GUIDE_SETUP_ON_OPENWRT.md](docs/GUIDE_SETUP_ON_OPENWRT.md) — Full setup guide
- [docs/LCD.md](docs/LCD.md) — ILI9341 display driver
- [docs/TOUCH.md](docs/TOUCH.md) — SX8650 touchscreen
- [docs/Fibocom_Setup.md](docs/Fibocom_Setup.md) — LTE modem
- [Battery_Drain/Battery_Drain_ALGO.md](Battery_Drain/Battery_Drain_ALGO.md) — Battery algorithm
- [docs/ideas/PIC_FIRMWARE_ANALYSIS.md](docs/ideas/PIC_FIRMWARE_ANALYSIS.md) — PIC16 firmware RE
- [docs/ideas/pic_firmware.hex](docs/ideas/pic_firmware.hex) — PIC16 firmware dump
- [openwrt-patch/almond3s.patch](openwrt-patch/almond3s.patch) — DTS patch for OpenWrt

## Credits

- Almond 3S DTS: [fildunsky/openwrt PR #22141](https://github.com/openwrt/openwrt/pull/22141)
- Display/touch protocol reverse engineered from stock firmware (kernel 3.10.14)
- PIC16 firmware dumped via [PICkit 3](docs/ideas/TODO_PICkit.md)
- Community: [4PDA forum](https://4pda.to/forum/index.php?showtopic=1116429)

## License

GPL-2.0
