# PIC16 Battery — Итоги исследования 20 марта 2026

## Статус: READ НЕ РАБОТАЕТ

PIC firmware жива (мелодия, buzzer, ACK на все команды), но **не отправляет данные** при I2C read.

## Что проверено сегодня

### 6 стратегий bit-bang read (lcd_drv v0.37)

| # | Метод | Результат |
|---|-------|-----------|
| 0 | simple_read (без команды) | aa 54 a8 50 a0 40 80 00 |
| 1 | bat_read polling {2F,00,01} + 500ms + read | aa 54 a8 50 a0 40 80 00 |
| 2 | bat_read oneshot {2F,00,02} + 500ms + read | aa 54 a8 50 a0 40 80 00 |
| 3 | reinit WAKE(count=1) + tables + bat_read + read | aa 54 a8 50 a0 40 80 00 |
| 4 | combined write+restart+read | NACK (PIC в test mode) |
| 5 | SX8650-style SM0 (STATUS=2, CFG=0xFA) | ff ff ff ff |

**Все варианты = bus noise.** `aa 54 a8 50 a0 40 80 00` — каждый байт = предыдущий << 1 (descending bit pattern).

### Kernel i2c_transfer

| Момент | Результат |
|--------|-----------|
| EARLY (до SX8650 init, ~17s) | ret=-6 (ENXIO), PIC NACK |
| PRE-INIT (после SX8650 init) | ret=-145, PIC NACK |

Kernel i2c-mt7621 hardware timing несовместим с PIC clock stretching.

### D0/D1 регистры при boot — НЕ от PIC!

Три загрузки подряд:
```
Boot 1: D0=0xED254797 D1=0xE6013E39
Boot 2: D0=0xED250797 D1=0xC6413E39
Boot 3: D0=0x6D250797 D1=0xC6403E39
```

После kernel i2c_read на PIC (0x2A): `D0=0x00000055` (= PIC read addr 0x55, без данных).
**D0/D1 содержат данные от MT7530 PHY или другого устройства, не от PIC.**

### GPIO мониторинг I2C шины

SDA=1, SCL=1, **0 изменений за 5 секунд** — PIC не инициирует I2C транзакции самостоятельно.

## Доказанные факты

1. **PIC WRITE работает** — bit-bang GPIO с clock stretching, все команды ACK
2. **PIC ACKает read address** (0x55) через bit-bang — slave address match работает
3. **PIC НЕ загружает SSPBUF** при read — SDA остаётся в предыдущем состоянии
4. **SM0 auto mode read** = тот же bus noise что и bit-bang
5. **SM0_CFG (0x900) = READ-ONLY** на MT7621 (silicon limitation)
6. **Kernel i2c = NACK** — hardware timing несовместим с PIC
7. **D0/D1 boot data ≠ PIC** — от другого устройства на шине
8. **PIC не multi-master** — не инициирует I2C
9. **WAKE count=0 vs count=1** — разницы нет, read всё равно bus noise
10. **SX8650 touch read работает** через тот же SM0 — проблема специфична для PIC

## Почему SX8650 читается а PIC нет

- **SX8650**: стандартный I2C slave, отвечает немедленно, **без clock stretching**
- **PIC16LF1509**: clock stretching через CKP bit, firmware должна загрузить SSPBUF
- SM0 hardware не ждёт clock stretching → читает bus noise
- Bit-bang ждёт clock stretching → PIC ACKает адрес, но **не загружает данные**

## Гипотезы почему PIC не отправляет данные

### A) PIC firmware не имеет slave transmit handler
PIC ACKает адрес (MSSP hardware), но ISR не загружает SSPBUF для передачи.

### B) Нужна специфическая последовательность SM0 регистров
Стоковое ядро 3.10 на другом silicon revision могло записывать SM0_CFG (0x900) = 0xFA.
На нашем MT7621 eco:3 этот регистр READ-ONLY. Без правильного SM0_CFG PIC не видит корректный I2C timing.

### C) PIC ждёт калибровочные данные
Пустые таблицы {0x2D}/{0x2E} запускают мелодию, но не включают ADC read-back.
Нужны реальные данные калибровки (которых у нас нет).

## КРИТИЧЕСКОЕ ОТКРЫТИЕ: SM0_CFG=0xFA (дизассемблирование стокового ядра)

Реальные адреса PIC функций (подтверждено capstone дизассемблированием):
- **PIC_I2C_WRITE** @ 0x80414F90 — SM0 auto mode write
- **PIC_I2C_READ** @ 0x80414E80 — SM0 auto mode read
- **SM0 init** @ 0x80413728 — RSTCTRL reset + **SM0_CFG=0xFA** + N_CTL0=0x90640042

Адреса в almond-battr/CLAUDE.md (0x412E78/0x412F78) были **НЕВЕРНЫЕ** — это LCD/GPIO код!

