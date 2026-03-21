# PIC16LF1509 Батарея — Лог исследования

## Текущий статус
- **Bit-bang I2C WRITE работает** (GPIO API + clock stretching → ACK от PIC)
- **SM0 manual mode READ работает** (данные `55 00 00 00 39 3e 42 e6`)
- **Данные статичны** в рамках сессии, но byte 6 изменился после bat_read (0x01→0x42)
- Мониторим разряд от батареи (без зарядки)

## Что работает
| Метод | Write | Read | Примечание |
|-------|-------|------|-----------|
| SM0 Auto Mode (0x900-0x920) | ❌ ЛОМАЕТ PIC | ❌ ЛОМАЕТ PIC | Переводит в test mode `aa 54 a8` |
| SM0 Manual Mode (0x944/0x950) | ❌ NACK | ✅ Работает | Единственный рабочий READ |
| Linux I2C (i2c_transfer) | ❌ NACK | ❌ NACK | Несовместимо |
| GPIO bit-bang (прямые регистры) | ❌ SDA=0 всегда | ❌ SDA=0 | DIR регистр заблокирован I2C контроллером |
| GPIO bit-bang (kernel GPIO API) | ✅ ACK! | ⚠️ 1-bit сдвиг | gpio_get_value(515) работает! Нужен clock stretching |

## Ключевые открытия

### 1. Auto mode ломает PIC на ядре 6.12
Любое обращение через SM0 auto mode registers (SM0_DATA/SM0_START/SM0_DATAOUT) переводит PIC в тестовый режим (паттерн `aa 54 a8 50 a0 40 80 00`). Сброс только физическим отключением батареи на 10+ сек.

### 2. {0x41} — НЕ корректная команда
IDA анализ стокового ядра 3.10.14 НЕ нашёл команду 0x41 ни в одной PIC функции. Отправка {0x41} переводит PIC в test mode.

### 3. Калибровочные данные загружаются в runtime
Адрес 0x8067D4C8 в стоковом ядре = всё 0xFF (BSS). Калибровка читается из NVRAM/Flash при старте, каждый роутер имеет свою.

### 4. GPIO bit-bang через kernel API
- `gpio_request(515/516)` + `gpio_direction_input/output` работают
- Прямые записи в DIR регистр (0x620) для GPIO 3,4 **игнорируются** — I2C контроллер держит пины
- `gpio_get_value(515)` возвращает **правильное значение** (SDA=1 в idle)
- Нужен `GPIOMODE |= (1<<2)` для переключения I2C→GPIO + `bb_lock` чтобы LCD gw_dir() не перезаписывал DIR

### 5. Clock stretching обязателен
PIC использует clock stretching. Без него — NACK на write. С ним:
- `{0x33,0x00,0x01}`: ACK (wake)
- `{0x33}`, `{0x2F}`, `{0x2D}`, `{0x2E}`: ACK
- `{0x2F,0x00,0x02}`: ACK (bat_read)

### 6. Bit-bang READ имеет 1-bit сдвиг
SM0 читает `55 00 00 00`, bit-bang читает `aa 54 a8 50` (каждый байт × 2). После address ACK есть лишний clock. PIC отдаёт только 1 байт через bit-bang multi-byte read (bytes 1-7 = ff).

### 7. bat_read меняет byte 6
| Сессия | Bytes 4-5 | Byte 6 | Byte 7 | Условия |
|--------|-----------|--------|--------|---------|
| USB, без bat_read | 39 3C | 00 | E6 | Первый тест |
| Батарея, без bat_read | 39 3E | 00 | E6 | После переподключения |
| Батарея+USB, без bat_read | 39 3E | 01 | E6 | Заряжается |
| Батарея, BB bat_read ACK | 39 3E | 42 | E6 | bat_read повлиял! |

## Формат данных PIC (8 байт, повторяется 2 раза + 1)
```
55 00 00 00 39 3E 42 E6
│  │  │  │  │  │  │  └─ Byte 7: 0xE6 — константа/чексумма?
│  │  │  │  │  │  └──── Byte 6: статус (00=idle, 01=USB, 42=bat_read active?)
│  │  │  │  │  └─────── Byte 5: 0x3E (62) — ADC high byte?
│  │  │  │  └────────── Byte 4: 0x39 (57) — ADC low byte?
│  │  │  └───────────── Bytes 1-3: всегда 0x00
└──│──│──────────────── Byte 0: 0x55 — header/маркер
```

## Стоковый boot flow (из IDA)
1. `sub_D1E74` — загрузка калибровочных данных из NVRAM
2. `sub_292FEC` — обработка калибровки
3. `PIC_CALIB_LOOP` — отправка калибровки в PIC (Table1 0x2D + Table2 0x2E)
4. `PIC_BAT_READ_CMD` — {0x2F, 0x00, 0x02} запуск мониторинга
5. `PIC_BATTERY_MONITOR` → `PIC_PARSE_BATTERY` — обработка через CALIB_LOOKUP

## Стоковые команды PIC (IDA)
| Cmd | Формат | Назначение |
|-----|--------|-----------|
| 0x33 | {0x33, hi, count} | WAKE / начало калибровки |
| 0x2D | {0x2D, data...} | Калибровочная таблица 1 (401 байт) |
| 0x2E | {0x2E, data...} | Калибровочная таблица 2 (401 байт) |
| 0x2F | {0x2F, 0x00, N} | BAT_READ — запуск мониторинга |
| 0x34 | {0x34, 0x00, val} | BUZZER (val=3 ON, val=0 OFF) |

## Пороги батареи (из IDA, ПОСЛЕ калибровки)
- `< 0x191 (401)` = критически низко
- `< 0x21E (542)` = нормальный заряд
- `>= 0x21E (542)` = полный заряд

## Даташит PIC16LF1509 — I2C (MSSP модуль)

### Основные факты
- MSSP = Master Synchronous Serial Port
- I2C Slave mode: **1-байтный буфер** (SSPBUF) — нет FIFO!
- Поддерживает: 7/10-bit адресацию, clock stretching, byte NACKing
- SCL/SDA = open-drain с внешним pull-up
- **MSB first** — данные передаются старшим битом вперёд
- ACK = SDA LOW на 9-м clock цикле

### Протоколы I2C (3 типа сообщений)
1. **Master Write → Slave** — master шлёт адрес+W, затем данные
2. **Master Read ← Slave** — master шлёт адрес+R, slave отдаёт данные
3. **Combined** — master шлёт Start+Write, затем Restart+Read (или несколько W/R)

### Clock Stretching (раздел 21.3.1)
- Slave **держит SCL LOW** когда не готов принять/отдать следующий байт
- Происходит **после приёма/отправки каждого бита** если SSPBUF не обработан
- Master должен **ждать** пока SCL не станет HIGH
- PIC растягивает clock когда:
  - SSPBUF полный (не прочитан firmware)
  - Firmware обрабатывает предыдущий байт
  - Возможно во время ADC conversion
