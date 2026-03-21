# Глубокий анализ PIC16 батареи — стоковое ядро 3.10.14

Дата анализа: 2026-03-19
Источник: IDA MCP анализ `/tmp/kernel.bin` (base 0x80001000, MIPS LE)

---

## 1. SM0 (I2C) ИНИЦИАЛИЗАЦИЯ — sub_411770 (0x411770)

**Это ПЕРВАЯ функция инициализации I2C контроллера SM0.**

### Полная последовательность:
```
1. RSTCTRL (0xBE000034) |= 0x10000     // Assert I2C reset
2. RSTCTRL (0xBE000034) &= ~0x10000    // Deassert I2C reset
3. udelay(500)                          // Ждём стабилизации
4. SM0_CTL1 (0xBE000940) = 0x90640042  // Конфигурация SM0
5. SM0_DEBUG (0xBE000928) = 1           // Debug/enable
6. SM0_SLAVE (0xBE00090C) = 0           // Slave addr = 0
7. SM0_DATA  (0xBE000908) = 0x48        // Default slave = SX8650 touch
```

### КРИТИЧЕСКАЯ НАХОДКА: SM0_CTL1 = 0x90640042

Разбор битов `0x90640042`:
```
Бит 31 (0x80000000) = 1 — SM0 Auto mode enable
Бит 28 (0x10000000) = 0 — NOT set (в CLAUDE.md указано 0x90644042 — НЕВЕРНО!)
Бит 22-20          = 011 — CLKDIV = 3 (100KHz mode? зависит от CPU clock)
Бит 17 (0x00020000) = 1 — ACK enable
Бит 6  (0x00000040) = 1 — VSYNC_MODE или какой-то режим
Бит 1  (0x00000002) = 1 — SCL output enable
```

**ВАЖНО: Значение 0x90640042 (НЕ 0x90644042!)** — разница в бите 14 (0x4000).

---

## 2. SX8650 Тачскрин I2C Probe — sub_411A80 (0x411A80)

Эта функция делает I2C bit-bang probe тачскрина SX8650 по адресу 0x48:

### Последовательность SM0 регистров:
```
SM0_DATA  (0x908) = 0x48     // Slave addr = SX8650
SM0_START (0x920) = 0        // Clear
udelay(150)

SM0_DATAOUT (0x910) = 0x80   // SX8650: SoftReset command
SM0_STATUS  (0x91C) = 2      // Write mode, 2 bytes
udelay(150)

SM0_START (0x920) = 0
udelay(150)

SM0_DATAOUT (0x910) = 0x90   // SX8650: RegWrite(PenDetect?)
SM0_STATUS  (0x91C) = 2      // Write mode, 2 bytes
udelay(150)

SM0_CTL0 (0x900) = 0xFA      // ??? Возможно rate/clock config
udelay(150)

SM0_START (0x920) = 0
udelay(150)
SM0_START (0x920) = 1        // Trigger START
udelay(150)
SM0_START (0x920) = 1        // Second trigger
udelay(150)

SM0_STATUS (0x91C) = 1       // Read mode
udelay(150)

SM0_DATAIN (0x914) -> s3     // Read byte 1
udelay(150)
SM0_DATAIN (0x914) -> s2     // Read byte 2
udelay(150)

SM0_START (0x920) = 0        // Stop
udelay(150)

SM0_START (0x920) = 1        // Re-trigger
SM0_CTL0? (0x940) = 0x8064800E  // ??? Дополнительная конфигурация

// Return: (s3 & 0xF) << 8 | s2  — 12-bit touch value
```

---

## 3. PIC_I2C_WRITE — 0x412F78

### Сигнатура:
```c
int PIC_I2C_WRITE(uint8_t first_byte, int buffer_ptr, int count)
```

### SM0 регистры для WRITE:
```
SM0_DATA   (0x908) = 0x2A     // PIC slave address (42 decimal)
SM0_START  (0x920) = count    // Number of bytes
SM0_DATAOUT(0x910) = first_byte  // First data byte
SM0_STATUS (0x91C) = 0        // Write mode (0 = write)
```

Если count > 0 — цикл: записывает оставшиеся байты из buffer по одному.
Если count == 0 — проверяет SM0_BUSY (0x918) bit 0, ждёт завершения (100000 итераций).

---

## 4. PIC_I2C_READ — 0x412E78

### Сигнатура:
```c
int PIC_I2C_READ(int buffer_ptr, int count)
```

### SM0 регистры для READ:
```
SM0_START  (0x920) = count - 1  // Length
SM0_STATUS (0x91C) = 1          // Read mode (1 = read)
```