### Протокол READ из стокового ядра (0x80414E80):
```
SM0_START = count - 1        (0x920)
SM0_STATUS = 1               (0x91C, read mode)
Loop:
  poll SM0_POLLSTA & 0x04    (0x918, bit 2 = read ready)
  udelay(10)
  byte = SM0_DATAIN          (0x914)
```
**Идентичен нашему `pic_read_battery_palmbus()`!** Протокол правильный.

### Ключевое отличие: SM0_CFG (0x900) = 0xFA
Стоковое ядро при SM0 init (0x8041375C) записывает:
```
addiu $v0, $zero, 0xFA
sw    $v0, 0x900($s0)     ← SM0_CFG = 0xFA
```
На нашем MT7621 eco:3 этот регистр **READ-ONLY** (всегда возвращает 0).

**SM0_CFG=0xFA задаёт I2C clock timing.** Без него SM0 использует default timing,
который не совместим с PIC16LF1509 clock stretching. SX8650 не использует clock
stretching → работает без SM0_CFG. PIC использует → не работает.

### Вывод
Стоковое ядро 3.10 работало на другом silicon revision MT7621 (возможно eco:1 или eco:2),
где SM0_CFG был записываемый. На eco:3 (наш) — READ-ONLY = silicon regression.

## ДОПОЛНЕНИЕ: NEW manual mode тоже не работает

Тест на роутере (lcd_drv):
- **SM0_CFG=0xFA** — READ-ONLY, и в manual mode тоже (0x00 всегда)
- **NEW manual mode read** (N_CTL1: START→WRITE addr→READ×8→STOP): `aa 54 a8 50 a0 40 80 00`
- Тот же bus noise что OLD auto mode и bit-bang GPIO

**Все 3 HW метода I2C × 6+ протоколов = один результат.** Проблема в PIC firmware.

## SM0_CFG — ТУПИК (все подходы проверены)

SM0_CFG (0x1E000900) = READ-ONLY на MT7621 eco:3 при ЛЮБЫХ условиях:
- Direct write → 0
- После RSTCTRL reset → 0
- После CTL1=stock → 0
- После CFG2=auto → 0
- iowrite8 → 0
- 5 разных 32-bit значений → все 0

U-Boot (стоковый) пишет SM0_CFG по адресу **0xBE100900** (Ethernet FE), не 0xBE000900 (SM0)!

Стоковое ядро 3.10 писало SM0_CFG=0xFA, но на eco:3 запись не работает.
Либо SM0_CFG не был причиной, либо стоковое ядро работало по другим причинам.

## ПРОРЫВ: Стоковое ядро РАБОТАЕТ на eco:3!

Загрузили стоковую прошивку через HTTP recovery. PIC battery read **РАБОТАЕТ**:
```
Read_Battery 0x2D  → Battery not connected (ADC=45, без батареи)
Read_Battery 0x2CF → C_Percentage 100 (ADC=719, батарея 100%)
Read_Battery 0x2CD → ADC=717 (чуть разрядилась)
```

**Вывод:** PIC firmware РАБОТАЕТ. SM0_CFG=0xFA НЕ нужен. Наша реализация SM0 read
имеет ошибку в init/протоколе. Дизассемблированный PIC_I2C_READ (0x80414E80) протокол
ИДЕНТИЧЕН нашему — значит проблема в init последовательности или порядке операций.

**Следующий шаг:** сравнить ПОЛНУЮ init последовательность стокового ядра с нашей.
Стоковое ядро: sub_412400 (main PIC worker) — init + loop.
Файл `boot_stock.txt` — полный лог загрузки стокового ядра с работающим PIC.

## Оставшиеся варианты

### 1. Загрузить стоковое ядро 3.10 через tftp initramfs
- Снять дамп ВСЕХ SM0 регистров в момент когда battery read работает
- Сравнить с нашими значениями
- Увидеть SM0_CFG реальное значение на стоковом silicon

### 2. Осциллограф на SDA/SCL
- Увидеть реальные сигналы при стоковом read vs наш read
- Определить есть ли clock stretching от PIC
- Увидеть что PIC реально выдаёт на SDA при read

### 3. Дизассемблировать PIC firmware (HEX dump через ICSP без перезаписи)
- Прочитать flash PIC через ICSP (НЕ записывать!)
- Найти slave transmit handler в firmware
- Понять протокол

### 4. Brute-force SM0_CTL1 значения
- Попробовать все комбинации CTL1 при read
- Может нужен нестандартный mode (не 0, 1, 2)

## Файлы проекта

- `final_20mart.md` — этот файл
- `modules/lcd_drv.c` — kernel module v0.37 с 6 стратегиями read
- `modules/at_cmd.c` — утилита AT-команд для Fibocom модема
- `modules/palmbus_read.c` — утилита чтения SM0 регистров
- `BATTERY_STATUS.md` — предыдущий статус (до сегодня)
- `IDA_READ_PROTOCOL.md` — полный анализ стокового протокола из IDA