- **CKP бит** (SSPxCON1<4>) — если =0, PIC держит SCL LOW. Firmware должен установить CKP=1 чтобы отпустить clock

### I2C Operation (раздел 21.4)
- Всё общение **9-bit сегментами**: 8 бит данных + 1 ACK
- **MSB first** — данные сдвигаются старшим битом
- SDAx меняется пока SCL LOW, сэмплируется по **rising edge SCL**
- SDAx и SCLx = **open-drain** при включении I2C (SSPEN). TRIS биты должны быть = input
- **Data tied to output zero** когда I2C mode включен — выход по умолчанию LOW
- SDA Hold Time: SDAHT бит в SSPxCON3 — 300ns min hold после falling edge SCL

### Формат байта (9-bit segment)
```
Master Write:  [D7 D6 D5 D4 D3 D2 D1 D0] [ACK]
                ←── Master drives SDA ──→   ←Slave

Master Read:   [D7 D6 D5 D4 D3 D2 D1 D0] [ACK]
                ←── Slave drives SDA ──→    ←Master
```
- После 8-го falling edge SCL: передатчик **отпускает SDA** (input)
- На 9-м clock: приёмник **читает/шлёт ACK** (LOW = ACK, HIGH = NACK)

### Start / Stop / Restart
- **Start**: SDA HIGH→LOW пока SCL HIGH
- **Stop**: SDA LOW→HIGH пока SCL HIGH
- **Restart**: = Start без предварительного Stop. Валиден в любой момент когда Stop валиден
  - Сбрасывает ВСЮ slave логику — как свежий Start
  - Slave готовится принять новый адрес
  - Master может адресовать тот же или другой slave
  - **КРИТИЧНО**: позволяет master сделать Write→Restart→Read БЕЗ отпускания шины!

### ACK (9-й clock)
- Приёмник тянет SDA LOW = ACK (готов к следующему байту)
- Передатчик **ОБЯЗАН отпустить SDA** на 9-м clock чтобы принять ACK
- Slave НЕ шлёт ACK если:
  - **BF бит** (Buffer Full) установлен — SSPBUF не прочитан firmware!
  - **SSPOV бит** (Overflow) установлен
- AHEN/DHEN биты в SSPxCON3 — позволяют firmware управлять ACK вручную
- **ACKTIM бит** — показывает что идёт ACK фаза (только при AHEN/DHEN)

### **ВАЖНО: Combined Message (Write→Restart→Read)**
Стоковая прошивка может использовать:
```
START → [0x54] → ACK → [0x2F] → ACK → [0x00] → ACK → [0x02] → ACK →
RESTART → [0x55] → ACK → [byte0] → ACK → [byte1] → ... → [byteN] → NACK → STOP
```
Это **Combined message** — Write cmd затем Restart+Read ответ в ОДНОЙ транзакции.
PIC firmware видит Restart как сигнал "теперь отдавай данные".
Без Restart PIC может не знать что master хочет прочитать результат!

### I2C Slave Mode (раздел 21.5)

**Адресация:**
- SSPxADD содержит slave адрес (у нашего PIC = 0x2A)
- Первый байт после Start/Restart **сравнивается** с SSPxADD
- Совпадение → загрузка в SSPxBUF → прерывание firmware
- Нет совпадения → модуль idle, никакой реакции
- В 7-bit mode LSb (R/W бит) **игнорируется** при сравнении адреса

**Slave Receive (Master Write → PIC):**
- R/W бит = 0 → slave режим приёма
- Адрес загружается в SSPxBUF и ACK отправляется
- **НЕ ACK если overflow**: BF бит (Buffer Full) или SSPOV бит установлены!
- **SSPxIF** генерируется для **каждого** принятого байта
- **SEN бит**: если =1, SCL **растягивается после каждого принятого байта**
  - Firmware должен прочитать SSPBUF, обработать, установить CKP=1
  - CKP=1 отпускает SCL → master может слать следующий байт

**Ключевое для нас (Slave Receive):**
```
Master: START → [0x54 addr+W] → [0x2F cmd] → [0x00] → [0x02] → STOP
PIC:                            ACK          ACK       ACK       ACK
        ↓ SSPxIF              ↓ SSPxIF     ↓ SSPxIF  ↓ SSPxIF
        firmware видит        cmd=0x2F     param=0   param=2
        write request         → начинает обработку bat_read
```
- PIC firmware получает **прерывание на КАЖДЫЙ байт**
- Между байтами PIC **растягивает clock** (SEN=1) пока firmware не установит CKP=1
- Наш bit-bang с clock stretching **правильно** это обрабатывает → ACK

### Ключевые выводы для нашей задачи
1. **PIC I2C slave имеет только 1-байтный буфер** — firmware обрабатывает каждый байт через прерывание
2. **Clock stretching обязателен** — SEN бит включает stretch после каждого байта. Без ожидания SCL HIGH → PIC не успевает → overflow → NACK или мусор
3. **SM0 auto mode на 6.12 не ждёт clock stretch** → PIC overflow → test mode
4. **Bit-bang с clock stretching** правильно ждёт → write ACK работает
5. **Slave Receive подтверждён** — PIC firmware ВИДИТ наши bat_read команды (byte 6 изменился)
6. **Но данные не обновляются!** PIC ждёт **Restart+Read** в той же транзакции
7. **Multi-byte bit-bang read = ff** — PIC отдаёт 1 байт, потом ff

### 7-bit Slave Receive — пошаговый протокол (раздел 21.5.2.1)
```
Шаг   Действие                                          Кто
─────────────────────────────────────────────────────────────────
1.    Start bit на шине                                  Master
2.    S бит SSPxSTAT = 1, SSPxIF (если Start interrupt)  PIC hw
3-4.  Адрес+W принят, совпал → ACK, SSPxIF              PIC hw
5-6.  firmware: очистить SSPxIF, прочитать SSPxBUF→BF=0  PIC fw
7.    Если SEN=1: firmware ставит CKP=1 → отпустить SCL  PIC fw
8.    Master шлёт байт данных                            Master
9.    PIC: ACK, SSPxIF                                   PIC hw
10.   firmware: очистить SSPxIF                           PIC fw
11.   firmware: прочитать SSPxBUF → BF=0                  PIC fw
12.   Повторить 8-11 для каждого байта                    ...
13.   Master шлёт Stop → P бит, шина idle                 Master
```

**Критическое наблюдение:**
- Шаг 7: **SEN=1 → clock stretch после КАЖДОГО принятого байта**
- PIC firmware **ДОЛЖЕН** установить CKP=1 чтобы master мог продолжить
- Между шагами 9-11 PIC **держит SCL LOW** пока firmware не обработает байт
- Наш bit-bang clock stretching (ждём SCL HIGH) **правильно** это обрабатывает
- SM0 auto mode на 6.12 **НЕ ждёт** → overflow → NACK/мусор/test mode