Если count > 0 — цикл: читает SM0_DATAIN (0x914) для каждого байта.
Если count == 0 — проверяет SM0_BUSY (0x918) bit 0, ждёт.

Таймаут: 100000 (0x186A0) итераций polling.

---

## 5. sub_413F78 — Высокоуровневый I2C WRITE wrapper

### Вызов:
```c
sub_413F78(uint8_t slave_addr, uint8_t *buffer, int length)
// slave_addr = 42 (0x2A) для PIC
// buffer[0] = command byte
// length = total bytes including command
```

Внутренне вызывает PIC_I2C_WRITE. Это **ТОЛЬКО WRITE**, без READ.
Использует калибровочные таблицы для пост-обработки (CALIB_INTERP, CALIB_LOOKUP).

---

## 6. PIC_CALIBRATE — 0x413200

### Полная последовательность инициализации калибровки:

```
1. memset(local_buf, 0, 100)
2. mutex_lock(0x80704C84)            // Захват SM0 mutex
3. udelay(5000)                       // 5ms пауза
4. count = struct->offset_4           // Число калибровочных записей (signed byte)
5. WAKE: sub_413F78(0x2A, {0x33, count>>8, count}, 3)
   // Команда 0x33 = WAKE/CALIB_START, с числом записей
6. if (count <= 0) goto NO_CALIB_DATA

--- ЕСЛИ ЕСТЬ КАЛИБРОВОЧНЫЕ ДАННЫЕ ---

7. PIC_CALIB_LOOP:
   a) Byte-swap Table1 (struct+0x14): int32[] -> big-endian bytes
      Для каждого int32: [high_byte, low_byte] (2 байта на запись)
   b) total = 2 * count
   c) udelay(5000)
   d) sub_413F78(0x2A, {0x2D, swapped_table1...}, total + 1)
      // Команда 0x2D = CALIB_TABLE1

   e) Byte-swap Table2 (struct+0x1A4): int32[] -> big-endian bytes
   f) buffer[0] = 0x2E
   g) udelay(5000)
   h) sub_413F78(0x2A, {0x2E, swapped_table2...}, total + 1)
      // Команда 0x2E = CALIB_TABLE2

   i) udelay(5000)
   j) mutex_unlock(0x80704C84)

8. Сохранение в глобалы:
   0x8159A034 = struct->offset_4  (count)
   0x8159A040 = struct->offset_16 (voltage reference?)
   memcpy(0x8159A044, Table1, 400)  // 100 int32 = 400 bytes
   memcpy(0x8159A1D4, Table2, 400)  // 100 int32 = 400 bytes

--- ЕСЛИ НЕТ КАЛИБРОВОЧНЫХ ДАННЫХ ---

9. udelay(5000)
10. sub_413F78(0x2A, {0x2D}, 1)  // Пустая CALIB_TABLE1
11. buffer[0] = 0x2E
12. -> CALIB_TABLE2 далее...
```

### Команды PIC при калибровке:
| Команда | Байт | Описание |
|---------|------|----------|
| WAKE    | 0x33 | Старт калибровки: {0x33, count_hi, count_lo} |
| TABLE1  | 0x2D | Отправка таблицы 1: {0x2D, data...} |
| TABLE2  | 0x2E | Отправка таблицы 2: {0x2E, data...} |

---

## 7. PIC_BAT_READ2 — 0x413094

### Точная последовательность:
```
1. mutex_lock(0x80704C84)
2. udelay(5000)
3. sub_413F78(0x2A, {0x2F, 0x00, 0x02}, 3)
   // Команда 0x2F = BAT_READ, sub=0x00, param=0x02
4. udelay(5000)
5. mutex_unlock(0x80704C84)
```

---

## 8. PIC_BAT_READ_CMD — 0x413138

### Точная последовательность:
```c
// a0 = timeout_seconds
if (timeout <= 0) return -1000;

timeout_ms = timeout * 1000;
MEMORY[0x80704D88] = 1;           // Set pending flag
mutex_lock(0x80704C84);
udelay(5000);
sub_413F78(0x2A, {0x2F, 0x00, 0x01}, 3);
// Команда 0x2F = BAT_READ, sub=0x00, param=0x01 (!)
udelay(5000);
mutex_unlock(0x80704C84);

// Далее: schedule_delayed_work и ожидание ответа
```

**ОТЛИЧИЕ от PIC_BAT_READ2**: третий байт = 0x01 (не 0x02).
- `{0x2F, 0x00, 0x01}` — команда с polling/ответом
- `{0x2F, 0x00, 0x02}` — одноразовое чтение

---

## 9. PIC_BUZZER — 0x4133F0

