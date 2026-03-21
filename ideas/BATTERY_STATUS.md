# PIC16 Battery — Полный статус (2026-03-20)

## ИТОГ

**WRITE в PIC работает** — PIC получает команды, играет мелодию, ACK на все команды.
**READ из PIC НЕ работает** — все значения = bus noise (bit-shift от 0xAA).

## Доказано

### WRITE (bit-bang GPIO sysfs):
| Команда | Результат |
|---------|-----------|
| {0x33, 0x00, 0x00} WAKE | ACK (на чистом PIC) |
| {0x2D} Table1 | ACK |
| {0x2E} Table2 | ACK |
| {0x2F, 0x00, 0x01} bat_read | ACK |
| {0x34, 0x00, 0x00} buzzer off | ACK |
| {0x34, 0x00, 0x03} buzzer on | ACK |

PIC firmware ОБРАБАТЫВАЕТ команды — мелодия играет после {0x2D}+{0x2E}+{0x2F}.

### READ — ВСЕ методы возвращают bus noise:
| Метод | Результат | Проблема |
|-------|-----------|----------|
| SM0 auto mode read (OLD regs) | ff aa XX YY ff ff ff ff | XX YY = bit-shift от 0xAA |
| NEW SM0 manual mode read | aa 54 a8 50 a0 40 80 00 | Descending bit pattern |
| Kernel i2c_transfer | NACK | Не работает |
| i2cget | 0xAA | Один байт, bus echo |
| Bit-bang GPIO read | 1-bit shifted данные | PIC не загружает SSPBUF |

### "ADC значения" 423 и 591 = ФЕЙК!
```
Все "значения" = 0xAA (PIC addr echo 0x55<<1) с разным clock alignment:
0xAA >> 7 = 0x01 → bytes 01 a7 → "ADC=423"
0xAA >> 6 = 0x02 → bytes 02 4f → "ADC=591"
0xAA >> 5 = 0x05 → bytes 05 9f → "ADC=1439"
0xAA >> 4 = 0x0A → bytes 0a 3f → "ADC=2623"
0xAA >> 3 = 0x15 → bytes 15 7f → "ADC=5503"
0xAA >> 2 = 0x2A → bytes 2a ff → "ADC=11007"
```
"591 на зарядке vs 423 без" = случайное совпадение bit-shift.

## Почему READ не работает

PIC I2C slave ACKает read address (0x55), но **НЕ загружает данные в SSPBUF**.
SM0 hardware тактирует SCL, читает SDA — но PIC не drives SDA с данными.
Результат: SM0 читает pull-up/echo = 0xAA или 0xFF.

### Возможные причины:
1. **PIC firmware не распознаёт наш read протокол** — стоковое ядро могло использовать
   специфическую последовательность SM0 регистров которую мы не воспроизвели
2. **Нужна калибровка с РЕАЛЬНЫМИ данными** (не пустые таблицы) для запуска ADC read
3. **PIC отправляет данные через ДРУГОЙ механизм** (не I2C slave transmit)
4. **Timing проблема** — PIC clock stretches но SM0/bit-bang не ждёт достаточно

## Что точно известно из IDA

### Стоковый протокол:
1. **PIC_I2C_WRITE** (0x412F78): SM0_CTL0=0x2A, SM0_DATA=cmd, SM0_START=count, SM0_CTL1=0
2. **PIC_I2C_READ** (0x412E78): SM0_START=count-1, SM0_CTL1=1, poll bit 0x04, SM0_DATAIN
3. **sub_413F78** = WRITE-ONLY wrapper (НЕ Combined write+read!)
4. Чтение = ОТДЕЛЬНАЯ транзакция через state machine fall-through
5. Worker loop 500ms между write и read

### Стоковый init:
1. SM0 RSTCTRL reset
2. SM0_CTL1 = 0x90640042 (hardware меняет на 0x8064800E)
3. WAKE {0x33, count_hi, count_lo}
4. Table1 {0x2D, data...} — даже пустая при count=0
5. Table2 {0x2E, data...}
6. bat_read {0x2F, 0x00, 0x01}
7. Worker loop: 500ms delay → SM0 auto read