### Slave Transmit — как PIC отдаёт данные (раздел 21.5.3)

**Последовательность:**
```
Шаг   Действие                                          Кто
─────────────────────────────────────────────────────────────────
1.    Start/Restart на шине                              Master
2.    S бит SSPxSTAT = 1                                 PIC hw
3.    Адрес+R принят, совпал → SSPxIF                    PIC hw
4.    PIC шлёт ACK на 9-м clock                          PIC hw
5.    *** CKP автоматически = 0 → SCL LOW (stretch!) *** PIC hw
6.    firmware: очистить SSPxIF                           PIC fw
7.    firmware: прочитать SSPxBUF (адрес), BF=0           PIC fw
8.    firmware: загрузить данные в SSPxBUF                PIC fw
9.    firmware: CKP=1 → отпустить SCL                     PIC fw
10.   Master тактирует 8 бит данных из slave              Master
11.   Master шлёт ACK (SDA LOW) на 9-м clock              Master
12.   PIC: ACKSTAT = 0 (ACK), SSPxIF = 1                  PIC hw
      *** CKP автоматически = 0 → SCL LOW (stretch!) ***
13.   firmware: загрузить СЛЕДУЮЩИЙ байт в SSPxBUF        PIC fw
14.   firmware: CKP=1 → отпустить SCL                     PIC fw
15.   Повторить 10-14 для каждого байта                    ...
16.   Master шлёт NACK (SDA HIGH) → PIC idle               Master
17.   Master шлёт Stop или Restart                         Master
```

**КРИТИЧЕСКИЕ МОМЕНТЫ:**
1. **CKP=0 автоматически после ACK адреса** → SCL LOW → clock stretch
2. **Firmware ДОЛЖЕН загрузить SSPxBUF** перед CKP=1
3. **CKP=0 после КАЖДОГО ACK от master** → stretch на каждом байте
4. **NACK от master** → slave idle, clock НЕ stretch, SSPxIF всё равно set
5. **Master ОБЯЗАН послать NACK на последнем байте** иначе slave не отпустит SCL

**Почему SM0 multi-byte read работает а bit-bang нет:**
- SM0 hardware корректно шлёт ACK на 9-м clock в правильный момент
- PIC видит ACK → CKP=0 → stretch → firmware загружает следующий байт → CKP=1
- SM0 ждёт SCL HIGH (clock stretch) → читает следующий байт
- **Bit-bang**: наш gpio_direction_output(0) для ACK слишком медленный
  - Между 8-м data clock и 9-м ACK clock проходит время
  - PIC может не увидеть SDA=LOW вовремя на rising edge 9-го clock
  - PIC интерпретирует как NACK → idle → ff на остальные байты

**РЕШЕНИЕ bit-bang multi-byte read:**
- SDA LOW (ACK) должен быть установлен **ДО** rising edge 9-го SCL
- `gpio_direction_output(SDA, 0)` до `bb_scl_high()` с достаточным запасом
- Или: использовать SM0 для read (работает), bit-bang только для write

### PIC16LF1509 — характеристики чипа (из даташита page 1-5)

**Процессор:**
- 8-bit RISC, 49 инструкций, DC-20 MHz, 200ns/инструкция
- 8192 words Flash (program), 512 bytes SRAM, **128 bytes High-Endurance Flash** (HEF)
- **НЕТ EEPROM!** Вместо него HEF — 128 bytes, 100K erase/write cycles
- 16-level hardware stack, WDT 1ms-256s

**Питание:**
- PIC16**L**F1509: **1.8V - 3.6V** (наш вариант, low voltage)
- Sleep current: 20 nA @ 1.8V (!!)
- Operating: 30 µA/MHz @ 1.8V

**I/O:**
- 18 I/O pins (1 input-only)
- Sink/source 25 mA per pin
- Weak pull-ups, IOC (Interrupt-on-Change)

**I2C (MSSP):**
- SDA = **RB4** (pin 13 PDIP)
- SCL = **RB6** (pin 11 PDIP)
- 7-bit addressing, SMBus/PMBus compatible

**ADC:**
- 10-bit, 12 external channels (AN0-AN11)
- 3 internal: Fixed Voltage Ref, DAC, Temperature
- Auto acquisition, conversion during Sleep

**Таймеры:**
- Timer0: 8-bit + 8-bit prescaler
- Timer1: 16-bit (Enhanced)
- Timer2: 8-bit + prescaler + postscaler

**PWM:** 4 × 10-bit каналов

**Важно для батареи:**
- HEF (128 bytes) = место для калибровочных данных (не EEPROM!)
- ADC Auto-conversion trigger от Timer0/1/2 — PIC может автоматически мерить ADC по таймеру
- Sleep 20nA — PIC может спать между измерениями
- Temperature sensor встроен

### Pin Allocation (наш PIC на плате Almond 3S)
| PIC Pin | Phys | ADC | MSSP | PWM | Other | Almond 3S? |
|---------|------|-----|------|-----|-------|-----------|
| RA0 | 19 | AN0 | — | — | DAC1OUT, ICSPDAT | ADC батарея? |
| RA1 | 18 | AN1 | — | — | VREF+, ICSPCLK | Ref voltage? |
| RA2 | 17 | AN2 | — | PWM3 | DAC1OUT2, C1OUT | ADC/PWM? |
| RA3 | 4 | — | — | — | MCLR/VPP (input only!) | Reset |
| RA4 | 3 | AN3 | — | — | SOSCO, T1G | Oscillator? |
| RA5 | 2 | — | — | — | SOSCI | Oscillator? |
| **RB4** | **13** | AN10 | **SDA** | — | CLC3IN0 | **I2C Data → MT7621 GPIO 3** |
| RB5 | 12 | AN11 | — | — | RX/DT, CLC4IN0 | UART RX? ADC? |
| **RB6** | **11** | — | **SCL/SCK** | — | — | **I2C Clock → MT7621 GPIO 4** |
| RB7 | 10 | — | — | — | TX/CK, CLC3 | UART TX? |
| RC0 | 16 | AN4 | — | — | CLC2 | ADC? |
| RC1 | 15 | AN5 | — | PWM4 | NCO1 | PWM buzzer? |
| RC2 | 14 | AN6 | — | — | — | ADC? |
| RC3 | 7 | AN7 | — | PWM2 | CLC2IN0 | PWM? |
| RC4 | 6 | — | — | — | CWG1B, C2OUT, CLC4 | — |
| RC5 | 5 | — | — | PWM1 | CWG1A, CLC1 | PWM buzzer? |
| RC6 | 8 | AN8 | — | — | NCO1 | ADC? |
| RC7 | 9 | AN9 | SDO | — | CLC1IN1 | SPI data out? |

### I/O Ports — ключевое из даташита (раздел 11)

**PORTA (6 бит, RA0-RA5):**
- RA3 = input only (MCLR/VPP), TRIS always reads 1
- ANSELA: по умолчанию все = 1 (analog!) → firmware должен очистить для digital
- ADC каналы: AN0 (RA0), AN1 (RA1), AN2 (RA2), AN3 (RA4)

