# PIC16 Battery — Состояние на 24 марта 2026

## Платформа
- **Ядро**: 6.12.74 (fildunsky_openwrt, OpenWrt 25.12.0)
- **DTS**: `&ethphy0 { /delete-property/ interrupts; }` — обязательно для стабильного LAN
- **lcd_drv**: v1.0 (md5 8cf0746e) — framebuffer + touch + PIC polling
- **PIC16LF1509**: I2C slave 0x2A, bus 0 (SM0)
- **PIC firmware**: ДАМПНУТА через PICkit 3, полностью проанализирована (CP=OFF)

## ПРОРЫВ 24 марта: PIC прошивка дампнута и полностью разобрана

### Дамп прошивки
- **PICkit 3** + **MPLAB IPE v6.05** (v6.25+ дропнул PICkit 3!)
- Роутер полностью обесточен (батарея + зарядка off), PICkit подаёт VDD 3.3V
- Команда: `sudo ipecmd.sh -P16LF1509 -TPPK3 -W3.3 -GF"pic_firmware.hex"`
- Config Word 1: `0x09E4`, Config Word 2: `0x13FF`, **CP=OFF** (код НЕ защищён)
- Файл: `ideas/pic_firmware.hex` (44KB), `ideas/pic_firmware.asm` (8198 строк, gpdasm)

### Полный анализ прошивки
- **`ideas/PIC_FIRMWARE_ANALYSIS.md`** — 1000+ строк, детальный анализ
- 30+ функций замаплены, 14 I2C команд, все RAM переменные
- Карта ISR, main loop, все функции, таблицы данных

### КЛЮЧЕВЫЕ ОТКРЫТИЯ из анализа прошивки

#### 1. Cmd 0x36 = ЕДИНСТВЕННЫЙ способ получить live ADC
- При получении 0x36 PIC **немедленно** (в ISR!) читает канал AN11 (батарея)
- Результат сохраняется в 0x38 (ADC high), 0x39 (ADC low), 0x3A (status)
- `cmd_state = 0x0A` — разрешает master read 3 байта
- **В main loop НЕТ автоматического polling ADC!** Только IDLE цикл ожидания прерываний

#### 2. Cmd 0x37 = firmware version (возвращает 0x07)
- Master read после 0x37: получает 1 байт = 0x07

#### 3. Данные `39 3E` = ЗАВОДСКИЕ значения из flash, НЕ ADC!
- `ram_init` (0x0B32) копирует flash → RAM при старте
- 0x38=0x39, 0x39=0x3E — это начальные значения из flash таблицы
- Byte 6 (0x40/0x01) меняется потому что Timer1 ISR обновляет charge_snapshot (PORTE.3 GPIO)
- **Мы читали НЕ данные батареи, а flash defaults всё это время!**

#### 4. КАЛИБРОВОЧНЫЕ ТАБЛИЦЫ = МЕЛОДИИ, НЕ БАТАРЕЯ!
- `pic_calib.h` содержит LINEAR RAMP (4,5,6,7...) = бессмысленные данные для батареи
- Flash таблицы 0x0A7E/0x0ACE = PWM частоты для мелодий (0x0526=1318Hz, 0x04DC=1244Hz, etc.)
- cmd 0x2D/0x2E загружают **НОТНЫЕ ТАБЛИЦЫ** в RAM 0x00A0/0x0120
- cmd 0x2F с данными:
  - 0x0001 → main_state=1 (NOTE_PLAY — проиграть ноту)
  - 0x0002 → main_state=2 (IDLE)
  - 0x0003 → main_state=3 (NOTE_TABLE — проиграть таблицу нот)
- **Батарейный ADC НЕ нуждается в калибровочных таблицах! Только cmd 0x36!**