### SM0 регистры:
| Offset | Name | Write | Read |
|--------|------|-------|------|
| 0x900 | SM0_CFG | **READ-ONLY!** (всегда 0) | Silicon bug |
| 0x908 | SM0_DATA/CTL0 | Slave addr (0x2A) | OK |
| 0x910 | SM0_DATAOUT | Write data byte | OK |
| 0x914 | SM0_DATAIN | — | Read data (bus noise) |
| 0x918 | SM0_POLLSTA | — | Status bits |
| 0x91C | SM0_STATUS/CTL1 | 0=write, 1=read | OK |
| 0x920 | SM0_START | count (write) / count-1 (read) | OK |
| 0x928 | SM0_CFG2 | 0=manual, 1=auto | OK |
| 0x940 | N_CTL0/SM0_CTL1 | Config (hardware modifies!) | OK |

### SM0_CFG (0x900) = READ-ONLY!
Подтверждено на ОБОИХ ядрах (6.6.127 и 6.12.74).
Write 0xFA → readback 0x00. Это **silicon limitation** MT7621.
Стоковое ядро 3.10 могло писать сюда — может другой silicon revision.

## D0/D1 кэш — единственные реальные данные
При boot kernel i2c-mt7621 driver probe записывает в D0/D1 (0x950/0x954):
```
D0 = 0xED254797 → bytes: 97 47 25 ED
D1 = 0xE6013E39 → bytes: 39 3E 01 E6
```
Эти данные меняются между boots: byte 5 (3C→3E), byte 6 (00→01→41→42).
Но это BOOT SNAPSHOT, не live.

## Hardware

- **Ядро**: 6.6.127 (OpenWrt 24.10, fork/openwrt_almond)
- **SM0 Auto Mode**: СЛОМАН на silicon (6.6.127 и 6.12.74)
- **i2c-mt7621**: встроен в ядро, unbind отключает I2C clock!
- **PIC I2C addr**: 0x2A
- **SX8650 touch**: 0x48 — работает через тот же SM0
- **GPIO bit-bang**: gpio 515 (SDA), gpio 516 (SCL), GPIOMODE bit 2
- **Модем**: Fibocom L860-GL (MBIM, 2cb7:0007) — нужен umbim

## Сборка прошивки
Идёт сборка с новыми пакетами: umbim, luci-proto-mbim, zapret, AmneziaWG.
Build server: fork/openwrt_almond, branch almond-3s.

## Файлы проекта
- `BATTERY_STATUS.md` — этот файл
- `PROGRESS1.md` — хронологический лог
- `STEPS_BATTERY.md` — полная история исследования
- `ANALYSIS_BUZZER.md` — анализ мелодии
- `ANALYSIS_FINAL.md` — анализ bus noise
- `IDA_DEEP_ANALYSIS.md` — протокол из IDA
- `IDA_DATA_TRACE.md` — data path
- `IDA_READ_PROTOCOL.md` — протокол чтения
- `IDA_BUZZER.md` — бипер
- `PIC_FUNCTIONS_IDA.md` — все PIC функции
- `Fibocom_Setup.md` — настройка LTE модема
- `plan_stock.md` — устаревший план стоковой прошивки

## Следующие шаги

### Приоритет 1: Прошивка с umbim
Сборка идёт. После сборки — прошить, настроить LTE + VPN.

### Приоритет 2: PIC battery read
Нужен принципиально другой подход:
1. **Kernel i2c_transfer ПОСЛЕ init** — раньше NACK, но теперь PIC инициализирован
2. **Прерывание от PIC** — может PIC дёргает SDA как interrupt line
3. **Анализ SDA осциллографом** — увидеть что реально на шине при read
4. **Сравнение с SX8650** — touch read РАБОТАЕТ, скопировать ТОЧНЫЙ протокол для PIC
5. **Стоковое ядро initramfs** — загрузить через tftp, снять данные