**PORTB (4 бит, RB4-RB7):**
- RB4 = **SDA** (I2C data, highest priority)
- RB6 = **SCL/SCK** (I2C clock, highest priority)
- ANSELB: RB4=AN10, RB5=AN11 — по умолчанию analog!
- **TRISB по умолчанию = 0xF0** (все input) — правильно для I2C open-drain

**PORTC (8 бит, RC0-RC7):**
- 8 каналов ADC: AN4-AN9 (RC0-RC3, RC6-RC7)
- PWM: RC1=PWM4, RC3=PWM2, RC5=PWM1
- ANSELC: по умолчанию все = 1 (analog!)
- RC7 = SDO (SPI data out)

**Важно для Almond 3S:**
- **ANSEL биты по умолчанию = analog** → PIC firmware ОБЯЗАН очистить ANSEL для digital пинов
- Buzzer скорее всего на одном из PWM выводов: **RC1 (PWM4)** или **RC5 (PWM1)**
- ADC батареи скорее всего на **RA0 (AN0)** или **RC0 (AN4)** — ближайшие к батарейному делителю
- Weak pull-ups (WPUA, WPUB) по умолчанию enabled (reset = 1)
- Interrupt-on-Change: PORTA и PORTB поддерживают IOC — PIC может просыпаться от I2C Start condition

### Flash Program Memory / HEF (раздел 10)

**Нет EEPROM!** PIC16LF1509 использует:
- **Program Flash**: 8192 words (14-bit), для кода
- **HEF (High-Endurance Flash)**: последние 128 bytes program flash, 100K erase/write
- **User IDs**: 4 words at 0x8000-0x8003

**HEF = место для калибровки:**
- Последние 128 байт flash = HEF (адреса ~0x1F80-0x1FFF)
- 100K циклов записи — достаточно для периодического обновления калибровки
- Калибровочные данные (400 байт × 2 таблицы = 800 байт) НЕ помещаются в HEF!
- Значит калибровка хранится либо в обычном Flash (8K words) либо получается от MT7621

**Flash операции:**
- Read: PMADRH:PMADRL → RD bit → 2 NOP → PMDATH:PMDATL
- Write: unlock sequence (0x55 → PMCON2, 0xAA → PMCON2, WR=1), 2ms erase/write
- Erase: по рядам (32 words), 2ms typical
- **Запись во flash стоит ~2ms** — PIC stalls (clock stretching I2C!)

**Важно для нас:**
1. Калибровка 800 байт > 128 bytes HEF → хранится в основном Flash или приходит от MT7621
2. Flash write = 2ms stall → PIC может stretch I2C на 2ms при записи калибровки
3. User IDs (4 words) — могут содержать серийный номер устройства
4. **CONFIG2 WRT bits** — определяют защиту flash от записи. Если firmware write-protected, калибровка может храниться только в HEF

### Вывод по хранению калибровки
Стоковый flow из IDA: `sub_D1E74` → `sub_296938(0x8067D4C8)` → загрузка калибровки.
Адрес 0x8067D4C8 в ядре = 0xFF (BSS). Данные грузятся в runtime.

**Три варианта откуда:**
1. Из PIC Flash/HEF → MT7621 читает калибровку из PIC при старте (через I2C read)
2. Из NVRAM роутера → сохранено при производстве
3. Вычисляется → firmware генерирует таблицу из базовых параметров

Вариант 1 наиболее вероятен — стоковый kernel читает 400 байт из PIC, потом отправляет обратно (byte-swapped). Это объясняет зачем PIC_CALIB_LOOP byte-swaps данные.

### 10-bit Addressing (раздел 21.5.4-21.5.5)
- НЕ используется нашим PIC (у нас 7-bit адрес 0x2A)

### General Call (раздел 21.5.8)
- Адрес 0x00 — broadcast всем slave
- GCEN бит SSPxCON2 — автоматический ACK на general call
- Не релевантно для нас

### SSPx Mask (раздел 21.5.9)
- SSPxMSK — маска для адреса: 0 = don't care для соответствующего бита
- Reset = все 1 (точное совпадение)
- Не релевантно для нас

### I2C Master Mode (раздел 21.6) — ВАЖНО для понимания протокола
- **Firmware Controlled Master mode**: user code управляет SDA/SCL напрямую
  - Start/Stop detection — единственное что hardware делает
  - Всё остальное — software bit-bang!
  - **Это ИМЕННО то что делает наш bit-bang на MT7621!**
- SSPxIF устанавливается при:
  - Start condition detected
  - Stop condition detected
  - Data byte transmitted/received
  - ACK transmitted/received
  - **Repeated Start generated** ← PIC firmware получает interrupt на Restart!

### Clock Arbitration (раздел 21.6.2)
- Master отпускает SCL → BRG (Baud Rate Generator) останавливается
- BRG ждёт пока SCL **реально** станет HIGH (slave может держать LOW = stretch)
- Когда SCL HIGH → BRG перезагружается из SSPxADD и считает
- **SCL HIGH time всегда ≥ 1 BRG count** — даже если slave stretches

### WCOL (раздел 21.6.3)
- Запись в SSPxBUF во время активной операции → WCOL бит = 1, запись **игнорируется**
- Queuing не поддерживается — нельзя писать в SSPxCON2 пока Start не завершён

### Master Start Condition (раздел 21.6.4)
- SEN бит SSPxCON2 = 1 → инициация Start
- Проверка: SDA и SCL оба HIGH → BRG начинает счёт
- По таймауту BRG → SDA driven LOW (пока SCL HIGH) = **Start condition**
- S бит SSPxSTAT = 1
- BRG считает ещё раз → SEN автоматически = 0
- Если SDA или SCL LOW в начале → **Bus Collision** (BCLxIF = 1, Start abort)

### Repeated Start Timing (раздел 21.6.5)
Последовательность сигналов при Restart:
```
1. SCL driven LOW (assert)
2. BRG загружается, начинает счёт
3. SDA отпускается HIGH на 1 TBRG
4. SDA сэмплируется HIGH → SCL отпускается HIGH
5. SCL сэмплируется HIGH → BRG перезагружается
6. SDA и SCL оба HIGH на 1 TBRG
7. SDA driven LOW (пока SCL HIGH) = Restart condition!
8. SCL driven LOW
9. RSEN бит автоматически = 0
10. S бит SSPxSTAT = 1 (Start detected)
```
**Для нашего bit-bang Restart:**
```c
// После Write, SCL уже LOW
bb_sda_high();    // 3: SDA HIGH
udelay(BB_DELAY);
bb_scl_high();    // 4-5: SCL HIGH (wait stretch)
udelay(BB_DELAY); // 6: оба HIGH
bb_sda_low();     // 7: SDA LOW пока SCL HIGH = RESTART!
udelay(BB_DELAY);
bb_scl_low();     // 8: SCL LOW — готов к адресу
```

