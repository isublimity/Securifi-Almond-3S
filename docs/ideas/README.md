# PIC16 Battery — Research & Experiments

## PIC16LF1509

Securifi Almond 3S contains **PIC16LF1509** (Microchip) managing:
- **Power control** (power button, auto-start)
- **Battery monitoring** (2S Li-Ion, BQ24133 charger, AN11 ADC)
- **Buzzer** (PWM on PORTC.0, plays melodies via ISR chain)
- **LED indicator** (PORTE.4, active LOW)
- **External outputs** (PORTA.0/1, controlled via cmd 0x34)

I2C bus 0 (GPIO 3, 4), address **0x2A** (write) / **0x55** (read).

## PIC Firmware Dump (2026-03-24)

**Firmware successfully read via PICkit 3!**

- **`pic_firmware.hex`** — Intel HEX dump (44KB, 8K words program memory)
- **`pic_firmware.asm`** — Full disassembly (gpdasm, 8198 lines)
- Config Word 1: `0x09E4`, Config Word 2: `0x13FF`
- **Code Protection: OFF** — firmware fully readable
- Device Revision: 2

### How to read PIC firmware
1. PICkit 3 connected to ICSP pins on Almond 3S PCB
2. Router **fully powered off** (battery disconnected, no charger)
3. PICkit 3 supplies VDD 3.3V via `-W3.3` flag
4. **MPLAB IPE v6.05** required (v6.25+ dropped PICkit 3 support!)
5. Command:
```bash
sudo "/Applications/microchip/mplabx/v6.05/mplab_platform/mplab_ipe/bin/ipecmd.sh" \
  -P16LF1509 -TPPK3 -W3.3 -GF"pic_firmware.hex"
```

### How to disassemble
```bash
brew install gputils
gpdasm -p 16lf1509 pic_firmware.hex > pic_firmware.asm
```

## Current Status (2026-03-24)

### What WORKS
- PIC init + polling via lcd_drv.ko (touch thread, every 10s)
- NEW SM0 manual mode read returns boot snapshot (flash defaults, not live ADC)
- LED control: cmd 0x32=ON, 0x31=OFF, 0x30=BLINK (tested!)
- Charge status (byte 6) changes between boots
- OLD SM0 auto mode WRITE works for sending commands
- RSTCTRL I2C reset safe for MT7530 LAN

### Core Problem: Live ADC
- **cmd 0x36 is the ONLY way to get live ADC** (reads AN11 in ISR, stores 3 bytes)
- NEW SM0 manual mode read does not support PIC clock stretching (CKP)
- OLD SM0 auto mode read returns FF (SM0_CFG read-only on MT7621 eco:3)
- Separate pic_battery.ko module failed (SM0 sharing too fragile)

### Key Discoveries

#### Calibration Tables = MELODY DATA (NOT battery!)
- `pic_calib.h` contains LINEAR RAMP (4,5,6,7...) = GARBAGE for battery
- Flash tables at 0x0A7E/0x0ACE = PWM frequencies for melodies
- cmd 0x2D/0x2E load NOTE TABLES into RAM 0x00A0/0x0120
- cmd 0x2F: 0x0001=NOTE_PLAY, 0x0002=IDLE, 0x0003=NOTE_TABLE
- Battery ADC needs ONLY cmd 0x36 — no calibration needed!

#### Buzzer Mystery Solved
- Melody at init = ISR detects PORTE.5 LOW (shutdown) during PIC reset
- cmd 0x40/0x41 set/clear buzzer_request FLAG only
- cmd 0x34 controls PORTA pins, NOT buzzer
- No I2C command directly triggers melody — it's ISR-automatic

#### SM0CTL0 Difference
```
Stock SM0CTL0: 0x8064800E — SCL_STRETCH=0, ODRAIN=1, clk_div=0x064
Ours SM0CTL0:  0x01F3800F — SCL_STRETCH=1, ODRAIN=0, clk_div=0x1F3
```
With stock CTL0: data changed from `39 3e 43 ee` to `ff ff ff ff`

#### GPIO Mapping (confirmed from firmware analysis + testing)
| Pin | I/O | Function |
|-----|-----|----------|
| PORTE.0 | INPUT | Charger connected |
| PORTE.3 | INPUT | Charge state (TMR1 monitors) |
| PORTE.4 | OUTPUT | LED (ACTIVE LOW! bsf=OFF, bcf=ON) |
| PORTE.5 | OUTPUT | Shutdown signal |
| PORTA.0/1 | OUTPUT | External outputs (cmd 0x34) |
| PORTC.0 | OUTPUT | Buzzer PWM |
| AN11 | ADC | Battery voltage (cmd 0x36) |

## Next Steps
1. **TEST**: Write `{0x36}` via OLD SM0, delay 10ms, NEW SM0 read 3 bytes
2. **TEST**: cmd 0x37 (firmware version) — should return 0x07
3. **TEST**: SM0CTL0=0x8064800E (stock) + cmd 0x36 cycle
4. **FALLBACK**: GPIO bit-bang I2C with manual clock stretching support
5. Remove/mark calibration tables as melody data in lcd_drv

## Files

### PIC Firmware
- **`pic_firmware.hex`** — PIC16LF1509 firmware dump (Intel HEX, 44KB)
- **`pic_firmware.asm`** — Full disassembly (gpdasm, 8198 lines)

### Analysis
- **`PIC_FIRMWARE_ANALYSIS.md`** — Complete firmware analysis (1000+ lines, 30+ functions, 14 I2C commands, all RAM variables)
- **`BATTERY_22_MART.md`** — Comprehensive battery research status (updated 2026-03-24)

### Reverse Engineering (stock kernel 3.10.14)
- `IDA_DEEP_ANALYSIS.md` — Deep analysis of SM0 I2C init (disassembly)
- `IDA_DATA_TRACE.md` — SM0 data tracing (disassembly)
- `IDA_READ_PROTOCOL.md` — PIC read protocol from stock kernel (worker thread)
- `IDA_BUZZER.md` — Buzzer control analysis (disassembly)
- `PIC_FUNCTIONS_IDA.md` — All PIC-related functions from stock kernel (disassembly)

### Other
- `GPIOMODE_DISCOVERY.md` — GPIOMODE differences stock vs OpenWrt
- `STOCK_DUMP_20mart.md` — Register dumps from stock kernel
- `TFTP_STOCK_BOOT.md` — How to boot stock kernel via TFTP
- `TODO_PICkit.md` — PICkit programming options and setup
- `pic_emu.py` — PIC I2C slave emulator (Python)
- `stock_dumps/` — JSON dumps from restdebug
- `pic_tools/` — Test utilities for PIC I2C
- `debug_tools/` — restdebug, memdebug, sm0_dump
- `old_ui/` — Old Lua-based LCD UI (replaced by ucode lcd_ui.uc)
