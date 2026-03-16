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

## Hardware

- 2S Li-Ion battery, BQ24133 charger
- PIC16LF1509 at I2C addr 0x2A measures voltage via ADC
- Full charge: raw ADC ~0x2CC ≈ 8.4V (2S full)
- Without calibration tables, PIC returns test pattern: `AA 54 A8 50 A0 40 80`