### Master Transmit (раздел 21.6.6)
- Запись в SSPxBUF → BF=1 → BRG начинает → data shifted на falling edge SCL
- SCL LOW = 1 TBRG, SCL HIGH = 1 TBRG
- После 8 бит: master отпускает SDA, slave шлёт ACK на 9-м clock
- **ACK сэмплируется на rising edge 9-го clock** → ACKSTAT
- После 9-го clock: SSPxIF = 1, BRG остановлен, SCL LOW

**Typical Master Transmit + Restart + Read (Combined Message):**
```
Шаг   Действие
──────────────────────────────────────────────
1.    SEN=1 → Start condition
2.    SSPxIF set → clear
3.    Write addr+W (0x54) в SSPxBUF → shift out → ACK from slave
4.    Write cmd (0x2F) в SSPxBUF → shift out → ACK from slave
5.    Write param1 (0x00) → ACK
6.    Write param2 (0x02) → ACK
7.    RSEN=1 → Repeated Start  ← КЛЮЧЕВОЙ МОМЕНТ!
8.    SSPxIF set → clear
9.    Write addr+R (0x55) в SSPxBUF → shift out → ACK from slave
10.   Slave stretches clock, загружает данные
11.   Master reads 8 bytes, ACK каждый, NACK последний
12.   PEN=1 → Stop condition
```

**Это EXACTLY то что нам нужно реализовать в bit-bang!**
PIC slave видит Restart (шаг 7) → сбрасывает slave logic → новый адрес+R → переходит в Slave Transmit mode → отдаёт результат bat_read

### Master Receive (раздел 21.6.7)
- **RCEN=1** → master начинает приём (BRG тактирует SCL, данные в SSPxSR)
- После 8-го falling edge SCL:
  - RCEN автоматически = 0
  - SSPxSR → SSPxBUF, BF = 1, SSPxIF = 1
  - **BRG остановлен, SCL LOW** — master idle
- Master читает SSPxBUF → BF = 0
- Master решает ACK/NACK: ACKDT + ACKEN → ACK clocked out
- **SSPOV**: если BF уже = 1 при новом приёме → overflow, данные потеряны

**Typical Master Combined (Write→Restart→Read) полный протокол:**
```
Шаг   Master                          Slave (PIC)
───────────────────────────────────────────────────────────────────
1.    SEN=1 → Start
2.    SSPxBUF = 0x54 (addr+W)         → ACK, SSPxIF, firmware видит Write req
3.    SSPxBUF = 0x2F (cmd)            → ACK, SSPxIF, firmware: cmd=bat_read
4.    SSPxBUF = 0x00 (param1)         → ACK, SSPxIF, firmware: param1=0
5.    SSPxBUF = 0x02 (param2)         → ACK, SSPxIF, firmware: param2=2
      *** PIC firmware знает: bat_read(0, 2) — запустить ADC ***
6.    RSEN=1 → Repeated Start         → S бит, slave logic reset
7.    SSPxBUF = 0x55 (addr+R)         → ACK, CKP=0, SCL stretch
      *** PIC firmware: загрузить byte0 в SSPxBUF, CKP=1 ***
8.    RCEN=1 → clock 8 bits in        ← byte0 shifted out by slave
9.    Master: ACKDT=0, ACKEN=1 → ACK  → PIC: CKP=0, загрузить byte1, CKP=1
10.   RCEN=1 → clock 8 bits           ← byte1
11.   ... повторить для всех байт ...
12.   Master: ACKDT=1 (NACK)          → PIC: idle, отпустить шину
13.   PEN=1 → Stop
```

**Ключевое отличие от нашего текущего подхода:**
- Сейчас: Write → **Stop** → Start → Read (две отдельные транзакции)
- Нужно: Write → **Restart** → Read (одна Combined транзакция)
- PIC firmware при Stop после Write: обработал команду, но результат **не привязан** к следующему Read
- PIC firmware при Restart: обработал команду И **сразу отдаёт результат** в той же сессии

### AHEN/DHEN — Hold после 8-го бита (до ACK)
- AHEN = Address Hold: interrupt + clock stretch после 8-го бита адреса (ДО ACK)
- DHEN = Data Hold: interrupt + clock stretch после 8-го бита данных (ДО ACK)
- firmware может решить ACK или NACK через бит ACKDT
- PMBus support — firmware контролирует каждый ACK вручную
- **Если NACK → SSPxIF НЕ устанавливается** — slave молча игнорирует

## Нерешённые вопросы

### Нужно из даташита ещё:
1. **MSSP Slave Transmit** (раздел 21.4.3) — точная последовательность: когда PIC загружает SSPBUF? Когда CKP сбрасывается? Когда slave может NACK?
2. **ADC** — разрешение (10-bit?), каналы, ADRESH:ADRESL формат, время конверсии
3. **EEPROM** — есть ли на LF1509, размер

### Для нас:
- Почему SM0 read данные статичны? Нужна ли калибровка для ADC polling?
- bat_read (ACK через bit-bang) изменил byte 6 (0x01→0x42) — что это значит?
- Как декодировать bytes 4-5 (0x39 0x3E) в напряжение/процент?
- Нужно ли Combined message (Write bat_read + Restart + Read) вместо отдельных W и R?

### ACK Sequence Timing (раздел 21.6.8)
- ACKEN=1 → SCL LOW, ACKDT значение на SDA
- BRG count → SCL HIGH (clock arbitration) → BRG count → SCL LOW
- ACKEN автоматически = 0, module idle
- **ACKDT=0** → ACK (SDA LOW), **ACKDT=1** → NACK (SDA HIGH)
- NACK на последнем байте обязателен — сигнал slave что передача окончена

### Stop Condition Timing (раздел 21.6.9)
- PEN=1 → после 9-го clock SCL LOW → SDA driven LOW
- BRG count → SCL HIGH → TBRG → SDA HIGH (= Stop condition)
- P бит SSPxSTAT = 1, PEN автоматически = 0, SSPxIF = 1

### Sleep Operation (раздел 21.6.10)
- **I2C slave может работать во Sleep!**
- Приём адреса/данных, при совпадении — wake-up процессора
- Значит PIC может спать между опросами батареи и просыпаться от I2C

### Reset (раздел 21.6.11)
- Reset отключает MSSP и прерывает текущий transfer
- PIC после power cycle: MSSP выключен, нужна реинициализация firmware

### Multi-Master (разделы 21.6.12-21.6.13)
- Bus collision detection через BCLxIF
- Арбитраж: если master шлёт 1, а SDA = 0 → collision → reset I2C to idle
- После collision: BF cleared, SDA/SCL released, SSPxBUF можно писать
- Не релевантно для нас (единственный master)

