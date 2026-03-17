# PIC16LF1509 Battery — Reference Data

## Battery Values (stock firmware, 2026-03-17)

### With battery (100% charged, USB power)

```
Read_Battery 0x2CC  vref 0x1  LastVref 01
  c_Last_Percentage 100  f_Last_Percentage 99
  C_Percentage 100  F_Percentage 99
  CBatVal 9  FBatVal 8
  BatStat 1  BatCount 0
```

| Field | Value | Description |
|-------|-------|-------------|
| raw ADC | 0x2CC (716) | Battery voltage raw reading |
| vref | 0x1 | Reference voltage |
| C_Percentage | 100 | Coarse battery percentage |
| F_Percentage | 99 | Fine battery percentage |
| CBatVal | 9 | Coarse battery value |
| FBatVal | 8 | Fine battery value |
| BatStat | 1 | 1 = full charge detected |
| BatCount | 0 | Error counter (0 = OK) |

### Without battery (USB power only)

```
Battery not connected
Read_Battery 0x2CF  vref 0x1  LastVref 01
  c_Last_Percentage -1  f_Last_Percentage -2
  C_Percentage -1  F_Percentage -2
  CBatVal 9  FBatVal 8
  BatStat 1  BatCount 1
```

| Field | Value | Description |
|-------|-------|-------------|
| raw ADC | 0x2CF (719) | ~same as with battery (USB voltage) |
| C_Percentage | -1 | Battery not detected |
| F_Percentage | -2 | Battery not detected |
| BatStat | 1 | Not updated (stale) |
| BatCount | 1 | Error count incremented |

### Boot sequence (battery connecting)

```
1. "Battery not connected"       → C%=-1, F%=-2, BatCount=1
2. "Battery tamper detected"     → C%=100, F%=99, BatCount=0
3. "full charge detected"        → C%=100, F%=99, BatStat=1
```

## Parsing Rules

- `C_Percentage >= 0` → battery connected, value = percentage
- `C_Percentage == -1` → no battery
- `BatStat: 0=normal, 1=full charge`
- `BatCount > 0` → error/no battery

## Calibration Tables

Extracted from stock firmware RAM dump (22 MB via `/dev/mem`).

- **Table 1**: cmd=0x03, 200 × int16 big-endian, values [4..203]
- **Table 2**: cmd=0x2E, 200 × int16 big-endian, values [47..246]
- Stored in `modules/pic_calib.h`
- Sent to PIC via palmbus I2C at lcd_drv.ko boot

## Without Calibration (OpenWrt, kernel 6.12)

PIC returns fixed patterns depending on battery presence:

| State | Raw bytes | Detection |
|-------|-----------|-----------|
| Battery connected | `AA 00 15 7F FF FF FF...` | byte[1]=0x00 |
| No battery | `AA 54 A8 50 A0 40 80 00...` | byte[1]=0x54 |

- Values do NOT change during discharge (no ADC without calibration)
- PIC detects battery presence at boot, caches the state
- Calibration via Linux I2C is accepted (ACK) but not processed by PIC
- Palmbus direct (SM0_CTL1=0x90644042) write works but read fails on kernel 6.12
- SM0_CTL1 is locked by i2c-mt7621 driver — cannot switch to raw mode from userspace

## 17-Byte Response Format

```
Offset  Field               Stock example
[0-1]   raw ADC (BE)        0x02CC = 716
[2]     vref                0x01
[3]     LastVref            0x01
[4]     c_Last_Percentage   100
[5]     f_Last_Percentage   99
[6]     C_Percentage        100 (-1 = no battery)
[7]     F_Percentage        99 (-2 = no battery)
[8]     CBatVal             9
[9]     FBatVal             8
[10]    BatStat             1 (0=normal, 1=full)
[11]    BatCount            0 (0=ok, >0=error)
[12-16] reserved            0x00
```

## Stock Firmware I2C Protocol (from reverse engineering)

### Write (SM0 register sequence):
```
SM0_DATA    = 0x2A          // slave address
SM0_START   = total_len     // FULL length, set ONCE
SM0_DATAOUT = data[0]       // first byte (command)
SM0_STATUS  = 0             // write mode
for i=1..len:
    poll 0x918 bit 1        // write ready
    delay 15ms
    SM0_DATAOUT = data[i]
poll 0x918 bit 0            // completion
```

### Read:
```
SM0_START  = len - 1        // CRITICAL: len minus 1
SM0_STATUS = 1              // read mode
for i=0..len:
    poll 0x918 bit 2        // read ready (NOT bit 1!)
    delay 10us
    buf[i] = SM0_DATAIN
poll 0x918 bit 0            // completion
```

### Calibration sequence:
1. Write 401 bytes: `{0x03, data[400]...}` (table 1)
2. Delay 5ms
3. Write 401 bytes: `{0x2E, data[400]...}` (table 2)
4. Wait 1000ms
5. Write `{0x2F, 0x00, 0x02}` (battery read command)
6. Read 17 bytes (battery data)

## Hardware

- 2S Li-Ion battery, BQ24133 charger
- PIC16LF1509 at I2C addr 0x2A measures voltage via ADC
- Full charge: raw ADC ~0x2CC = 8.4V (2S full)
- ICSP pins: MCLR (RC5/pad R507), PGD, PGC, VDD, GND
- PICkit 2/3 needed for custom firmware