#### 5. GPIO маппинг PIC (подтверждено анализом firmware + тестами)
| Пин | I/O | Назначение | Проверка |
|-----|-----|-----------|----------|
| PORTE.0 | INPUT | Зарядка подключена | cmd 0x36 → status byte bit 0 |
| PORTE.3 | INPUT | Состояние заряда | TMR1 ISR мониторит каждые ~125ms |
| PORTE.4 | OUTPUT | **LED (ACTIVE LOW!)** bsf=OFF, bcf=ON | cmd 0x32=ON, 0x31=OFF, 0x30=BLINK — **ТЕСТИРОВАНО!** |
| PORTE.5 | OUTPUT | Сигнал выключения | LOW = кнопка питания → мелодия + LED off |
| PORTA.0 | OUTPUT | Выход A | cmd 0x34 data=0x01: set |
| PORTA.1 | OUTPUT | Выход B | cmd 0x34 data=0x02: set |
| PORTC.0 | OUTPUT | Бипер PWM | gpio_output_pwm генерирует звук |
| AN11 (RB5?) | ADC | Напряжение батареи | cmd 0x36 → adc_read_channel(0x0B) |

#### 6. Тайна мелодии при init РАЗГАДАНА
- ISR CLC/TMR6 обнаруживает PORTE.5 LOW (shutdown signal) во время PIC reset
- Автоматически запускает `beep_startup` (3 ноты: E6-D#6-A6)
- cmd 0x40/0x41 только устанавливают/сбрасывают `buzzer_request` FLAG
- cmd 0x34 управляет PORTA пинами, НЕ бипером напрямую
- Мелодия при init = PIC видит PORTE.5 LOW после RSTCTRL reset → мелодия автоматическая
- **Нет I2C команды для прямого запуска мелодии** — только через ISR цепочку

#### 7. SM0CTL0 разница найдена
```
Наш SM0CTL0:   0x01F3800F — SCL_STRETCH=1, ODRAIN=0, clk_div=0x1F3
Stock SM0CTL0: 0x8064800E — SCL_STRETCH=0, ODRAIN=1, clk_div=0x064
```
- Stock: SCL_STRETCH **ВЫКЛЮЧЕН**, ODRAIN=1
- Наш: SCL_STRETCH **ВКЛЮЧЁН**, ODRAIN=0
- Тест со stock CTL0 в lcd_drv: данные изменились с `39 3e 43 ee` на `ff ff ff ff` — другое поведение!

## Что РАБОТАЕТ

### PIC polling (NEW SM0 manual mode)
- Регистры: 0x944 (N_CTL0) / 0x950 (N_D0 write) / 0x954 (N_D0 read)
- Polling каждые 10 сек в touch thread lcd_drv
- **НЕ ломает MT7530 / IRQ #23 / LAN** — стабильно
- SM0 save/restore перед каждой операцией

### PIC init (OLD SM0 auto mode WRITE)
- `{0x41}` — PIC init (бипер пиликает из-за PORTE.5 ISR, не из-за команды)
- Калибровочные таблицы — **бесполезны для батареи** (это МЕЛОДИИ), но PIC принимает
- `{0x2F, 0x00, 0x02}` — устанавливает main_state=2 (IDLE), cmd_state=0
- OLD SM0 auto mode WRITE работает на 6.12.74

### LED управление (ПРОТЕСТИРОВАНО!)
- cmd 0x32 → LED ON (bcf PORTE.4, active LOW)
- cmd 0x31 → LED OFF (bsf PORTE.4)
- cmd 0x30 → LED BLINK (toggle, TMR1 мигает каждые 4 тика)

### Данные charge status (byte 6) ЖИВЫЕ
```
Зарядка:  55 00 00 00 39 3E 40 E6
Батарея:  55 00 00 00 39 3E 01 C6
```
- Byte 6: `0x40` (зарядка) → `0x01` (батарея) — Timer1 ISR обновляет через PORTE.3
- Byte 0: `0x55` = I2C addr echo (0x2A<<1|1), NEW SM0 не фильтрует
- Bytes 4-5: `0x39 0x3E` = **ЗАВОДСКИЕ ЗНАЧЕНИЯ ИЗ FLASH, НЕ ADC!**

### Пороги ADC (из стокового ядра, дизассемблер)
- Raw < 401 → CRITICAL
- Raw 401-541 → LOW
- Raw >= 542 → NORMAL

## Что НЕ РАБОТАЕТ

### Live ADC данные батареи
- Нужна команда `{0x36}` + чтение 3 байт с clock stretching
- NEW SM0 manual mode **не поддерживает clock stretching** PIC
- PIC удерживает SCL LOW на ~30-40us между байтами (SSP1CON2.SEN=1, CKP)
- OLD SM0 auto mode READ → FF (SM0_CFG read-only на MT7621 eco:3)

### Правильный протокол (из анализа firmware)
```
Шаг 1: I2C Write [0x54, 0x36]     — запрос ADC чтения
Шаг 2: delay 5ms (ADC ~15us, но safety margin)
Шаг 3: I2C Read [0x55] → 3 байта:
        byte0 = ADC high (10-bit)
        byte1 = ADC low
        byte2 = status flags
ADC value = (byte0 << 8) | byte1  (0-1023)
```

### Почему текущий lcd_drv получает flash defaults а не ADC
1. lcd_drv отправляет `{0x2F, 0, 2}` — это НЕ ADC read, а main_state=IDLE
2. PIC НЕ делает ADC при 0x2F — только устанавливает main_state
3. При чтении cmd_state=0 → master read handler не загружает SSPBUF → fallback
4. Данные `39 3E` = flash ram_init values, timer ISR обновляет только charge GPIO

### Аппаратная проблема: Clock Stretching
- PIC ISR при master read: загружает SSPBUF → `bsf CKP` (release SCL) → poll BF
- SCL удерживается LOW ~30-40us пока PIC обрабатывает ISR до `bsf CKP`
- **NEW SM0** может не обнаруживать SCL stretch → клочит мусор
- **OLD SM0** поддерживал clock stretching аппаратно, но SM0_CFG=0xFA not writable on eco:3

### Отдельный pic_battery.ko модуль — НЕ РАБОТАЕТ
- SM0 sharing между i2c-mt7621, touch, PIC слишком хрупкий
- SM0_CFG2=0: `aa 54 a8` (bus noise)
- SM0_CFG2=1: START timeout
- **Оригинальный lcd_drv с PIC внутри** — единственный рабочий вариант

## ОПРОВЕРГНУТЫЕ гипотезы

### ~~Конфликт SM0 между lcd_drv и i2c-mt7621~~
**НЕВЕРНО.** SM0 save/restore работает. PIC polling не ломает LAN. gpio_request() — единственная причина IRQ #23.

### ~~PIC данные статичные~~
**ЧАСТИЧНО НЕВЕРНО.** Charge status (byte 6) обновляется через Timer1 ISR (GPIO PORTE.3). ADC данные (bytes 4-5) = flash defaults, ПОТОМУ ЧТО мы не отправляли cmd 0x36.

### ~~Калибровочные таблицы нужны для батареи~~
**ПОЛНОСТЬЮ НЕВЕРНО.** Таблицы 0x2D/0x2E = PWM частоты для МЕЛОДИЙ. pic_calib.h содержит linear ramp (4,5,6,...) = МУСОР. Батарейный ADC использует ТОЛЬКО cmd 0x36. Никакой калибровки не нужно.

### ~~Нужен осциллограф~~
**НЕ НУЖЕН.** Прошивка дампнута и полностью проанализирована. Протокол I2C известен.

### ~~Bus noise (aa 54 a8) = SM0 конфликт~~
Было на ядре 6.6.127. На 6.12.74 NEW SM0 читает нормально.

### ~~GPIOMODE=0x95A8 — ключ к PIC~~
Не подтверждено. На 6.12.74 PIC читается без смены GPIOMODE.

### ~~RSTCTRL reset ломает MT7530 LAN~~
**НЕВЕРНО.** Протестировано 24 марта — RSTCTRL I2C reset (bit 16) безопасен для MT7530.

### ~~cmd 0x34 = бипер~~
**НЕВЕРНО.** cmd 0x34 управляет PORTA.0/PORTA.1 (внешние выходы), НЕ бипером. Вероятно: управление BQ24133 зарядкой.

### ~~Бипер включается командой 0x40/0x41~~
**ЧАСТИЧНО НЕВЕРНО.** cmd 0x40/0x41 только устанавливают FLAG `buzzer_request`. Бипер генерируется сложной ISR цепочкой (CLC+TMR6+adc_ready). Мелодия при init = автоматическая реакция ISR на PORTE.5 LOW.

## WARNING: Бипер
- Мелодия при init = ISR PORTE.5 LOW detection, не I2C команда
- cmd 0x40 = buzzer_request=1, cmd 0x41 = buzzer_request=0
- PWM на PORTC.0 зависит от ISR chain (CLC+TMR6+adc_ready_flag)
- **НЕТ прямой I2C команды для воспроизведения мелодии** — только ISR автоматика

## Результаты тестов

### 23 марта 2026

#### i2c_transfer() через Linux I2C API — NACK
- `i2cget -y 0 0x2a` → Read failed
- `i2ctransfer -y 0 r1@0x2a` → "No such device or address"
- SX8650 (0x48) тоже NACK из userspace — lcd_drv touch thread мешает
- Из kernel i2c_transfer (lcd_drv ioctl) — тоже NACK
- **Вывод**: PIC не отвечает через kernel I2C API (i2c-mt7621)

#### Userspace прямой SM0 доступ — ЛОМАЕТ PIC!
- Запись в SM0 регистры из userspace при работающем lcd_drv → гонка с touch thread
- PIC переходит в broken state: `aa 54 a8 50 a0 40 80 00` (bus noise)
- Восстановление ТОЛЬКО полным обесточиванием (батарея + зарядка off на 15+ сек)
- **НИКОГДА не писать в SM0 из userspace при загруженном lcd_drv!**

#### Без батареи PIC отвечает `55 00 00 00 00 00 00 00`
- После полного обесточивания (30с без батареи) + boot
- Нули = нет батареи (ram_init flash defaults? или PIC обнуляет при отсутствии)

#### i2c-mt7621.c исходник изучен
- Использует **те же** NEW SM0 регистры: SM0CTL0(0x940), SM0CTL1(0x944), D0(0x950), D1(0x954)
- SM0CFG2=0 ставит ТОЛЬКО в `mtk_i2c_reset()` (probe + error recovery)
- Использует `iowrite32`/`ioread32` (с memory barriers), lcd_drv — `__raw_writel` (без barriers)
- SM0CTL0 = `(clk_div << 16) | EN | SCL_STRETCH` → 0x01F3800F

#### Стоковое ядро 3.10 I2C init (r2 анализ MTD4_KER.BIN)
Три функции: I2C_Reset(0x41271C), I2C_Configuration(0x412770), i2cInit(0x4127DC)
- RSTCTRL |= 0x10000 / &= ~0x10000 + udelay(500)
- **SM0_CFG (0x900) = 0xFA** ← записывается сразу после reset
- **SM0_CTL1 (0x940) = 0x90640042** ← KEY CONFIG VALUE
- SM0_CFG2 (0x928) = 1 (auto mode)

**SM0_CFG=0xFA НЕ записывается на MT7621 eco:3** — silicon limitation.

#### RSTCTRL I2C reset — БЕЗОПАСЕН
- Протестировано с pic_rstctrl.c (без lcd_drv)
- MT7530 LAN НЕ ломается после RSTCTRL bit 16 assert/deassert
- SM0_CTL1=0x90640042 → readback 0x8064800E (hw modifies, совпадает со стоком)

#### Стоковый U-Boot (r2 анализ MTD1_BOO.BIN)
- НОЛЬ обращений к SM0 I2C
- Не делает I2C reset
- PIC/SM0 инициализируется ТОЛЬКО ядром (AlmondPic2 module)

### 24 марта 2026

#### PIC firmware дампнута через PICkit 3
- MPLAB IPE v6.05, PICkit 3, VDD 3.3V
- `pic_firmware.hex` (44KB), `pic_firmware.asm` (8198 строк)
- CP=OFF — полностью читаемая
- Полный анализ в PIC_FIRMWARE_ANALYSIS.md

#### SM0CTL0 stock value test
- Установлен SM0CTL0=0x8064800E (stock) перед PIC read
- Результат: данные изменились с `39 3e 43 ee` на `ff ff ff ff`
- **Другое поведение, но не лучше** — clock stretching всё равно не работает

#### LED управление подтверждено
- cmd 0x32 → LED ON (active low, bcf PORTE.4)
- cmd 0x31 → LED OFF (bsf PORTE.4)
- cmd 0x30 → LED BLINK (toggle + TMR1 periodic)

#### Калибровочные таблицы = мелодии
- pic_calib.h linear ramp = МУСОР (не имеет смысла для батареи)
- Flash 0x0A7E/0x0ACE = PWM частоты для нот
- cmd 0x2D/0x2E = загрузка нотных таблиц
- cmd 0x33 = размер таблицы, cmd 0x35 = repeat count

#### OLD SM0 auto read = тупик (окончательно)
- SM0_CFG read-only на eco:3 даже после RSTCTRL
- SM0_DATAIN всегда FF
- Стоковое ядро 3.10 как-то писало SM0_CFG=0xFA — не воспроизводимо

## СЛЕДУЮЩИЕ ШАГИ (приоритет)

### 1. ТЕСТ: cmd 0x36 write + NEW SM0 read
Правильная последовательность:
1. OLD SM0 auto write `{0x36}` (работает!)
2. delay 10ms (PIC делает ADC в ISR, устанавливает cmd_state=0x0A)
3. NEW SM0 manual read 4 байта (1 addr echo + 3 data)
**Если clock stretching работает** → получим live ADC данные.
**Если не работает** → `55 00 00` или `55 FF FF`

### 2. ТЕСТ: cmd 0x37 (firmware version)
- Write `{0x37}` → read 1 байт → должно быть 0x07
- Простой тест работоспособности read с cmd_state != 0

### 3. SM0CTL0 = stock value + cmd 0x36
- Временно SM0CTL0=0x8064800E перед полным циклом write+read
- Stock использовал SCL_STRETCH=0, ODRAIN=1 — может лучше для PIC

### 4. GPIO bit-bang I2C (если SM0 не работает)
- GPIO 3 (SDA), GPIO 4 (SCL) — ручной clock с проверкой SCL level
- Поддержка clock stretching вручную
- Конфликт с i2c-mt7621 / touch thread — нужен mutex

### 5. Убрать калибровочные таблицы из lcd_drv init
- pic_calib.h = бесполезный мусор для батареи
- Либо удалить из init, либо оставить для совместимости (PIC их принимает без проблем)
- Убрать `{0x2F, 0, 2}` — заменить на `{0x36}` в polling

## Файлы
- `ideas/pic_firmware.hex` — PIC16LF1509 firmware dump (Intel HEX, 44KB)
- `ideas/pic_firmware.asm` — Full disassembly (gpdasm, 8198 строк)
- `ideas/PIC_FIRMWARE_ANALYSIS.md` — детальный анализ прошивки (1000+ строк)
- `ideas/stock_dumps/` — дампы стоковой прошивки (SM0 regs, kallsyms, PIC transactions)
- `ideas/STOCK_DUMP_20mart.md` — анализ стоковых SM0 регистров
- `ideas/IDA_READ_PROTOCOL.md` — дизассемблирование PIC функций стокового ядра
- `ideas/IDA_BUZZER.md` — анализ бипера (дизассемблер стокового ядра)
- `ideas/debug_tools/` — restdebug, memdebug, sm0_dump
- `ideas/pic_tools/` — тестовые утилиты для PIC
- `boot_stock.txt` — полный boot log стоковой прошивки (ядро 3.10.14)
- `i2c-mt7621.c` — исходник kernel I2C драйвера MT7621 (скачан с сервера)
- `modules/pic_calib.h` — **НЕ калибровка! Мелодийные таблицы** (linear ramp = мусор)
- `modules/pic_battery.c` — отдельный PIC модуль (НЕ РАБОТАЕТ, для справки)
- `modules/sm0_shared.h` — общие SM0 defines (для справки)