### Bus Collision при Start/Restart (раздел 21.6.13.1-21.6.13.2)
- **Start collision**: если SDA или SCL уже LOW → abort, BCLxIF=1, module idle
- **Restart collision case 1**: SDA LOW когда SCL идёт HIGH → другой master шлёт 0
- **Restart collision case 2**: SCL LOW до SDA LOW → другой master шлёт 1
- Не релевантно (единственный master) но важно: bit-bang должен проверять SDA/SCL перед Start/Restart

### Baud Rate Generator (раздел 21.7)
- SSPxADD = reload value для BRG
- BRG начинает считать при записи в SSPxBUF
- Определяет SCL частоту: `FCLOCK = FOSC / (4 * (SSPxADD + 1))`
- Не релевантно для нас (мы bit-bang, не PIC master)

---

## ADC модуль PIC16LF1509 (раздел 15)

### Основные характеристики
- **10-bit** ADC (не 12-bit!)
- Successive approximation
- Результат: **ADRESH:ADRESL** (2 байта, 10 бит)
- Формат: **ADFM бит** — left justified или right justified
  - Right justified: `ADRESH[1:0] : ADRESL[7:0]` = 10 бит
  - Left justified: `ADRESH[7:0] : ADRESL[7:6]` = 10 бит
- **15 каналов**: AN0-AN11, Temperature, DAC1, FVR
- Voltage reference: VDD/VSS или внешний VREF+

### Конверсия
- **ADON=1** → включить ADC
- **GO/DONE=1** → старт конверсии
- Завершение: GO/DONE=0, ADIF=1, ADRESH:ADRESL обновлены
- Время: **11.5 TAD** для полной 10-bit конверсии
- TAD зависит от clock source (FOSC/2..FOSC/64 или FRC)

### Sleep операция
- ADC может работать **во Sleep** (только с FRC clock)
- Уменьшает системный шум
- Прерывание ADIF пробуждает процессор
- **PIC может спать и периодически делать ADC по таймеру!**

### Auto-Conversion Trigger
- Timer0/Timer1/Timer2 overflow → автоматический старт ADC
- Без участия firmware — hardware periodic measurement
- **PIC firmware может настроить Timer + ADC** для автоматического периодического измерения батареи

### Что это значит для наших данных
- Bytes 4-5 в ответе PIC (`0x39 0x3E`) могут быть **10-bit ADC** в каком-то формате:
  - Right justified: `0x039E` = 926 (10-bit max = 1023)
  - Или `0x393E` = 14654 (16-bit, маловероятно)
  - Или byte 4 = high, byte 5 = low: `0x393E` → old bytes, not 10-bit
  - Или **только byte 5** = ADRESL (8 low bits), byte 4 = ADRESH (2 high bits): `0x39` & 0x03 = 0x01, + `0x3E` = 0x013E = 318
  - Или полный 16-bit с калибровкой
- **Без знания PIC firmware** точный формат неизвестен
- **Пороги из IDA** (401, 542) — это ПОСЛЕ калибровки, не raw ADC
- 10-bit ADC max = 1023 → если VDD = 3.3V → 1 LSB ≈ 3.2mV

### Декодирование Combined Message данных

**Hybrid Combined init вернул: `97 07 25 e5 39 3e 40 e6`**
**Standalone SM0 read:           `55 00 00 00 39 3e 40 e6`**

Bytes 0-3 РАЗНЫЕ! Combined возвращает свежие данные из PIC.

**Попытка декодирования bytes 0-1 (0x97 0x07) как 10-bit ADC:**
- ADFM=1 (right justified): ADRESH[1:0]=0x97 & 0x03 = 0x03, ADRESL=0x07 → **0x0307 = 775**
  - 775/1023 × 100% = **75.8%** заряда (между 542=full и 401=critical)
- ADFM=0 (left justified): ADRESH=0x97=151→bits[9:2], ADRESL=0x07>>6=0 → **604**
  - 604/1023 = 59% или 604 > 542 = полный по IDA порогам

**Bytes 2-3 (0x25 0xE5) — второй ADC канал?:**
- Right justified: 0x25 & 0x03 = 0x01, 0xE5 → **0x01E5 = 485**
  - 485 между 401 и 542 — нормальный уровень
- Может быть Vref или температура

**Bytes 4-7 (`39 3e 40 e6`) — одинаковые в обоих чтениях:**
- Byte 6: 0x40 = 64 — status/flags
- Byte 7: 0xE6 = 230 — константа/checksum

### ВЫВОД: Combined Message — единственный способ получить СВЕЖИЕ ADC данные!
- Standalone Read: `55 00 00 00` = статический header
- Combined (Write bat_read → Restart → Read): `97 07 25 e5` = свежие ADC
- **Нужно периодически делать Combined reads и отслеживать bytes 0-1!**

---

## ИТОГОВЫЙ АНАЛИЗ И ПЛАН

### Почему данные PIC статичны — корневая причина
На основе даташита, PIC I2C slave работает так:
1. **Write transaction** (bat_read cmd): PIC firmware получает прерывание на каждый байт, обрабатывает команду
2. **Stop после Write**: PIC firmware запускает ADC/обработку, результат готов через N мс
3. **Отдельная Read transaction**: PIC отдаёт данные из **текущего буфера** — который был заполнен при boot, а не после bat_read!

**Проблема**: наш Write и Read — **отдельные транзакции** с Stop между ними. PIC firmware не связывает их.

### Решение: Combined Message (Write → Restart → Read)
```
START → [0x54] → {0x2F,0x00,0x02} → RESTART → [0x55] → read N bytes → NACK → STOP
          addr+W   bat_read cmd       без Stop!    addr+R    результат
```
- PIC firmware при Restart: сбрасывает slave logic, видит новый addr+R
- Firmware знает что ТОЛЬКО ЧТО получил bat_read → загружает свежие данные
- Master читает обновлённые данные в той же сессии

### Что нужно реализовать (bit-bang)
```c
void pic_combined_bat_read(u8 *resp, int len) {
    bb_acquire();

    // 1. Write phase
    bb_i2c_start();
    bb_i2c_write_byte((PIC_ADDR << 1) | 0);  // addr+W = 0x54
    bb_i2c_write_byte(0x2F);                   // bat_read cmd
    bb_i2c_write_byte(0x00);                   // param1
    bb_i2c_write_byte(0x02);                   // param2

    // 2. Restart (NOT stop!)
    bb_i2c_restart();                           // SDA↑ SCL↑ SDA↓ SCL↓

    // 3. Read phase
    bb_i2c_write_byte((PIC_ADDR << 1) | 1);  // addr+R = 0x55
    for (int i = 0; i < len; i++)
        resp[i] = bb_i2c_read_byte(i < len-1); // ACK all, NACK last

    // 4. Stop
    bb_i2c_stop();
    bb_release();
}
```

