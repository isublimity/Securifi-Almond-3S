# PIC16 Battery — Research & Experiments

## PIC16LF1509

Securifi Almond 3S contains **PIC16LF1509** (Microchip) managing:
- **Power control** (power button, auto-start)
- **Battery monitoring** (2S Li-Ion, BQ24133 charger)
- **Buzzer** (plays melodies, not just ON/OFF)
- **Watchdog**

I2C bus 0 (GPIO 3, 4), address **0x2A** (write) / **0x55** (read).

## Current Status (2026-03-22)

### What WORKS

- **PIC init via SM0 auto mode** — {0x41} init + calibration tables + bat_read {0x2F,0,2}. Works on kernel 6.12.74 in lcd_drv v1.0.
- **PIC polling via NEW SM0 registers** (0x944/0x950/0x954) — stable read every 10s in touch thread. Returns `55 00 00 00 39 3e 40 e6`. Does NOT break MT7530 LAN.
- **Buzzer** — {0x34, state, 0x00}. PIC plays melodies after init.
- **SM0 auto mode WRITE** — calibration tables (2×401 bytes) sent successfully at boot. PIC ACKs all.

### What DOES NOT WORK

- **Live ADC update** — data is STATIC after init. `55 00 00 00 39 3e 40 e6` never changes regardless of battery charge state.
- Unclear what stock firmware does differently to get live ADC updates.

### Key Correction (2026-03-22)

**Previous conclusion was WRONG**: "SM0 auto mode is broken on MT7621 silicon" — FALSE.
SM0 auto mode WORKS for PIC write (calibration, bat_read commands). The problem was `gpio_request()` in modified lcd_drv.c causing IRQ #23 crash, NOT SM0 operations.

The stable lcd_drv.ko (md5 8cf0746e, from firmware build) uses SM0 auto mode for PIC init + NEW SM0 manual mode for polling — both work fine on kernel 6.12.74 with correct DTS.

## Init Sequence (lcd_drv v1.0)

```
1. {0x41}                  — PIC init (buzzer plays)
2. {0x34, 0x00, 0x00}      — buzzer OFF
3. {0x33, 0x00, 0x01}      — WAKE
4. Calibration table 1 (401 bytes, cmd 0x03, byte-swapped int16)
5. Calibration table 2 (401 bytes, cmd 0x2E, byte-swapped int16)
6. {0x2F, 0x00, 0x02}      — bat_read command
7. Wait 200ms
8. NEW SM0 read 17 bytes    — initial battery data
9. Touch thread: poll every 10s via NEW SM0 read
```

## Open Question

Stock firmware (kernel 3.10.14) had live ADC updates. Our init sends the same commands (confirmed by IDA reverse engineering) but data stays static. Possible causes:
- Different PIC firmware state (factory vs our init sequence)
- Timing/frequency of bat_read command
- Missing periodic bat_read re-trigger
- Clock stretching behavior difference between kernels

## Files

### Analysis
- `ANALYSIS_BUZZER.md` — buzzer protocol analysis
- `ANALYSIS_FINAL.md` — buzzer vs battery comparison
- `BATTERY_STATUS.md` — full PIC status (OUTDATED conclusions about SM0)
- `PROGRESS1.md` — breakthrough log (buzzer worked!)

### Reverse Engineering (stock kernel 3.10.14)
- `IDA_DEEP_ANALYSIS.md` — deep analysis of I2C functions
- `IDA_DATA_TRACE.md` — SM0 data tracing
- `IDA_READ_PROTOCOL.md` — PIC read protocol
- `IDA_BUZZER.md` — buzzer control analysis
- `PIC_FUNCTIONS_IDA.md` — all PIC functions decompiled

### Kernel Comparison
- `KERNEL_DIFF.md` — SM0 register diff (stock vs OpenWrt)
- `GPIOMODE_DISCOVERY.md` — GPIOMODE differences
- `STOCK_DUMP_20mart.md` — register dumps from stock kernel TFTP boot
- `TFTP_STOCK_BOOT.md` — how to boot stock kernel via TFTP
- `stock_dumps/` — JSON dumps (PIC state, calibration, dmesg)

### Experiment Logs
- `STEPS_BATTERY.md` — step-by-step experiment log
- `final_20mart.md`, `final_20mart_v2.md` — March 20 research notes
- `pic_emu.py` — SM0 I2C protocol emulator

### Tools (ideas/pic_tools/)
- `pic_test.c`, `pic_final.c`, `pic_newmode.c` — test utilities
- `pic_buzzer.c` — buzzer test
- `pic_bruteforce.c` — command scan
- `pic_calib_*.c` — calibration experiments

### Moved Here
- `battery_todo.md`, `BATTERY.md` — old battery research TODO
- `TODO_modem.md` — old modem TODO (Quectel EC21-E, replaced by Fibocom)
- `TODO_PICkit.md` — PICkit programming options
- `touch_plan.md` — touch + LAN coexistence plan (SOLVED: no gpio_request)
