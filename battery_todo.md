# PIC16 Battery — Research Log & TODO

## Текущий статус (2026-03-18)

Palmbus write калибровки **работает**. Palmbus read через новые регистры 6.12 **работает**. PIC отвечает разными данными с/без батареи и до/после калибровки. Но ответ — **echo калибровочных данных**, не battery readings.

## Что работает

| Операция | Метод | Результат |
|----------|-------|-----------|
| Калибровка write (2×401 байт) | Старые SM0 регистры (0x908-0x920) | OK, PIC ACK |
| Battery read | Новые SM0 регистры (0x944, 0x950, 0x954) | OK, данные читаются |
| Wake команда {0x33, 0x00, 0x01} | Palmbus write | OK |
| Byte-swap int16 в таблицах | До отправки | Изменяет ответ PIC |

## Ответы PIC (через новые регистры)

```
Без калибровки, с батареей:      55 00 00 00 39 3e 01 e6  (повторяется 8 байт)
Без калибровки, без батареи:     55 00 00 00 39 3e 41 e6
С byte-swap калибровкой:         55 00 00 00 39 3e 03 c6  (данные изменились!)
Без калибровки, без батареи (v1): 55 00 00 00 00 00 00 00
```

17 байт: `55 00 00 00 39 3e 03 c6 55 00 00 00 39 3e 03 c6 55` — 8 байт повторяются.

## Стоковые данные (из BATTERY.md)

```
С батареей (100%): Read_Battery 0x2CC vref 0x1 C%=100 F%=99 BatStat=1 BatCount=0
Без батареи:       Read_Battery 0x2CF vref 0x1 C%=-1  F%=-2 BatStat=1 BatCount=1
```

## Ключевые открытия из r2 дизассемблера

### PIC WRITE (0x80413F78):
```mips
lui v0, 0xbe00              # palmbus base
addiu v1, zero, 0x2a        # PIC addr
sw v1, 0x908(v0)            # SM0_DATA = 0x2A
sw a2, 0x920(v0)            # SM0_START = len (ONCE!)
sw a0, 0x910(v0)            # SM0_DATAOUT = data[0]
sw zero, 0x91c(v0)          # SM0_STATUS = 0 (write)
# Loop: poll 0x918 bit 1, udelay(1000), SM0_DATAOUT = next
```

### PIC READ (0x80413E78):
```mips
lui v0, 0xbe00
addiu v1, a1, -1            # v1 = len - 1
sw v1, 0x920(v0)            # SM0_START = len - 1
addiu v1, zero, 1
sw v1, 0x91c(v0)            # SM0_STATUS = 1 (read)
# Loop: poll 0x918 bit 2 (0x04), udelay(10), lw SM0_DATAIN (0x914)
```

### Калибровка (0x80414200):
```
1. lb s2, 4(s0)              # read count from struct
2. Wake: write {0x33, s2>>8, s2}
3. Byte-swap loop: sra a1, a0, 8; sb a1, (v0); sb a0, 1(v0) — swap each int16
4. PIC_WRITE(0x2A, buf, count+1) — table 1 (cmd=0x03)
5. udelay(5000)
6. PIC_WRITE(0x2A, buf, count+1) — table 2 (cmd=0x2E)
7. udelay(5000)
8. memcpy tables to kernel memory
9. Return — НЕТ команды активации!
```

### Регистры i2c-mt7621 на ядре 6.12:
```
Старые (3.10):              Новые (6.12):
SM0_DATA    = 0x908          -
SM0_DATAOUT = 0x910          -
SM0_DATAIN  = 0x914          -
SM0_STATUS  = 0x91C          -
SM0_START   = 0x920          -
SM0_CTL1    = 0x940         SM0CTL0 = 0x940 (совпадает!)
-                           SM0CTL1 = 0x944 (command)
-                           SM0D0   = 0x950 (data 4 bytes)
-                           SM0D1   = 0x954 (data 4 bytes)
```

Старые и новые регистры **сосуществуют**. Write через старые работает. Read через старые — нет (poll bit 2 timeout). Read через новые — работает.

## Нерешённые проблемы

### 1. PIC echo вместо battery data
PIC возвращает `55 00 00 00 39 3e 03 c6` — это НЕ формат `Read_Battery 0x%x vref...`. Похоже на echo калибровочных данных. Гипотезы:
- **А**: Калибровочные таблицы неверные (извлечены из RAM другого экземпляра)
- **Б**: Read через новые регистры читает SM0 write буфер, а не PIC ответ
- **В**: Нужна пауза между write cmd {0x2F} и read (PIC ADC conversion time)
- **Г**: Нужно читать через СТАРЫЕ регистры после правильной инициализации SM0

### 2. Байт 0x55 в каждом 8-byte chunk
`0x55` повторяется на позициях 0, 8, 16. Это может быть:
- Адрес PIC `(0x2A << 1) | 1 = 0x55` — R/W address echo в SM0D0
- Реальный первый байт данных PIC (инвертированный 0xAA?)
- Артефакт чтения — SM0D0 содержит адрес от write phase

### 3. Формат калибровочных таблиц
Таблицы извлечены из RAM дампа стоковой прошивки через /dev/mem. Byte-swap подтверждён дизассемблером. Но значения могут быть device-specific (калибровка генерируется в runtime).

## TODO

- [ ] Попробовать read через СТАРЫЕ регистры (0x914) после инициализации SM0 в old mode
- [ ] Увеличить паузу между battery write cmd и read (100ms → 1000ms)
- [ ] Попробовать read БЕЗ предварительного write cmd {0x2F} (просто read after calib)
- [ ] Проверить: SM0D0 после read содержит данные PIC или echo нашего write?
- [ ] Дизассемблировать функцию Read_Battery (искать через xref к строке "Read_Battery")
- [ ] Попробовать читать ioctl(2) через /dev/almond_pic на стоковой прошивке (загрузить сток через initramfs)
- [ ] Дампнуть PIC firmware через ICSP (pic_icsp.c) и дизассемблировать I2C slave код
- [ ] Проверить что BQ24133 charger подаёт напряжение на PIC ADC вход (мультиметром)

## Файлы

- `modules/lcd_drv.c` — калибровка + read (текущая реализация)
- `modules/pic_calib.h` — калибровочные таблицы (из RAM дампа)
- `modules/pic_icsp.c` — ICSP programmer (GPIO bit-bang)
- `modules/pic_calib_fix.c` — эксперименты с stock protocol
- `modules/sm0_dump.c` — дамп SM0 регистров
- `/tmp/kernel.bin` — распакованное стоковое ядро 3.10.14
- r2 команда: `r2 -q -a mips -b 32 -e cfg.bigendian=false -m 0x80001000 /tmp/kernel.bin`