### Оставшиеся проблемы для решения
1. **Bit-bang multi-byte read**: ff после byte 0 — ACK timing.
   - Даташит: slave CKP=0 после ACK (stretch), firmware загружает SSPxBUF, CKP=1
   - Наш ACK через `gpio_direction_output(0)` может быть OK — PIC должен stretch SCL
   - **Проверить**: ждём ли мы SCL HIGH после ACK? (clock stretching в bb_scl_high)
   - Возможно нужно: ACK → SCL LOW → SCL HIGH (wait stretch!) → read next byte
2. **1-bit сдвиг** при bit-bang read — вероятно лишний clock после address ACK
3. **SM0 read работает** для multi-byte — можно гибрид: bit-bang Write+Restart, SM0 Read

## ПЛАН РЕАЛИЗАЦИИ (v2 — Combined Message)

### Шаг 1: bb_i2c_restart() — новая функция
```c
static void bb_i2c_restart(void) {
    // SCL уже LOW после последнего байта
    bb_sda_high();        // SDA HIGH
    udelay(BB_DELAY);
    bb_scl_high();        // SCL HIGH (wait clock stretch)
    udelay(BB_DELAY);
    bb_sda_low();         // SDA LOW пока SCL HIGH = RESTART
    udelay(BB_DELAY);
    bb_scl_low();         // SCL LOW — готов к адресу
}
```

### Шаг 2: Combined bat_read — Write+Restart+Read
```c
static int pic_combined_read(u8 *cmd, int cmd_len, u8 *resp, int resp_len) {
    int i, ok = 1;
    bb_acquire();

    // Write phase
    bb_i2c_start();
    if (!bb_i2c_write_byte((PIC_ADDR << 1) | 0)) { ok = 0; goto done; }
    for (i = 0; i < cmd_len; i++) {
        if (!bb_i2c_write_byte(cmd[i])) { ok = 0; goto done; }
    }

    // Restart (NOT stop!)
    bb_i2c_restart();

    // Read phase
    if (!bb_i2c_write_byte((PIC_ADDR << 1) | 1)) { ok = 0; goto done; }
    for (i = 0; i < resp_len; i++)
        resp[i] = bb_i2c_read_byte(i < resp_len - 1);  // ACK all, NACK last

done:
    bb_i2c_stop();
    bb_release();
    return ok;
}
```

### Шаг 3: Вызов в touch thread
```c
// Каждые ~10 сек
u8 cmd[3] = { 0x2F, 0x00, 0x02 };
u8 resp[8] = {0};
if (pic_combined_read(cmd, 3, resp, 8)) {
    pr_info("lcd_drv: PIC BAT: %02x %02x %02x %02x %02x %02x %02x %02x\n", ...);
}
```

### Шаг 4: Починить bit-bang multi-byte read
Проблема: byte 0 = OK, bytes 1-7 = ff (PIC не видит ACK).
Причина из даташита: после ACK slave CKP=0 → SCL stretch → firmware загружает SSPxBUF → CKP=1.
Наш ACK:
```c
gpio_direction_output(SDA, 0);  // SDA LOW = ACK
udelay(5);
bb_scl_high();                   // SCL HIGH — PIC читает ACK
bb_scl_low();                    // SCL LOW → PIC CKP=0, stretch
// ЗДЕСЬ PIC stretch'ит SCL пока загружает следующий байт
// bb_scl_high() в следующем read_byte ЖДЁТ SCL HIGH
```
**Гипотеза**: ACK clock timing OK, проблема может быть в том что `gpio_direction_output(SDA, 0)` вызывается СЛИШКОМ ПОЗДНО — PIC уже сэмплировал SDA на rising edge 9-го clock и видит HIGH (NACK).

**Fix**: установить SDA LOW **ДО** 8-го falling edge, или сразу после 8-го бита данных:
```c
// После 8 бит данных, ПЕРЕД 9-м clock:
gpio_direction_output(BB_SDA_GPIO, 0);  // SDA LOW (ACK) — готовим ЗАРАНЕЕ
udelay(10);                              // дать время установиться
bb_scl_high();                           // 9-й clock — PIC читает ACK
udelay(5);
bb_scl_low();                            // PIC: CKP=0, stretch
gpio_direction_input(BB_SDA_GPIO);       // отпустить SDA
udelay(10);
// Следующий read_byte: bb_scl_high() ждёт clock stretch
```

### Шаг 5: Если bit-bang read всё равно не работает — гибрид
- Bit-bang: START → Write cmd → RESTART → Write addr+R
- **НЕ делать Stop** — оставить шину в состоянии "после addr+R ACK"
- Переключить GPIOMODE обратно на I2C
- SM0 manual mode: продолжить READ (SM0 подхватит шину)
- Это рискованно но SM0 multi-byte read уже работает

### Результаты тестирования (v0.21)

**Что РАБОТАЕТ:**
1. ✅ Bit-bang write → PIC ACK (clock stretching через kernel GPIO API)
2. ✅ Bit-bang Combined (write+restart+read) — 8 байт (агрессивный ACK fix)
3. ✅ SM0 hybrid init read: `97 47 25 e5 39 3e 40 e6` — свежие данные от PIC
4. ✅ mutex между touch и battery — no deadlocks
5. ✅ Версионирование модуля (LCD_DRV_VERSION)
6. ✅ SM0 auto mode hardware: ДОКАЗАНО что CFG read-only, CTL0 модифицируется (pic_stock_test)

**Что НЕ работает:**
1. ❌ Данные СТАТИЧНЫ — PIC не запускает ADC без калибровки
2. ❌ Bit-bang read имеет 1-bit left shift (raw `aa 54` = corrected `55 2a`)
3. ❌ SM0 hybrid read в loop = ff (SM0 state corrupted после touch)
4. ❌ 100ms задержка перед restart не помогает — PIC не делает ADC вообще

**КОРНЕВАЯ ПРИЧИНА**: PIC firmware НЕ ЗАПУСКАЕТ ADC без полной инициализации:
- Стоковый flow: WAKE {0x33} → Calibration Table1+Table2 → bat_read {0x2F}
- Без калибровки PIC возвращает **кэшированные данные из RAM**, не live ADC
- Калибровка говорит PIC КАКОЙ канал ADC читать, какой Vref использовать
- bat_read без предварительной калибровки = чтение пустого буфера

### Текущий статус (финальный)
- **Write через bit-bang**: работает (ACK с clock stretching)
- **Read через SM0**: работает (правильные данные)
- **Combined Message**: работает технически, но PIC всё равно не обновляет ADC
- **Данные `39 3e` статичны** — не меняются ни при разряде, ни при зарядке, ни после bat_read

### Оставшиеся гипотезы
1. **PIC нужна калибровка** для запуска непрерывного ADC — без таблиц PIC возвращает boot snapshot
2. **bat_read {0x2F,0x00,0x02}** — может быть неправильные параметры
3. **Данные уже реальные** но меняются ОЧЕНЬ медленно (1 count за 20+ мин)
4. **PIC firmware** ждёт полную последовательность: WAKE → Calib1 → Calib2 → bat_read
5. **Батарея «на 5-7 пинов»** — smart battery pack с собственным контроллером, PIC просто мост

