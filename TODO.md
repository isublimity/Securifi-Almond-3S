# TODO — Securifi Almond 3S

## PIC16LF1509 Battery Monitoring

### Статус: НЕ РЕШЕНО

PIC16LF1509 на I2C addr 0x2A отвечает тестовым паттерном `AA 54 A8 50 A0 40 80` — реальные данные батареи не отдаёт без калибровки (800 байт runtime-данных от стокового ядра).

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

## lcd_drv.ko — Автозагрузка

- [ ] Заменить lcd_gpio.ko на lcd_drv.ko в сборке прошивки
- [ ] Проверить что модуль грузится при старте
- [ ] Добавить lcd_render в /etc/init.d/

## Дисплей — UI

- [ ] Статус-дашборд: IP, сигнал, uptime, батарея
- [ ] Touch-реактивные кнопки
- [ ] Интеграция с LuCI

## LTE модем

- [ ] Автоподключение при загрузке
- [ ] Отображение сигнала на дисплее
- [ ] Переключение бендов через UI
