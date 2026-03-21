# PROGRESS1 — Бипер заработал! (2026-03-20)

## Что произошло

При запуске `pic_final` на чистом PIC (после power cycle батареи) бипер начал играть **мелодию** (не просто пик, а мелодию!).

## Что делал pic_final

Последовательность команд через **bit-bang I2C** (GPIO sysfs, gpio 515/516):

```
1. WAKE  {0x33, 0x00, 0x00}  → NACK (PIC в test mode не принял)
2. Table1 {0x2D}              → ACK!
3. Table2 {0x2E}              → ACK!
4. bat_read {0x2F, 0x00, 0x01} → ACK!
```

Потом **NEW SM0 manual mode** reads (CTL0=0x01F3800F, CTL1 commands) — стабильные данные.

И ещё **NEW manual mode write** bat_read — тоже ACK (result=0).

## Почему заработал бипер?

### Гипотеза 1: Команда {0x2D} или {0x2E} (пустые калибровочные таблицы)
- PIC интерпретирует пустую таблицу {0x2D} (1 байт, без данных) как команду инициализации
- Стоковая прошивка при count=0 тоже отправляет пустые {0x2D} и {0x2E} (из IDA: PIC_CALIBRATE → "if count <= 0 → send empty Table1 + Table2")
- Это может быть **startup sequence** которая включает бипер как индикацию "PIC проснулся"

### Гипотеза 2: bat_read {0x2F, 0x00, 0x01} запускает мониторинг
- PIC получил bat_read в polling mode
- PIC начал цикл мониторинга батареи
- Как часть цикла — пищит (уведомление о состоянии батареи)
- LOW battery → alarm мелодия!

### Гипотеза 3: Комбинация Table1 + Table2 + bat_read = полная инициализация
- PIC ждал ИМЕННО эту последовательность: {0x2D} → {0x2E} → {0x2F}
- Без калибровочных данных но С правильными командами
- После инициализации PIC запускает мелодию при включении (startup sound)

### Наиболее вероятно: Гипотеза 2 + 3
Стоковая прошивка при загрузке делает ровно это:
1. WAKE → Calib Table1 → Calib Table2 → bat_read
2. PIC запускает мониторинг батареи
3. При LOW battery — играет alarm мелодию
4. Мелодия = **PIC firmware feature**, не просто ON/OFF бипер

## Что это значит

1. **PIC firmware СЛОЖНЕЕ чем мы думали** — умеет играть мелодии, не просто ON/OFF
2. **Bit-bang write РАБОТАЕТ** — PIC получает и обрабатывает команды
3. **Последовательность {0x2D} → {0x2E} → {0x2F} = полная инициализация PIC**
4. **ADC мониторинг мог запуститься** — нужно проверить NEW manual mode read после инициализации
5. **Бипер {0x34, 0x00, 0x00} должен остановить мелодию** (или PIC сам остановит)

## Что НЕ работает (пока)

1. **PIC в test mode** (aa 54 a8) — нужен power cycle для сброса
2. **SM0 auto mode** — сломан на silicon уровне (доказано на 6.6.127 и 6.12.74)
3. **NEW manual mode read** — стабильный но данные не обновляются (нужен правильный write)

## Что работает

| Компонент | Метод | Статус |
|-----------|-------|--------|
| PIC Write | bit-bang GPIO sysfs (clock stretching) | ✅ ACK |
| PIC Write | NEW SM0 manual mode (CTL0=0x01F3800F) | ✅ ACK (bat_read) |
| PIC Read | NEW SM0 manual mode | ✅ Стабильный (каждые 2 итерации) |
| PIC Read | OLD SM0 auto mode | ❌ Ломает PIC |
| Бипер | bit-bang {0x2D}+{0x2E}+{0x2F} | ✅ Мелодия! |
| Бипер OFF | bit-bang {0x34, 0x00, 0x00} | ? Не проверен на чистом PIC |
| ADC Live | Нужна проверка после init | ? |

## Текущий статус (конец дня 2026-03-20)

### РАБОТАЕТ:
- **bit-bang WRITE** → PIC ACK, мелодия играет, {0x34,00,00} стоп ACK
- **lcd_gpio (старый модуль из 6.6.127 прошивки)** → тоже запускает мелодию
- **PIC ALIVE** — обрабатывает команды {0x2D}, {0x2E}, {0x2F}, {0x34}

### НЕ РАБОТАЕТ:
- **ANY READ** → всегда `aa 54 a8 50 a0 40 80 00` или `ff ff ff`
- **NEW manual mode read** → `aa 54 a8` (SM0 bus noise, PIC не загружает SSPBUF)
- **OLD auto mode read** → `aa 54 a8` чередуется с `ff ff ff`
- **SM0 auto mode write** → POLLSTA OK но данные не доходят до PIC (тишина)

