# Реверс PIC Buzzer (AlmondPic2) - IDA анализ

## Общие сведения

PIC16LF1509 на Almond 3S имеет встроенный буззер (пьезо/динамик), управляемый через I2C команду `0x34`.
Адрес PIC на I2C шине: **0x2A**.

Анализ выполнен на стоковом ядре 3.10.14 (бинарный дамп), модуль `AlmondPic2_main.ko` (built-in).

---

## 1. Функция PIC_BUZZER (0x4133C0 - 0x413454)

### Полный дизассемблер

IDA показывает функцию с 0x4133F0, но реальный пролог начинается с 0x4133C0:

```
# Пролог (0x4133C0..0x4133F0, IDA не видит как функцию):
4133C0  addiu   $sp, -0x28
4133C4  sw      $s1, 0x20($sp)
4133C8  lui     $s1, 0x8070          # base addr для mutex
4133CC  sw      $s0, 0x1C($sp)
4133D0  move    $s0, $a0             # s0 = mode (аргумент)
4133D4  addiu   $a0, $s1, 0x4C84     # mutex addr = 0x80704C84
4133D8  sw      $ra, 0x24($sp)
4133DC  jal     sub_52A614           # spin_lock_irqsave(mutex)
4133E0  sw      $zero, 0x10($sp)     # buf[0] = 0 (delay slot)
4133E4  jal     sub_249210           # msleep()
4133E8  li      $a0, 0x1388          # msleep(5000)  - пауза 5 сек!
4133EC  li      $v0, 1               # v0 = 1

# Основной код PIC_BUZZER (0x4133F0):
4133F0  beq     $s0, $v0, loc_413458 # if (mode == 1) goto SKIP_1
4133F4  li      $v0, 2               # (delay slot) v0 = 2
4133F8  beq     $s0, $v0, loc_413460 # if (mode == 2) goto SKIP_2
4133FC  li      $v0, 3               # (delay slot) v0 = 3
413400  xori    $v1, $s0, 3          # v1 = mode ^ 3
413404  movn    $v0, $zero, $v1      # if (v1 != 0) v0 = 0, else v0 = 3
413408  move    $v1, $v0             # v1 = mode_byte
41340C  addiu   $a1, $sp, 0x10       # a1 = &buf (стек)
413410  li      $a2, 3               # a2 = 3 (длина)
413414  li      $v0, 0x34            # v0 = 0x34 (команда BUZZER)
413418  li      $a0, 0x2A            # a0 = 0x2A (I2C addr PIC)
41341C  sb      $v1, 0x12($sp)       # buf[2] = mode_byte
413420  sb      $v0, 0x10($sp)       # buf[0] = 0x34
413424  jal     sub_413F78           # PIC_I2C_WRITE(0x2A, buf, 3)
413428  sb      $zero, 0x11($sp)     # buf[1] = 0x00 (delay slot)

# После отправки:
41342C  lui     $v0, 0x815A
413430  li      $a0, 0x1388
413434  jal     sub_249210           # msleep(5000) - ещё 5 сек пауза
413438  sb      $s0, 0x8159A035      # buzzer_current_mode = mode (global)
41343C  jal     sub_52A264           # spin_unlock_irqrestore(mutex)
413440  addiu   $a0, $s1, 0x4C84     # mutex addr
413444  lw      $ra, 0x24($sp)
413448  lw      $s1, 0x20($sp)
41344C  lw      $s0, 0x1C($sp)
413450  jr      $ra                  # return
413454  addiu   $sp, 0x28

# SKIP_1 (mode == 1):
413458  j       loc_41440C           # перенаправление в калибровку
41345C  li      $v1, 1               # (delay slot) v1 = 1

# SKIP_2 (mode == 2):
413460  j       loc_41440C           # перенаправление в калибровку
413464  li      $v1, 2               # (delay slot) v1 = 2
```

### Декомпилированный псевдокод

```c
void PIC_BUZZER(int mode) {
    spin_lock_irqsave(&pic_mutex);  // 0x80704C84
    msleep(5000);

    if (mode == 1) {
        // НЕ отправляет команду буззера!
        // Перенаправляет в обработчик калибровки с v1=1
        goto calibration_handler;
    }
    if (mode == 2) {
        // НЕ отправляет команду буззера!
        // Перенаправляет в обработчик калибровки с v1=2
        goto calibration_handler;
    }

    // Формируем I2C команду буззера
    uint8_t buf[3];
    buf[0] = 0x34;        // команда BUZZER
    buf[1] = 0x00;        // зарезервировано

    if (mode == 3)
        buf[2] = 0x03;    // BUZZER ON
    else
        buf[2] = 0x00;    // BUZZER OFF

    PIC_I2C_WRITE(0x2A, buf, 3);  // отправляем на PIC

    buzzer_current_mode = mode;    // сохраняем в глобал 0x8159A035
    msleep(5000);
    spin_unlock_irqrestore(&pic_mutex);
}
```

