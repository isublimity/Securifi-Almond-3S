# ILI9341 LCD Display Driver

## Overview

The Securifi Almond 3S uses a 2.8" IPS display module (S028HQ29NN) with ILI9341 controller, connected via 8-bit parallel bus (Intel 8080-II protocol) to MT7621 GPIO pins.

The display is driven by a kernel module (`lcd_drv.ko`) that provides a framebuffer interface at `/dev/lcd`.

## Hardware Specs

| Parameter | Value |
|-----------|-------|
| Module | CDTech S028HQ29NN |
| Controller | ILI9341 |
| Resolution | 320 × 240 (landscape, MADCTL=0xA8) |
| Color depth | 16-bit RGB565 (65536 colors) |
| Interface | 8-bit parallel 8080-II |
| Backlight | 4 LEDs, common cathode, GPIO 31 |
| VCC | 2.5–3.3V |
| VDDI (I/O) | 1.65–3.3V |

## GPIO Pin Mapping

All display pins are on MT7621 GPIO bank 0 (physical base `0x1E000600`).

### Data Bus (8-bit)

| Bit | GPIO | Register Mask | Note |
|-----|------|---------------|------|
| D0 | 13 | 0x00002000 | JTAG group (must enable via GPIOMODE) |
| D1 | 18 | 0x00040000 | WDT group |
| D2 | 22 | 0x00400000 | RGMII2 group |
| D3 | 23 | 0x00800000 | RGMII2 group |
| D4 | 24 | 0x01000000 | RGMII2 group |
| D5 | 25 | 0x02000000 | RGMII2 group |
| D6 | 26 | 0x04000000 | RGMII2 group |
| D7 | 27 | 0x08000000 | RGMII2 group |

### Control Signals

| Signal | GPIO | Mask | Function |
|--------|------|------|----------|
| WRX | 14 | 0x00004000 | Write strobe (active LOW pulse) |
| RESET | 15 | 0x00008000 | Hardware reset (active LOW, hold 10ms) |
| CSX | 16 | 0x00010000 | Chip Select (active LOW) |
| D/CX | 17 | 0x00020000 | Data/Command select (0=cmd, 1=data) |
| Backlight | 31 | 0x80000000 | Backlight enable (HIGH=on) |

Combined mask for all LCD pins: `0x8FC7E000`

## DIR Register Bit-Bang Technique

Standard GPIO operations (sysfs, chardev, DSET/DCLR) **do not work** for these pins on MT7621 with OpenWrt kernel. The original firmware uses a special technique:

### How it works

1. **GPIO DATA register** (`0x1E000600`) is set to `1` for ALL LCD pins once during init
2. Pin levels are controlled through **GPIO DIR register** (`0x1E000620`):
   - `DIR = 1` (output mode) → pin outputs HIGH (since DATA=1)
   - `DIR = 0` (input mode) → pin floats → pulled LOW

### Why standard GPIO doesn't work

- sysfs GPIO: readback shows correct values, but physical pins don't toggle
- GPIO chardev (v1/v2 ioctl): returns errors or doesn't affect hardware
- Even kernel ioremap writing to DATA/DSET/DCLR doesn't toggle pins
- Only DIR register manipulation through ioremap works
- GPIO 31 (backlight) is the exception — it works through standard sysfs

### Shadow Register

The driver maintains a `shadow_dir` variable mirroring the DIR register. All pin changes go through this shadow:

```c
static u32 shadow_dir;

// Set pin HIGH
shadow_dir |= pin_mask;
__raw_writel(shadow_dir, gpio_base + 0x620);

// Set pin LOW
shadow_dir &= ~pin_mask;
__raw_writel(shadow_dir, gpio_base + 0x620);
```

## GPIOMODE Configuration

The GPIOMODE register (`0x1E000060`) must be set to `0x95A8` to match the original bootloader configuration:

| Group | Bits | Value | Mode |
|-------|------|-------|------|
| UART1 | [3:2] | 2 | mode2 (GPIO) |
| I2C | [5:4] | 2 | mode2 (GPIO for display, I2C still works) |
| UART3 | [7:6] | 2 | mode2 |
| UART2 | [9:8] | 1 | GPIO |
| JTAG | [11:10] | 1 | GPIO (frees D0, WRX, RESET, CSX, D/CX) |
| WDT | [13:12] | 1 | GPIO (frees D1) |

