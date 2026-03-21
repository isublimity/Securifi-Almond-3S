# Stock Kernel Debug Dump — 20 марта 2026

## Собрано через restdebug (Go REST agent) на стоковой прошивке 3.10.14

## SM0 Регистры (стоковое ядро, PIC read РАБОТАЕТ!)

| Регистр | Offset | Значение | Комментарий |
|---------|--------|----------|-------------|
| SM0_CFG | 0x900 | **0x00000000** | READ-ONLY и на стоке тоже! |
| SM0_904 | 0x904 | 0x00000000 | |
| SM0_DATA | 0x908 | 0x00000048 | SX8650 addr (touch idle) |
| SM0_SLAVE | 0x90C | 0x00000000 | |
| SM0_DOUT | 0x910 | 0x00000093 | Touch read cmd |
| SM0_DIN | 0x914 | 0x000000FF | |
| SM0_POLL | 0x918 | 0x00000002 | |
| SM0_STAT | 0x91C | 0x00000001 | read mode |
| SM0_STRT | 0x920 | 0x00000001 | |
| SM0_CFG2 | 0x928 | **0x00000001** | auto mode |
| N_CTL0 | 0x940 | **0x8064800E** | hw-modified |
| N_CTL1 | 0x944 | 0x00000000 | |
| N_D0 | 0x950 | 0xED2507FF | |
| N_D1 | 0x954 | 0xE6013E39 | |

**SM0_CFG=0x00 на стоковом ядре!** Запись 0xFA в IDA коде — мёртвый код (write не проходит на eco:3).

## PIC I2C Transaction (пойман через poll)

### Write phase:
```
DATA=0x2A  STAT=0x00(write)  STRT=0x0001  CTL0=0x80648006  POLL=0x03
```

### Read phase (~10ms позже):
```
DATA=0x2A  STAT=0x01(read)  STRT=0x0005  CTL0=0x8064800E  DIN=0x09  POLL=0x02
```

### Ключевое отличие от нашего кода:
- **STRT=1 при write** (мы писали STRT=3 для {0x2F,0x00,0x01})
- **STRT=5 при read** (count-1=5 → 6 bytes, мы читали 8)
- Write phase = **1 byte address setup**, не полная bat_read команда
- Стоковое ядро делает: write(addr) → read(6 bytes), а не write(cmd) → wait → read

## PIC State Machine
```
0x80704D88 = 0x02  (state: bat_read done, monitoring)
0x80704D8C = 0x02
0x80704D94 = 0x54  (84 decimal — timer?)
```

## Calibration Data
```
0x81391FAC = 0xB8220002  (count=2, NOT empty!)
0x81391FBC = 0x8FA3C240  (queue pointer)
0x81391FC4 = 0x00000025  (37)
0x81391FCC = 0x00000024  (36) → 0x0000002A (42) — CHANGING!
```
**Калибровочные таблицы НЕ пустые (count=2).** Мы отправляли пустые {0x2D}/{0x2E}.

## Stock Devices
```
/dev/almond_pic       (84, 0)
/dev/almond_touch     (82, 0)
/dev/almond_lcd       (80, 0)
/dev/almond_backlight (122, 0)
/dev/almond_remind    (86, 0)
/dev/almond_link      (35, 0)
/dev/i2cM0            (218, 0)
```

## Battery Data (LIVE)
```
Without battery: ADC=0x2D (45), 0x2E (46) → "Battery not connected"
With battery:    ADC=0x2CF (719) → 100%, slowly → 0x2CD (717) → 0x2CC (716)
vref=0x01 always
```

## GPIOMODE
0x00048580 — **одинаковый** на стоковом и OpenWrt!

## Выводы

1. **SM0_CFG=0xFA НЕ влияет** — стоковое ядро тоже записать не может, но PIC read работает
2. **Калибровочные таблицы count=2** — возможно нужны для PIC read
3. **Write STRT=1** — стоковое ядро пишет 1 byte (address setup) перед read, не 3 bytes (bat_read cmd)
4. **Read count=6** (STRT=5) — мы читали 8
5. **CTL0 динамически меняется** hardware: 0x80648006 (write) → 0x8064800E (read)
