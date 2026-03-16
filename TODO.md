# TODO — Securifi Almond 3S

## PIC16LF1509 Battery Monitoring

### Статус: ЗАБЛОКИРОВАНО — нужен PICkit

PIC16LF1509 на I2C addr 0x2A отдаёт тестовый паттерн `AA 54 A8 50 A0 40 80` (сдвиг бита) — дефолтный буфер до получения калибровки. Register-style reads игнорируются — PIC всегда отдаёт тот же буфер. Без 800 байт runtime-калибровки от стокового ядра батарея НЕ читается.

### Что установлено (реверс IDA Pro)

- **I2C протокол PIC (palmbus direct)**:
  - Write: SM0_DATA=0x2A, SM0_START=len, SM0_DATAOUT=cmd, SM0_STATUS=0, poll bit 1, delay 15ms
  - Read: SM0_START=len-1, SM0_STATUS=1, poll bit 2 (0x04), delay 10us, read SM0_DATAIN
- **Linux i2c_transfer работает** для PIC (write и read)
- **Команды PIC**:
  - `{0x33, 0x00, 0x01}` — OK, ответ не меняется
  - `{0x41}` — OK, но **убивает PIC** (перестаёт отвечать до power cycle!)
  - `{0x2F, 0x00, 0x02}` — нестабильно (иногда OK, иногда FAIL)
  - `{0x0F}`, `{0x34, 0x00, 0x01}` — FAIL
- **Калибровка (sub_413288)**: 2 × 400 байт данных отправляются PIC при старте стоковой прошивки
  - Первый блок: cmd=0x03, 400 байт (200 × int16, byte-swapped)
  - Второй блок: cmd=0x2E, 400 байт
  - Данные генерируются в runtime, НЕ захардкожены в ядре
  - Без них PIC возвращает только тестовый паттерн
- **Прошивка PIC в ядре НЕ найдена** (данные по 0x597BB0 — не PIC14 firmware, ложное совпадение)
- **Формат ответа батареи** (из строки ядра): `Read_Battery 0x%x vref 0x%x LastVref 0%x c_Last_Percentage %d f_Last_Percentage %d C_Percentage %d F_Percentage %d CBatVal %d FBatVal %d BatStat %d BatCount %d`

### Вариант 1: Своя прошивка PIC (рекомендуется)

Написать простую прошивку на C (SDCC) для PIC16LF1509:
- I2C slave на адресе 0x2A
- ADC чтение напряжения батареи
- По I2C запросу — отдавать raw ADC + вычисленное напряжение