### Команды:
```c
// buzzer_mode: 1 = silent, 2 = ?, 3 = beep
buf[0] = 0x34;  // BUZZER command
buf[1] = 0;     // padding
buf[2] = mode;  // 0, 3, или другое
sub_413F78(42, buf, 3);  // Write to PIC
```

| Режим | buf[2] | Описание |
|-------|--------|----------|
| 2     | 3      | Режим 2 = buzz? |
| 3     | 0      | Режим 3 = beep? |
| other | 0      | Default = off |

---

## 10. PIC_BATTERY_MONITOR — 0x413DCC

### Структура данных ($s0):
```
offset 0:  state/command
offset 4:  ???
offset 8:  RAW ADC value (сравнивается с 0x191=401 и 0x21E=542)
offset 12: mode/status (1=charging, 2=discharging?, 3=battery?)
```

### Логика решений:
```
if (mode == 1) {
    // Charging mode
    goto calib_lookup_chain
}
if (raw_adc < 0x191) {  // < 401
    // CRITICAL LOW! Prints error at 0x8067D980
    return
}
if (raw_adc < 0x21E) {  // < 542
    // LOW battery, value = 0x0B (11)
    // Call PIC_PARSE_BATTERY
    return
}
// Normal: lookup CALIB tables, call PIC_PARSE_BATTERY
```

### Пороги напряжения:
| RAW ADC | Статус |
|---------|--------|
| < 401   | КРИТИЧЕСКИ НИЗКИЙ (ошибка) |
| 401-541 | НИЗКИЙ заряд |
| >= 542  | НОРМАЛЬНЫЙ |

---

## 11. Главный PIC Worker — sub_412400

### Последовательность при загрузке:
```
1. alloc_chrdev_region(0x80704BC0, 0, 256, "AlmondPic2")
2. if (device_number == 0) -> store and return
3. Если device_number > 0:
   a) mutex_init(0x80704C84)            // I2C mutex
   b) memset(0x8159A024..., 0, 128)     // Clear state
   c) printk(...)
   d) platform_driver_register(0x81350000)  // Регистрация platform driver
   e) platform_device_register(...)
   f) sub_4127DC() — store device handle
   g) sub_412878() — calibration lookup init
   h) PIC_CALIB_LOOP(3)                 // ВЫЗОВ КАЛИБРОВКИ С count=3!
   i) jiffies -> store timer base
   j) if (workqueue exists) -> register chardev + ioctl
   k) else -> create_singlethread_workqueue
```

**КРИТИЧЕСКАЯ НАХОДКА**: `PIC_CALIB_LOOP(3)` вызывается с аргументом 3.
Это означает калибровочная таблица из 3 записей!

### Главный цикл (после init):
```
loop:
  if (MEMORY[0x80704D88] == 1) {    // pending flag from BAT_READ_CMD
      sub_413C40()                   // -> PIC_PARSE_BATTERY
      if (MEMORY[0x8159A6A0]) {      // ready flag
          PIC_BATTERY_MONITOR()
          MEMORY[0x8159A6A0] = 0
      }
      goto loop_delay
  }

  sub_414094()                       // ??? Touch processing?

loop_delay:
  for (i = 500; i > 0; i--)         // 500 * 1ms = 500ms delay
      udelay(1000)

  sub_414200(s0)                     // Read PIC response

  switch (s0->mode) {
      case 2: sub_414138(1); break   // Charging detect
      case 3:                        // Battery read
          sub_414138(s0->raw_adc)
          MEMORY[0x8159A6A0] = 1     // Set ready flag
          sub_413D94()               // Parse battery data
          break
  }

  // -> calibration voltage calculation chain
```

---

## 12. Источник калибровочных данных

### Platform driver (sub_296938) — каркас

Калибровочная структура передаётся через platform_data:
```
struct pic_calib_data {
    int ???;                    // offset 0
    int8_t count;               // offset 4 — число записей (3 в нашем случае)
    int ???;                    // offset 8
    int ???;                    // offset 12
    int voltage_ref;            // offset 16
    int32_t table1[100];        // offset 20 (0x14)  — 400 bytes
    int32_t table2[100];        // offset 420 (0x1A4) — 400 bytes
};
```

Данные загружаются из platform driver при probe(). Platform data определена
при компиляции ядра (built-in, не загружаемая). Код в `sub_292FEC` инициализирует
platform_device с этими данными.

**ДЛЯ НАШЕГО ДРАЙВЕРА**: калибровку можно пропустить (count=0), PIC будет
возвращать сырые ADC значения без коррекции.

---

## 13. Полная PIC команд таблица