**Warning**: OpenWrt sets GPIOMODE to `0x48580` by default. Without `0x95A8`, display pins won't work.

## 8080-II Protocol

### Write Command

```
1. CSX → LOW (select chip)
2. D/CX → LOW (command mode)
3. Set data bits D0-D7 via DIR register
4. WRX → LOW (begin write)
5. WRX → HIGH (latch data on rising edge)
6. CSX → HIGH (deselect)
```

### Write Data

Same as command, but D/CX → HIGH (data mode) in step 2.

### Write Timing

| Parameter | Min | Unit |
|-----------|-----|------|
| Write cycle (twc) | 66 | ns |
| WRX pulse H (twrh) | 15 | ns |
| WRX pulse L (twrl) | 15 | ns |
| Data setup (tdst) | 10 | ns |
| CS setup (tcs) | 15 | ns |

## ILI9341 Initialization Sequence

Full init from original bootloader firmware:

```c
// Power Control
lcd_cmd(0xCF); lcd_dat(0x00); lcd_dat(0xC1); lcd_dat(0x30);
lcd_cmd(0xED); lcd_dat(0x64); lcd_dat(0x03); lcd_dat(0x12); lcd_dat(0x81);
lcd_cmd(0xE8); lcd_dat(0x85); lcd_dat(0x00); lcd_dat(0x78);
lcd_cmd(0xCB); lcd_dat(0x39); lcd_dat(0x2C); lcd_dat(0x00); lcd_dat(0x34); lcd_dat(0x02);
lcd_cmd(0xF7); lcd_dat(0x20);
lcd_cmd(0xEA); lcd_dat(0x00); lcd_dat(0x00);

// Voltage
lcd_cmd(0xC0); lcd_dat(0x1B);       // Power Control 1
lcd_cmd(0xC1); lcd_dat(0x11);       // Power Control 2
lcd_cmd(0xC5); lcd_dat(0x3F); lcd_dat(0x3C);  // VCOM 1
lcd_cmd(0xC7); lcd_dat(0x8E);       // VCOM 2

// Display
lcd_cmd(0x36); lcd_dat(0xA8);       // MADCTL: landscape, BGR
lcd_cmd(0x3A); lcd_dat(0x55);       // 16-bit RGB565
lcd_cmd(0xB1); lcd_dat(0x00); lcd_dat(0x15);  // Frame rate ~67Hz
lcd_cmd(0xB6); lcd_dat(0x0A); lcd_dat(0xA2);  // Display Function

// Gamma
lcd_cmd(0xF2); lcd_dat(0x00);       // Gamma disable
lcd_cmd(0x26); lcd_dat(0x01);       // Gamma curve 1
lcd_cmd(0xE0); // Positive gamma (15 values)
lcd_cmd(0xE1); // Negative gamma (15 values)

// Enable
lcd_cmd(0x11); delay(120ms);        // Sleep Out
lcd_cmd(0x29);                       // Display ON
```

## Hardware Reset Sequence

```c
// From original bootloader:
delay(100ms);
GPIO15 → HIGH (via DIR=1);  delay(10ms);
GPIO15 → LOW  (via DIR=0);  delay(10ms);   // Reset pulse
GPIO15 → HIGH (via DIR=1);  delay(120ms);  // Wait for ILI9341 init
```

## Framebuffer

The kernel module provides a 320×240×2 = 153,600 byte framebuffer in RGB565 format.

### Writing Pixels

```c
int fd = open("/dev/lcd", O_RDWR);
uint16_t frame[320 * 240];

// Fill red
for (int i = 0; i < 320*240; i++)
    frame[i] = 0xF800;  // RGB565 red

lseek(fd, 0, SEEK_SET);
write(fd, frame, sizeof(frame));
```

### RGB565 Color Format

```
Bit:  15 14 13 12 11 | 10 9 8 7 6 5 | 4 3 2 1 0
       R  R  R  R  R |  G  G G G G G | B B B B B
```

Common colors:
| Color | RGB565 |
|-------|--------|
| Red | 0xF800 |
| Green | 0x07E0 |
| Blue | 0x001F |
| White | 0xFFFF |
| Black | 0x0000 |
| Yellow | 0xFFE0 |
| Cyan | 0x07FF |

## Render Thread

The kernel module runs a render thread that copies the framebuffer to the display at configurable FPS (default: 10). This means userspace writes to `/dev/lcd` are automatically displayed.