**Что нужно:**
- PICkit 2 или PICkit 3 (~500₽ AliExpress, клон)
- ПО: [PICKitPlus](https://pickitplus.co.uk) (~23 GBP) или бесплатный pk2cmd
- Компилятор: [SDCC](https://sdcc.sourceforge.net/) (бесплатный)
- Доступ к ICSP пинам на плате: MCLR (RC5), PGD, PGC, VDD, GND
- Заголовочный файл: `pic16lf1509.h` из SDCC non-free

**Ключевые регистры PIC16LF1509:**
| Регистр | Адрес | Описание |
|---------|-------|----------|
| SSP1ADD | 0x212 | I2C slave address (0x54 = addr 0x2A << 1) |
| SSP1CON1 | 0x215 | I2C mode (0x36 = SSPEN + 7-bit slave) |
| SSP1BUF | 0x211 | I2C data buffer |
| SSP1STAT | 0x214 | I2C status (BF, R/W, D/A) |
| ADCON0 | 0x09D | ADC control (channel, enable, GO) |
| ADCON1 | 0x09E | ADC reference, format |
| ADRESH | 0x09C | ADC result high |
| ADRESL | 0x09B | ADC result low |

### Вариант 2: Перехват калибровки из стоковой прошивки

1. Загрузить оригинальное ядро (из бэкапа MTD)
2. Дамп I2C трафика при загрузке (i2c sniffer или logic analyzer)
3. Или дамп kernel memory 0x8159A044 (400 байт) и 0x8159A1D4 (400 байт) после init
4. Отправить эти данные PIC через наш lcd_drv.ko

**Плюсы:** не нужно оборудование
**Минусы:** сложнее, калибровка может быть device-specific

## OpenWrt 25.12 / ядро 6.12.x — проверка совместимости

- [ ] Проверить: собрать OpenWrt 25.12 (main ветка, ядро 6.12.x) для Almond 3S
- [ ] Проверить: загрузка с текущим U-Boot (от a43/fildunsky)
- [ ] Если не грузится — попробовать U-Boot от [DragonBluep](https://github.com/DragonBluep/uboot-mt7621) (**ОСТОРОЖНО**: нужен SPI программатор CH341A для восстановления!)
- [ ] По данным ar2r2806 (4PDA): "25.12 нормально собирается и работает" с DragonBluep U-Boot

## lcd_drv.ko — Автозагрузка

- [ ] Заменить lcd_gpio.ko на lcd_drv.ko в сборке прошивки
- [ ] Проверить что модуль грузится при старте
- [ ] Добавить lcd_render в /etc/init.d/

## Дисплей — UI

- [ ] Статус-дашборд: IP, сигнал, uptime, батарея
- [ ] Touch-реактивные кнопки
- [ ] Интеграция с LuCI

## Buzzer / Динамик

### Статус: НЕ НАЙДЕН. Требуется осмотр платы.

Роутер **пищит при включении питания** — динамик есть физически.

### Что проверено
- sysfs GPIO (0, 1, 2, 29, 30) — звука нет
- PIC I2C команды (0x10-0xF0 одиночные) — PIC NACKает через Linux I2C
- PIC multi-byte команды ({0x33,xx,xx}, {0x34,xx,xx}, {0x2E,xx,xx}, {0x2F,xx,xx}) — FAIL через /dev/i2c-0
- IDA реверс: модуль `almond_remind` + `almond_remind_handler` найден в строках ядра, но код не привязан к функциям (stripped)
- Нет аудиофайлов в стоковой прошивке
- Нет PWM контроллера в DTS

### Гипотезы
1. **Buzzer на GPIO через DIR bit-bang** (как дисплей) — sysfs не работает, нужен mmap. Не протестировано через ioremap
2. **Buzzer управляется PIC16** — PIC пищит при старте до загрузки ОС. Нужен правильный I2C протокол (palmbus, не Linux driver)
3. **Buzzer подключён к одному из "занятых" GPIO** (5-12 = UART2/UART3) — не тестировались

### Следующие шаги
- [ ] Осмотр платы: найти buzzer/piezo элемент, проследить дорожку к GPIO/PIC
- [ ] Добавить ioctl в lcd_drv для GPIO toggle через DIR регистр (тест всех GPIO)
- [ ] Попробовать PIC команды через palmbus (как touch read, без Linux I2C driver)
- [ ] Проверить GPIO 5-12 (UART2/UART3) через DIR bit-bang

## LED

### Статус: только подсветка дисплея

- GPIO 31 = backlight (управляется через ioctl(4, 0/1))
- Других LED на плате не обнаружено
- DTS содержит только `display:power` на GPIO 31

## LTE модем

- [x] Автоподключение при загрузке (QMI, Билайн)
- [x] Отображение сигнала на дисплее (RSRP/SINR/Temp)
- [x] Переключение бендов через UI
- [x] LTE watchdog (автопереподключение)
- [x] SMS чтение
- [x] USSD баланс (*102#)
- [ ] Автовыбор лучшего бенда (скрипт есть, UI кнопка есть)

## U-Boot — кастомная сборка

### Статус: ИССЛЕДОВАНИЕ. ВЫСОКИЙ РИСК.

**Текущий U-Boot**: кастомный от a43/fildunsky, md5 `c60429c8c08be531b36a72ce5f6e1a3a`. Работает, есть WebUI Recovery (192.168.1.1).

### Желаемые фичи

1. **USB Recovery**: при загрузке проверять USB на наличие `recover_aa.bin` и прошивать автоматически
2. **LCD дисплей в U-Boot**: показывать статус загрузки на ILI9341 (уже есть GPIO bit-bang код из реверса)
3. **Совместимость с OpenWrt 25.12** (ядро 6.12.x): пользователь ar2r2806 утверждает что 25.12 работает с U-Boot от DragonBluep

### Источники

- **DragonBluep U-Boot**: https://github.com/DragonBluep/uboot-mt7621
  - U-Boot v2018.09 для MT7621
  - NOR flash, WebUI failsafe, TFTP recovery
  - Сборка через GitHub Actions (форк → Run workflow → скачать бинарник)
  - **НЕТ** поддержки USB boot
  - **НЕТ** поддержки LCD дисплея
  - 96 звёзд, 272 форка — достаточно проверен
- **Текущий U-Boot (a43)**: https://github.com/fildunsky/openwrt
- **Инфо от ar2r2806**: "25.12 нормально собирается и работает, U-Boot от DragonBluep"

### КРИТИЧЕСКИЕ РИСКИ

| Риск | Последствие | Восстановление |
|------|------------|---------------|
| U-Boot не загрузится | **КИРПИЧ** | Только SPI программатор (CH341A + SOIC8 клипса) |
| Неправильная MTD таблица | Firmware не найден, не грузится | WebUI recovery если U-Boot жив |
| Неправильные DDR параметры | RAM не инициализируется | Кирпич, SPI программатор |
| PIC16 отключит питание | U-Boot не успеет загрузиться | Кирпич без хака автостарта |
| Затёрт factory раздел | Потеря WiFi калибровки | Восстановление из бэкапа MTD |

**ВАЖНО**: На MT7621 **нет mtkuartboot**. Если U-Boot умер — единственный путь это SPI программатор.

### Безопасный план (если делать)

1. **Купить CH341A + SOIC8 клипсу** (~500₽) — ДО начала работ
2. Проверить что полный дамп flash есть: `BACKUP/MTD0_ALL.BIN` (64 МБ)
3. Снять отдельный дамп текущего U-Boot: `dd if=/dev/mtd0 of=/tmp/uboot_current.bin`
4. Попробовать загрузить НОВЫЙ U-Boot через **TFTP/RAM** (без записи во flash) — если текущий U-Boot поддерживает
5. Только после успешной загрузки из RAM — прошивать во flash
6. Параметры для Almond 3S:
   - Flash Type: **NOR**
   - MTD: `192k(u-boot),64k(u-boot-env),64k(factory),-(firmware)` — ПРОВЕРИТЬ через `cat /proc/mtd`!
   - Reset GPIO: **32**
   - LED GPIO: **31**
   - CPU: **880 МГц**
   - DDR: **256 МБ** (тип DDR2/DDR3 — определить)
   - Baud: проверить serial console

### Что нужно добавить в U-Boot (форк DragonBluep)

1. **USB recovery**: в `board_late_init()` — проверка USB mass storage на файл `recover_aa.bin`, если есть — `mtd write` в firmware раздел
2. **LCD дисплей**: порт GPIO bit-bang кода из lcd_drv.c в U-Boot — показ текста "Loading..." при старте
3. **GPIOMODE = 0x95A8**: установка правильного GPIOMODE для LCD пинов