| Команда | Байт | Формат | Описание |
|---------|------|--------|----------|
| BAT_READ | 0x2F | {0x2F, 0x00, mode} | Чтение батареи. mode=1: polling, mode=2: oneshot |
| WAKE/CALIB | 0x33 | {0x33, count_hi, count_lo} | Инициализация калибровки |
| BUZZER | 0x34 | {0x34, 0x00, mode} | Управление зуммером |
| CALIB_TABLE1 | 0x2D | {0x2D, data[2*count]...} | Калибровочная таблица 1 |
| CALIB_TABLE2 | 0x2E | {0x2E, data[2*count]...} | Калибровочная таблица 2 |

---

## 14. Периодическое чтение — механизм

1. `PIC_BAT_READ_CMD(timeout_sec)` устанавливает `MEMORY[0x80704D88] = 1`
2. Отправляет `{0x2F, 0x00, 0x01}` на PIC
3. Запускает delayed_work через `sub_48B38(4, ...)` с таймаутом
4. Worker loop каждые 500ms проверяет pending flag
5. Когда flag=1: вызывает PIC_PARSE_BATTERY -> PIC_READ_DATA
6. PIC_READ_DATA получает I2C ответ (SM0 auto mode READ)
7. Если `0x8159A6A0` = 1 (ready): вызывает PIC_BATTERY_MONITOR для обработки

---

## 15. ДЕЙСТВИЯ для нашего драйвера lcd_drv.ko

### Минимальная последовательность для чтения батареи:

```c
// 1. SM0 Init (один раз при загрузке)
writel(readl(0xBE000034) | 0x10000, 0xBE000034);   // Assert I2C reset
writel(readl(0xBE000034) & ~0x10000, 0xBE000034);  // Deassert
udelay(500);
writel(0x90640042, 0xBE000940);  // SM0_CTL1 — ВНИМАНИЕ: 0x90640042, НЕ 0x90644042!
writel(1, 0xBE000928);           // SM0_DEBUG
writel(0, 0xBE00090C);           // SM0_SLAVE = 0

// 2. PIC WAKE (необязательно, но стоковая прошивка делает)
uint8_t wake[] = {0x33, 0x00, 0x00};  // count=0, без калибровки
pic_i2c_write(0x2A, wake, 3);
udelay(5000);

// 3. Чтение батареи (периодически, каждые 10-30 сек)
uint8_t cmd[] = {0x2F, 0x00, 0x02};  // BAT_READ oneshot
pic_i2c_write(0x2A, cmd, 3);
udelay(5000);
// Подождать и прочитать ответ через SM0 READ

// 4. Зуммер
uint8_t buzz[] = {0x34, 0x00, 0x03};  // Buzz ON
pic_i2c_write(0x2A, buzz, 3);
```

### SM0 Auto mode WRITE:
```c
void pic_i2c_write(uint8_t addr, uint8_t *buf, int len) {
    writel(addr, 0xBE000908);        // SM0_DATA = slave addr
    writel(buf[0], 0xBE000910);      // SM0_DATAOUT = first byte
    writel(len, 0xBE000920);         // SM0_START = count
    writel(0, 0xBE00091C);           // SM0_STATUS = 0 (write)

    // Poll SM0_BUSY
    int timeout = 100000;
    while (readl(0xBE000918) & 1) {
        if (--timeout == 0) break;
    }
    // Если len > 1: записать остальные байты по одному
}
```

### SM0 Auto mode READ:
```c
int pic_i2c_read(uint8_t *buf, int len) {
    writel(len - 1, 0xBE000920);     // SM0_START = count-1
    writel(1, 0xBE00091C);           // SM0_STATUS = 1 (read)

    for (int i = 0; i < len; i++) {
        // Poll SM0_BUSY bit
        int timeout = 100000;
        while (readl(0xBE000918) & 1) {
            if (--timeout == 0) return -1;
        }
        buf[i] = readl(0xBE000914);  // SM0_DATAIN
    }
    return len;
}
```

---

## 16. ИСПРАВЛЕНИЯ к CLAUDE.md

1. **SM0_CTL1 = 0x90640042** (НЕ 0x90644042 как было записано ранее!)
   - Разница: бит 14 (0x4000) — НЕ установлен в стоковом ядре
   - Это может быть причиной проблем с PIC I2C на OpenWrt

2. **PIC_BAT_READ_CMD отправляет {0x2F, 0x00, 0x01}** (с polling)
   **PIC_BAT_READ2 отправляет {0x2F, 0x00, 0x02}** (oneshot)
   Разница в третьем байте!

3. **Калибровка необязательна** — можно отправить count=0 в WAKE (0x33)
   и работать с сырыми ADC значениями

4. **SM0_CTL0 = 0xFA** — записывается при SX8650 probe (не для PIC)

5. **Threshold значения ADC**: < 401 = CRITICAL LOW, 401-541 = LOW, >= 542 = NORMAL