### Единственный рабочий READ: v0.28 (lcd_drv kernel module)
- RSTCTRL reset + SM0 auto mode write + 500ms + SM0 auto mode read
- Давал ADC=591 (зарядка) и ADC=423 (батарея) — LIVE данные!
- Работал 20 мин, потом ff
- **Нужно пересобрать для ядра 6.6.127** (build server down)

### ПРОРЫВ #2: lcd_drv на 6.6.127 = LIVE ADC!
- **ADC=591** на зарядке (NORMAL >=542)
- **ADC=423** без зарядки (LOW 401-541)
- Kernel module БЕЗ unbind i2c = данные РЕАЛЬНЫЕ!
- Userspace с unbind = мусор (i2c clock disabled!)
- Первое чтение OK, последующие = ff (нужно fix)

### Почему userspace не работал:
**unbind i2c-mt7621 отключает I2C clock!** SM0 контроллер без clock = мусор.
Kernel module работает БЕЗ unbind — i2c driver держит clock enabled.

### v0.35: {0x2D}+{0x2E}+buzzer_off+bat_read ACK но ADC не обновляется
- Table1 {0x2D}: ACK, Table2 {0x2E}: ACK, buzzer off: ACK, bat_read: ACK
- ADC=423 стабильно на зарядке — не переходит в 591
- {0x2D}+{0x2E} НЕ достаточно для continuous ADC monitoring
- Единственный раз 591 появлялся: v0.28 (RSTCTRL + SM0 auto write + SM0 auto read)

### КРИТИЧЕСКОЕ ОТКРЫТИЕ: 423 и 591 = bus noise, НЕ ADC!

Все "ADC значения" = bit-shift артефакты от 0xAA (PIC addr echo):
```
0xAA >> 7 = 0x01 → bytes 01 a7 → "ADC=423"
0xAA >> 6 = 0x02 → bytes 02 4f → "ADC=591"
0xAA >> 5 = 0x05 → bytes 05 9f → "ADC=1439"
```
Каждое значение = 0xAA сдвинутое на разное количество бит.
"591 на зарядке" vs "423 без зарядки" = СЛУЧАЙНЫЙ clock alignment, НЕ ADC!

**МЫ НЕ ЧИТАЕМ РЕАЛЬНЫЕ ДАННЫЕ PIC.** SM0 auto mode read = bus echo.
PIC ACKает адрес но НЕ загружает данные в SSPBUF.

### Ключевой вопрос:
**КАК правильно читать данные из PIC?** v0.28 = RSTCTRL + SM0 auto mode WRITE bat_read + SM0 auto mode READ. Может SM0 auto mode write имеет ДРУГОЙ тайминг чем bit-bang, и PIC реагирует ТОЛЬКО на SM0 auto mode write?

### Нужно:
1. Добавить SM0 auto mode write (как в v0.28) В INIT — один раз
2. НЕ в loop (ломает PIC через 20 мин)
3. Один SM0 auto write при init + SM0 auto read в loop = может stable live ADC

## План дальше

### Немедленно:
1. **Остановить мелодию**: bit-bang {0x34, 0x00, 0x00}
2. **Power cycle батареи** → чистый PIC
3. Запустить pic_final → проверить READ данные (не test pattern)
4. Если READ показывает реальные данные (не aa 54 a8) → **БАТАРЕЯ LIVE!**

### Если READ данные live:
5. Мониторить 5 мин без зарядки → ADC падает?
6. Подключить зарядку → ADC растёт?
7. Если да → перенести в lcd_drv.ko

### Оптимизация:
8. NEW manual mode write + read (без bit-bang) — быстрее, надёжнее
9. Определить какая команда запускает мелодию (для контроля)
10. Отключить мелодию при boot (или сделать настраиваемой)

### Архитектура:
11. Отдельный battery thread с mutex
12. Periodic: NEW write bat_read → 500ms → NEW read
13. LCD widget для отображения ADC → процент

## Ключевые регистры SM0 (из всех тестов)

```
NEW SM0 Manual Mode (РАБОТАЕТ):
  N_CTL0 (0x940) = 0x01F3800F (kernel default — единственный рабочий!)
  N_CTL1 (0x944) = commands: START=0x11, WRITE=0x21, READ_LAST=0x41, READ=0x51, STOP=0x31
  N_D0   (0x950) = data bytes 0-3
  N_D1   (0x954) = data bytes 4-7
  SM0_CFG2 (0x928) = 0 (manual mode)

OLD SM0 Auto Mode (СЛОМАН на MT7621):
  SM0_CTL0 (0x940) = hardware модифицирует любое значение
  SM0_CFG  (0x900) = READ-ONLY (всегда 0)
  SM0_CFG2 (0x928) = 1 (auto mode)
  Любой auto mode read/write → PIC test mode (aa 54 a8)

Bit-bang GPIO:
  SDA = GPIO 515 (pin 3), SCL = GPIO 516 (pin 4)
  GPIOMODE bit 2 = I2C→GPIO switch
  Clock stretching: while(!gpio_val(SCL)) usleep(1)
```