---

## 2. I2C протокол команды Buzzer

### Формат команды

| Байт | Значение | Описание |
|------|----------|----------|
| buf[0] | 0x34 | Код команды BUZZER |
| buf[1] | 0x00 | Зарезервировано (всегда 0) |
| buf[2] | mode | Режим: 0x00=OFF, 0x03=ON |

### Отправка

```
I2C Write: addr=0x2A, data={0x34, 0x00, mode}, len=3
```

Используется SM0 (palmbus I2C), НЕ Linux i2c_transfer.
Функция PIC_I2C_WRITE по адресу 0x80413F78 (sub_413F78 в IDB).

---

## 3. Режимы буззера

| mode (a0) | buf[2] | Эффект | Примечание |
|-----------|--------|--------|------------|
| 0 | 0x00 | **ВЫКЛ** | Буззер выключен |
| 1 | --- | Не буззер | Перенаправляет в калибровку батареи |
| 2 | --- | Не буззер | Перенаправляет в калибровку батареи |
| 3 | 0x03 | **ВКЛ** | Буззер включен |
| другое | 0x00 | **ВЫКЛ** | Любое значение кроме 3 = выключение |

**Вывод**: Буззер работает как простой ON/OFF. PWM частотой НЕ управляется через I2C -
PIC сам генерирует звук. Управление = два состояния:
- `{0x34, 0x00, 0x03}` = **ВКЛ** (PIC включает буззер)
- `{0x34, 0x00, 0x00}` = **ВЫКЛ** (PIC выключает буззер)

---

## 4. Глобальные переменные

| Адрес (KVA) | IDB | Описание |
|-------------|-----|----------|
| 0x8159A035 | - | `buzzer_current_mode` - текущее состояние буззера (1 байт) |
| 0x8159A034 | - | Соседняя переменная, сбрасывается в 0 при init (0x4128CC) |
| 0x80704C84 | - | Mutex/spinlock PIC I2C (используется всеми PIC функциями) |

---

## 5. Кто вызывает PIC_BUZZER?

### Прямых вызовов нет!

Поиск JAL к 0x4133C0 и 0x4133F0 по всему ядру (0x400000-0x700000) - **0 результатов**.

PIC_BUZZER вызывается ТОЛЬКО через:
1. **IOCTL jump table** (косвенный вызов через `jr $v0`)
2. Таблица переходов PIC_IOCTL по адресу 0x80597AE0 (BSS, заполняется при init модуля)
3. PIC_IOCTL (0x413468) читает номер команды из struct, проверяет < 16, прыгает через таблицу

### Цепочка вызовов

```
Userspace: ioctl(fd, cmd, arg) на /dev/AlmondPic2
  -> Kernel: PIC_IOCTL(arg)        @ 0x413468
     -> jump_table[cmd](arg)       @ 0x80597AE0[cmd]
        -> PIC_BUZZER(mode)        @ 0x4133C0  (при cmd = N_BUZZER)
           -> PIC_I2C_WRITE(0x2A, {0x34, 0x00, mode}, 3) @ 0x413F78
```

### IOCTL номер для буззера

Точный IOCTL номер НЕ ОПРЕДЕЛЁН из статического анализа, т.к. jump table в BSS
заполняется при runtime init_module. Номер = индекс (0-15) в таблице,
куда записан адрес 0x804133C0.

**Метод определения**: на рабочем стоковом роутере можно:
1. `cat /proc/kallsyms | grep almond_pic` - найти адреса
2. Дампнуть jump table из `/dev/mem` по адресу 0x1E597AE0 (физ) или 0x80597AE0 (KVA)
3. Или подсмотреть через strace userspace демона AlmondPic

---

## 6. PIC_IOCTL (0x413468) - диспетчер команд

### Дизассемблер

```
PIC_IOCTL:
413468  addiu   $sp, -0x30
41346C  sw      $s0, 0x1C($sp)
413470  move    $s0, $a0             # s0 = arg struct
413474  li      $a0, 0x3E8
41348C  jal     sub_249210           # msleep(1000)
413494  lw      $v0, 0($s0)          # v0 = ioctl_cmd = *(uint32_t*)arg
413498  sltiu   $v1, $v0, 0x10       # check cmd < 16
41349C  bnez    $v1, loc_4134C4      # if valid -> jump table
# else: return 0 (invalid cmd)
4134A4  move    $v0, $zero
4134BC  jr      $ra

# Jump table dispatch:
4134C4  sll     $v0, 2               # cmd *= 4
4134C8  li      $v1, 0x80597AE0      # table base
4134CC  addu    $v0, $v1, $v0        # &table[cmd]
4134D0  lw      $v0, 0($v0)          # handler = table[cmd]
4134D4  jr      $v0                  # goto handler
```