### SM0 Auto Mode — СЛОМАН на уровне hardware (доказано pic_stock_test)

**Тест записи SM0 регистров (с unbind i2c-mt7621):**
```
CTL0: write 0x90644042 → read 0x8064800E  ❌ stock value не принимается
CTL0: write 0x80644002 → read 0x8064800E  ❌
CTL0: write 0x01F3800F → read 0x01F3800F  ✅ kernel value OK
CFG:  write 0x000000FA → read 0x00000000  ❌ ВСЁ ИГНОРИРУЕТСЯ!
CFG:  write 0x000000FF → read 0x00000000  ❌ РЕГИСТР READ-ONLY!
CFG2: write 0x00000001 → read 0x00000001  ✅ auto mode bit OK
DATA: write 0x0000002A → read 0x0000002A  ✅
```

**Вывод**: SM0_CFG (0x900) = read-only на текущем hardware. CTL0 принимает только определённые значения (kernel 6.12 value OK, stock 3.10 value — нет). **Auto mode SM0 физически несовместим со стоковым PIC протоколом на этом ядре.**

**Единственный рабочий путь записи в PIC = GPIO bit-bang с clock stretching.**

### IDA: PIC_CALIB_LOOP вызывается с $a0=3
Не 200 записей, а ТОЛЬКО 3! Значит калибровочные таблицы по 401 байт — это НЕ то что шлёт PIC_CALIB_LOOP каждый раз. Параметр $a0=3 может означать: 3 секции, или 3 повтора, или mode=3.

### ПЛАН СЛЕДУЮЩИХ ШАГОВ (обновлён после IDA deep analysis)

**Ключевые находки из IDA_DEEP_ANALYSIS.md:**
1. SM0_CTL1 = **0x90640042** (НЕ 0x90644042 — бит 14 лишний!)
2. **Калибровка необязательна** — WAKE с count=0 достаточно
3. bat_read **{0x2F, 0x00, 0x01}** = polling mode (мы слали 0x02 = oneshot!)
4. sub_413F78 = WRITE-ONLY (не Combined!) — чтение ОТДЕЛЬНОЙ транзакцией
5. Worker loop 500ms проверяет pending flag, потом читает

**Шаг 1: Исправить протокол**
- WAKE: {0x33, 0x00, 0x00} (count=0, без калибровки) через bit-bang
- bat_read: {0x2F, 0x00, **0x01**} (polling, НЕ 0x02!) через bit-bang
- Чтение: ОТДЕЛЬНАЯ транзакция SM0 read (НЕ Combined message!)
- Задержка 500ms-1000ms между write и read (как в стоковом worker loop)

**Шаг 2: SM0 init reset**
- RSTCTRL (0xBE000034) |= 0x10000 → &= ~0x10000 (hardware reset SM0)
- SM0_CTL1 = 0x90640042 (правильное значение!)
- Это может починить SM0 auto mode на 6.12

**Шаг 3: Если SM0 auto mode заработает после reset + правильный CTL1**
- Отправить WAKE + bat_read через SM0 auto mode (не bit-bang)
- Читать через SM0 auto mode read
- Всё в одном контроллере — нет проблем с GPIOMODE switch

**Шаг 4: Мониторинг**
- Каждые 10 сек: bat_read write → 500ms delay → SM0 read
- Проверить меняются ли данные при разряде
- Откалибровать эмпирически: 0%=min ADC, 100%=max ADC

**Результат v0.22: калибровка отправлена, ADC не обновляется**
- WAKE: ACK ✅, Table1 (401 bytes): ACK ✅, Table2: ACK ✅, bat_read: ACK ✅
- SM0 byte 6 изменился (0x00 → 0x41) — PIC ПРИНЯЛ калибровку
- Но bytes 4-5 (`39 3e`) НЕ изменились — ADC всё ещё не обновляется

**IDA анализ sub_296938**: калибровка загружается через **platform_data** framework, НЕ из PIC через I2C.
Данные прошиты в board file стокового ядра 3.10.14 или в firmware rootfs.
В MTD дампе калибровочный паттерн НЕ найден.
Стоковая прошивка НЕ поможет — нужен board file с зашитыми данными.

**РЕШЕНО в v0.24!** SM0 auto mode read с ПРАВИЛЬНЫМИ параметрами РАБОТАЕТ:
- SM0_START = **count-1** (не count!)
- SM0_POLLSTA bit **0x04** (не 0x02!)
- SM0_STATUS = 1 (read mode)
- SM0_DATAIN для каждого байта

**Данные:**
- `ff aa 01 a7 ff ff ff ff` — byte 0=ff (SM0 echo), byte 1=aa, bytes 2-3 = **0x01A7 = 423 = LIVE ADC!**
- 423 в диапазоне LOW (401-542) — батарея частично разряжена ✅
- Данные скачут при конфликте touch/battery — нужна стабилизация
- **Byte 0 (0xFF) = SM0 slave address echo** (аппаратный, не данные PIC)
- Предыдущие данные `55 00 00 00 39 3e` были от NEW SM0 manual mode — ДРУГОЙ протокол, ДРУГОЙ буфер

**v0.28: RSTCTRL reset + SM0 write + read в loop:**
- ADC=591 СТАБИЛЬНО на зарядке (20+ чтений без мусора!)
- RSTCTRL reset перед каждым циклом решает touch interference
- НО: через ~20 мин SM0 write ломает PIC → ff ff ff
- **RSTCTRL + write в loop = нестабильно долгосрочно**

**v0.30 (текущий план): RSTCTRL reset + READ ONLY в loop**
- Init: RSTCTRL + WAKE + bat_read (один раз)
- Loop: RSTCTRL + SM0 read only (без write)
- Гипотеза: RSTCTRL + read не сломает PIC (write ломал)

## Технические детали

### GPIO bit-bang I2C
```c
#define BB_SDA_GPIO  515  // gpiochip0(512) + pin 3
#define BB_SCL_GPIO  516  // gpiochip0(512) + pin 4
#define BB_DELAY     10   // us, ~50kHz

// LOW: gpio_direction_output(pin, 0)
// HIGH: gpio_direction_input(pin) — внешний pull-up
// READ: gpio_get_value(pin) после direction_input
// Clock stretching: while(!gpio_get_value(SCL)) udelay(1);
```

### Build для ядра 6.12.74
```bash
# НЕ fork/openwrt_almond (тот для 6.6.127)!
FILD_DIR="/mnt/sata/var/openwrt/fildunsky_openwrt"
KDIR="$FILD_DIR/build_dir/target-mipsel_24kc_musl/linux-ramips_mt7621/linux-6.12.74"
CROSS="$FILD_DIR/staging_dir/toolchain-mipsel_24kc_gcc-14.3.0_musl/bin/mipsel-openwrt-linux-musl-"
make -C $KDIR M=$PKG_SRC ARCH=mips CROSS_COMPILE=$CROSS modules
```