### 16 IOCTL команд (0-15)

Точное соответствие номеров и хендлеров можно восстановить только из runtime.
Но из анализа кода вокруг PIC_IOCTL известны следующие обработчики:

| I2C cmd | Описание | Адрес обработчика |
|---------|----------|-------------------|
| 0x2F | Battery read | PIC_BAT_READ2 (0x413094), PIC_BAT_READ_CMD (0x413138) |
| 0x30 | Неизвестно | ~0x4136A8 |
| 0x32 | Неизвестно | ~0x41371C |
| 0x33 | Calibrate start | PIC_CALIBRATE (0x413200) |
| 0x34 | **BUZZER** | PIC_BUZZER (0x4133C0) |
| 0x36 | Неизвестно | ~0x413894 |
| 0x37 | Неизвестно | ~0x4139F0 |
| 0x39 | Неизвестно | ~0x413B84 |
| 0x40 | Неизвестно (с 0x37) | ~0x4139F0 |
| 0x41 | Первый IOCTL case | ~0x4134DC |

---

## 7. Строки, связанные с PIC/Buzzer

Поиск по строкам ядра:

| Адрес | Строка | Контекст |
|-------|--------|----------|
| 0x5E7C60 | `PIC_h>l` | Вероятно printk при PIC high->low transition |
| 0x67C7AC | `almond_pic_handler` | Имя IRQ/workqueue handler |

Строки "buzz", "beep", "alarm", "sound", "ring", "tone", "speaker" - **НЕ НАЙДЕНЫ**.
Это значит что PIC buzzer управляется только через числовые команды без текстовых логов.

---

## 8. Как управлять буззером из нашей OpenWrt прошивки

### Через lcd_drv.ko (наш модуль)

Добавить IOCTL или sysfs интерфейс для отправки I2C команды:

```c
// Включить буззер
uint8_t buf[3] = {0x34, 0x00, 0x03};
pic_i2c_write(0x2A, buf, 3);  // через palmbus SM0

// Выключить буззер
uint8_t buf[3] = {0x34, 0x00, 0x00};
pic_i2c_write(0x2A, buf, 3);
```

### Из userspace (echo в /dev/lcd)

Можно добавить команды в lcd_drv.ko:
```bash
echo "buzzer on" > /dev/lcd    # включить
echo "buzzer off" > /dev/lcd   # выключить
```

### Важные замечания

1. **msleep(5000)** - стоковый драйвер делает паузу 5 секунд ДО и ПОСЛЕ отправки команды.
   Это может быть необходимо для стабильности I2C, или просто оверинжиниринг.
   Рекомендую начать с msleep(100) и увеличивать при проблемах.

2. **spin_lock** - PIC I2C шина шарится между touch, battery и buzzer.
   Нужна синхронизация с touch thread и battery polling.

3. **Нет PWM** - буззер = ON/OFF, частота/тональность задана аппаратно в прошивке PIC16.
   Для разных паттернов (бип-бип, длинный бип) нужно управлять таймингом из ядра:
   ```c
   buzzer_on();
   msleep(200);   // бип 200мс
   buzzer_off();
   msleep(100);   // пауза
   buzzer_on();
   msleep(200);   // бип 200мс
   buzzer_off();
   ```

---

## 9. Полная карта PIC функций (0x412000-0x415000)

| Адрес | Размер | Имя | Описание |
|-------|--------|-----|----------|
| 0x41217C | 0x10C | sub_41217C | Инициализация? |
| 0x412288 | 0x178 | sub_412288 | |
| 0x412400 | 0x1B0 | sub_412400 | Главный обработчик PIC |
| 0x4127DC | 0x24 | sub_4127DC | Store value |
| 0x412878 | 0x10 | sub_412878 | Калибровка entry |
| 0x412A80 | 0x1C | sub_412A80 | |
| 0x412B6C | 0x20 | sub_412B6C | |
| 0x412B8C | 0x08 | sub_412B8C | |
| 0x412B94 | 0x18 | sub_412B94 | |
| 0x412BAC | 0x5C | sub_412BAC | |
| 0x412C08 | 0x38 | sub_412C08 | |
| 0x412C40 | 0x28 | sub_412C40 | |
| 0x412C68 | 0x28 | sub_412C68 | |
| 0x412CA0 | 0xA8 | sub_412CA0 | |
| 0x412D4C | 0x48 | sub_412D4C | |
| 0x412D94 | 0x38 | sub_412D94 | |
| 0x412DCC | 0x38 | sub_412DCC | |
| 0x412E04 | 0x6C | sub_412E04 | |
| 0x412E78 | 0x100 | **PIC_I2C_READ** | I2C read через SM0 palmbus |
| 0x412F78 | 0x11C | **PIC_I2C_WRITE** | I2C write через SM0 palmbus |
| 0x413094 | 0x64 | **PIC_BAT_READ2** | Чтение батареи (вторичное) |
| 0x4130F8 | 0x40 | sub_4130F8 | |
| 0x413138 | 0xC8 | **PIC_BAT_READ_CMD** | Команда чтения батареи |
| 0x413200 | 0x88 | **PIC_CALIBRATE** | Калибровка батареи |
| 0x413288 | 0x108 | **PIC_CALIB_LOOP** | Цикл калибровки |
| 0x4133F0 | 0x60 | **PIC_BUZZER** | Управление буззером |
| 0x413468 | 0x3C | **PIC_IOCTL** | IOCTL диспетчер (16 команд) |
| 0x413C40 | 0x34 | sub_413C40 | Battery parse wrapper |
| 0x413CA0 | 0xF4 | **PIC_PARSE_BATTERY** | Парсинг данных батареи |
| 0x413D94 | 0x38 | sub_413D94 | |
| 0x413DCC | 0x164 | **PIC_BATTERY_MONITOR** | Мониторинг батареи |
| 0x413F78 | 0xF8 | **sub_413F78** | PIC_I2C_WRITE (реальная) |
| 0x414094 | 0x10 | sub_414094 | |
| 0x414138 | 0xC8 | sub_414138 | Калибровка interp |
| 0x414200 | 0x30 | sub_414200 | |

---

## 10. Все PIC I2C команды (полная таблица)

| Cmd (buf[0]) | Описание | buf[1] | buf[2] | Где используется |
|-------------|----------|--------|--------|------------------|
| 0x2E | Калибровка завершена/ошибка | ? | ? | PIC_CALIBRATE (0x4133A0) |
| 0x2F | Запрос данных батареи | 0x02 | - | PIC_BAT_READ2, PIC_BAT_READ_CMD |
| 0x30 | Неизвестно | ? | ? | IOCTL case (0x4136A8) |
| 0x32 | Неизвестно | ? | ? | IOCTL case (0x41371C) |
| 0x33 | Запуск калибровки | sign | value | PIC_CALIBRATE (0x41326C) |
| **0x34** | **BUZZER** | **0x00** | **mode** | **PIC_BUZZER (0x413424)** |
| 0x36 | Неизвестно | ? | ? | IOCTL case (0x413894) |
| 0x37 | Неизвестно | ? | ? | IOCTL case (0x4139F0) |
| 0x39 | Неизвестно | ? | ? | IOCTL case (0x413B84) |
| 0x40 | Неизвестно | ? | ? | IOCTL case (0x4139F0, совм. с 0x37) |
| 0x41 | Неизвестно | ? | ? | Первый IOCTL case (0x4134F8) |

---

## 11. Реализация для lcd_drv.ko

Минимальный код для управления буззером (добавить в существующий модуль):

```c
/* PIC Buzzer control via SM0 I2C */
static void pic_buzzer_set(int on)
{
    uint8_t buf[3];
    buf[0] = 0x34;    /* BUZZER command */
    buf[1] = 0x00;    /* reserved */
    buf[2] = on ? 0x03 : 0x00;  /* ON=3, OFF=0 */

    mutex_lock(&pic_mutex);
    msleep(100);  /* стоковый использует 5000, попробуем 100 */
    pic_sm0_write(0x2A, buf, 3);
    msleep(100);
    mutex_unlock(&pic_mutex);
}

/* В обработчике /dev/lcd write: */
if (strncmp(buf, "buzzer on", 9) == 0)
    pic_buzzer_set(1);
else if (strncmp(buf, "buzzer off", 10) == 0)
    pic_buzzer_set(0);
else if (strncmp(buf, "beep", 4) == 0) {
    /* Короткий бип */
    pic_buzzer_set(1);
    msleep(200);
    pic_buzzer_set(0);
}
```

---

*Анализ выполнен через IDA MCP на бинарном дампе стокового ядра 3.10.14*
*Адреса в формате IDB (без 0x80000000 prefix). KVA = IDB + 0x80000000*
