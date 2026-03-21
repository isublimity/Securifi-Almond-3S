# Анализ протокола чтения PIC батареи из стокового ядра 3.10.14

## КЛЮЧЕВОЕ ОТКРЫТИЕ

**PIC_BAT_READ_CMD НЕ ЧИТАЕТ данные напрямую!** Она только ОТПРАВЛЯЕТ команду `{0x2F, 0x00, 0x01}` на PIC и устанавливает pending flag. Фактическое чтение данных происходит в ДРУГОМ месте - в главном цикле worker thread через state machine.

## Полный путь данных: PIC hardware -> батарейное значение

### Шаг 1: Команда на чтение (PIC_BAT_READ_CMD @ 0x413138)

```c
void PIC_BAT_READ_CMD(int timeout) {
    MEMORY[0x80704D88] = 1;           // state = 1 (pending)
    lock_mutex(0x80704C84);            // sub_52A614
    udelay(5000);                      // sub_249210

    // Отправляем {0x2F, 0x00, 0x01} на адрес 0x2A (PIC)
    char cmd[3] = {0x2F, 0x00, 0x01};
    sub_413F78(0x2A, cmd, 3);          // I2C write to PIC

    udelay(5000);
    unlock_mutex(0x80704C84);          // sub_52A264

    // Запускаем таймер обратного вызова
    sub_48B38(4, timer_callback, ...);
}
```

**ВАЖНО**: `0x80704D88` = state переменная. Значение 1 = "pending bat read".

### Шаг 2: Worker thread главный цикл (sub_412400 @ 0x413534)

```
413534: lw v1, 0x4D88(s1)       // state = MEMORY[0x80704D88]
413538: li v0, 1
41353c: beq v1, v0, loc_413AF4  // if state == 1 -> goto step 3
413544: jal sub_414094           // else -> default processing
```

Цикл крутится каждые ~500 мс:
```
41354C: addiu s1, -1             // counter--
413550: jal sub_249210           // udelay(1000)  = 1ms
413554: li a0, 0x3E8
413558: bnez s1, loc_413550      // loop 500 times = 500ms
```

После 500ms задержки:
```
413564: jal sub_414200           // КЛЮЧЕВАЯ ФУНКЦИЯ - I2C READ!
413568: move a0, s0              // a0 = struct pointer
41356C: lw v0, 0xC(s0)           // state = struct->mode
413570: li v1, 2
413574: beq v0, v1, loc_41380C   // if mode == 2 -> charging
413578: li v1, 3
41357C: bne v0, v1, loc_4134A4   // if mode != 3 -> exit
413584: jal sub_414138            // mode == 3: calibration pipeline
413594: jal sub_413D94            // parse battery result
```

### Шаг 3: Когда state == 1 (pending) (loc_413AF4)

```
413AF4: jal sub_413C40           // Вызываем PIC_PARSE_BATTERY wrapper
413AF8: nop
413AFC: lw v1, 0x8159A6A0       // check ready flag
413B08: beqz v1, loc_413544      // if not ready -> back to normal loop
413B0C: sw a0=2, 0x4D88(s1)     // state = 2 (bat_read done)
413B10: jal PIC_BATTERY_MONITOR  // Обработка результата
413B14: sw zero, 0x8159A6A0     // clear ready flag
```

### Шаг 4: sub_413C40 вызывает PIC_PARSE_BATTERY (0x413CA0)

```c
void sub_413C40() {
    a0 = MEMORY[0x81391FBC];    // данные из очереди (queue pointer)
    a1 = 0x81391FAC;            // таблица калибровки
    a2 = &stack_buffer;         // буфер для результата (0x334 байт!)
    a3 = 0x334;                 // размер буфера
    PIC_PARSE_BATTERY(a0, a1, a2, a3);
}
```

### Шаг 5: PIC_PARSE_BATTERY (0x413CA0) - КРИТИЧЕСКИЙ КОД

```
413CBC: jal sub_414468           // вычисление калибровки (не I2C!)
413CC4: jal PIC_READ_DATA        // <<<< ВОТ ОНО! Swap queue
413CC8: lw a0, 8(s0)             // a0 = struct->data_ptr
413CCC: lw v1, 8(s0)             // v1 = struct->data_ptr (результат)
413CD0: li a0, -1
413CD4: bne v1, a0, parse_ok     // if data != -1 -> парсим
413CD8: ...                      // else -> return (ошибка)
```

Если данные валидны, идёт цепочка CALIB_LOOKUP:
```
413D0C: jal CALIB_LOOKUP          // lookup 1
413D24: jal CALIB_LOOKUP          // lookup 2
413D34: jal CALIB_LOOKUP          // lookup 3
413D44: jal CALIB_LOOKUP          // lookup 4 -> финальное значение
413D50: sw v0, arg_30(sp)         // сохраняем
```

### Шаг 6: PIC_READ_DATA (0x4080DC) - "Queue swap"

```c
void PIC_READ_DATA(int a1, int a2, int a3, int *a4) {
    int temp = *a4;     // a4 = указатель на текущий буфер
    *a4 = *v4;          // v4 = s6 (указатель на новый буфер)
    *v4 = temp;         // swap pointers
    JUMPOUT(0x408DDC);  // -> binary search / lookup в таблице
}
```

**Это НЕ I2C read!** Это swap указателей буферов (double buffering).
JUMPOUT на 0x408DDC - это функция бинарного поиска в таблице данных (sub_408D10).

## ГДЕ ЖЕ ФАКТИЧЕСКОЕ I2C ЧТЕНИЕ?

### sub_414200 - вот где I2C read! (вызывается в main loop)

НЕТ! sub_414200 - это просто CALIB_LOOKUP chain. Она не обращается к SM0 регистрам.

### sub_411A80 и sub_41217C - ШАБЛОНЫ I2C чтения через SM0

Эти две функции - **единственные** места где происходит реальное чтение SM0_DATAIN (0xBE000914).

#### sub_411A80 - чтение SX8650 touch (SELECT(X)=0x80)

```c
int touch_read_X() {
    SM0_CTL0 (0x908) = 0x48;      // slave addr = 0x48 (SX8650)
    SM0_START(0x920) = 0;          // reset
    udelay(150);
    SM0_DATA (0x910) = 0x80;       // SELECT(X) command
    SM0_CTL1 (0x91C) = 2;          // write 2 bytes? (addr+cmd)
    udelay(150);
    SM0_START(0x920) = 0;
    udelay(150);
    SM0_DATA (0x910) = 0x90;       // READ command (0x90 = read mode)
    SM0_CTL1 (0x91C) = 2;
    udelay(150);
    SM0_CFG  (0x900) = 0xFA;       // config (clock?)
    SM0_START(0x920) = 0;
    udelay(150);
    SM0_START(0x920) = 1;          // START read
    udelay(150);                   // <<<< WAIT
    SM0_CTL1 (0x91C) = 1;          // read 1 byte
    udelay(150);
    hi = SM0_DATAIN(0x914);        // READ byte 1 (high nibble)
    udelay(150);
    lo = SM0_DATAIN(0x914);        // READ byte 2 (low byte)
    udelay(150);
    SM0_START(0x920) = 0;          // STOP
    udelay(150);
    SM0_START(0x920) = 1;
    SM0_CTL0_RESET(0x940) = 0x8064800E;  // reset SM0
    return ((hi & 0xF) << 8) | lo;  // 12-bit value
}
```

#### sub_41217C - чтение SX8650 touch (SELECT(Y)=0x83/0x93)

Идентична sub_411A80, но:
- SM0_DATA = 0x83 (вместо 0x80) - SELECT(Y)
- SM0_DATA = 0x93 (вместо 0x90) - READ Y

### PIC_I2C_READ (0x412E78) - I2C READ для PIC

```c
void PIC_I2C_READ(int buf_ptr, int count) {
    // a0 = buf_ptr (куда складывать данные)
    // a1 = count (сколько байт читать)

    SM0_START(0x920) = count - 1;  // сколько байт читать
    SM0_CTL1 (0x91C) = 1;          // read mode

    if (count == 0) {
        // Просто проверяем SM0_STATUS
        if (SM0_STATUS(0x918) & 1)   // busy?
            goto poll_and_calibrate; // loc_413F40
        return;
    }

    // count > 0: цикл чтения байтов
    s3 = 0;  // index
    s2 = 0;  // accumulated value
    timeout = 100000;  // 0x186A0
    j loc_413EE4;      // -> в ГЛАВНЫЙ цикл обработки!
}
```

**КРИТИЧЕСКОЕ ОТКРЫТИЕ**: PIC_I2C_READ прыгает в loc_413EE4, которая находится ВНУТРИ PIC_BATTERY_MONITOR! Это значит, что PIC_I2C_READ НЕ самостоятельная функция - она является ЧАСТЬЮ огромной state machine PIC_BATTERY_MONITOR.

### PIC_I2C_WRITE (0x412F78) - I2C WRITE для PIC

```c
int PIC_I2C_WRITE(uint8_t cmd, int data_ptr, int count) {
    SM0_CTL0 (0x908) = 0x2A;      // slave addr = 0x2A (PIC!) << 1
    SM0_START(0x920) = count;      // сколько байт писать
    SM0_DATA (0x910) = cmd;        // первый байт команды
    SM0_CTL1 (0x91C) = 0;         // write mode

    if (count) {
        // Цикл записи байтов (прыгает в loc_413FEC)
        JUMPOUT(0x413FEC);
    }

    // Проверяем SM0_STATUS
    if (SM0_STATUS(0x918) & 1)     // busy?
        goto poll(0x41405C);
    return;
}
```

## SM0 РЕГИСТРЫ (palmbus I2C)

| Смещение | Регистр | Описание |
|----------|---------|----------|
| 0x900    | SM0_CFG | Конфигурация (0xFA = clock config) |
| 0x908    | SM0_CTL0 | Slave address (0x2A=PIC, 0x48=SX8650) |
| 0x910    | SM0_DATA | Данные для записи |
| 0x914    | SM0_DATAIN | Данные прочитанные (RO) |
| 0x918    | SM0_STATUS | Статус (bit0=busy, bit1=write_done, bit2=read_done) |
| 0x91C    | SM0_CTL1 | Режим: 0=write, 1=read, 2=write+read |
| 0x920    | SM0_START | Start/count: для write=count, для read=count-1 |
| 0x940    | SM0_RESET | Reset контроллера (0x8064800E) |

## ПРОТОКОЛ ЧТЕНИЯ БАТАРЕИ — ПОЛНАЯ ПОСЛЕДОВАТЕЛЬНОСТЬ

1. **PIC_BAT_READ_CMD** отправляет `{0x2F, 0x00, 0x01}` через I2C write на 0x2A:
   - SM0_CTL0 = 0x2A (PIC address)
   - SM0_DATA = 0x2F (первый байт)
   - SM0_START = 3 (count)
   - SM0_CTL1 = 0 (write mode)
   - Записывает остальные байты через state machine

2. **state = 1** (pending) установлен в 0x80704D88

3. **Worker loop** (каждые ~500ms) видит state == 1, вызывает **sub_413C40**

4. **sub_413C40** вызывает **PIC_PARSE_BATTERY** с буфером 0x334 байт

5. **PIC_PARSE_BATTERY** вызывает **PIC_READ_DATA** (queue swap), затем парсит данные через цепочку CALIB_LOOKUP

6. **НО!** Перед этим — в main loop (0x413564) вызывается **sub_414200** которая тоже работает с данными

## ГИПОТЕЗА: КТО ЗАПОЛНЯЕТ ОЧЕРЕДЬ?

PIC_READ_DATA делает swap указателей. Но КТО заполняет второй буфер?

**Варианты:**

### A) sub_413F78 (PIC I2C write function) делает write-then-read

sub_413F78 вызывается из PIC_BAT_READ_CMD с параметрами (0x2A, cmd, 3).
Она пишет данные, потом прыгает в calibration pipeline который вызывает CALIB_LOOKUP и в конце прыгает в loc_414DF8.

loc_414DF8 = sub_414DF0 — это функция которая:
```c
int sub_414DF0() {
    memset(0x8159A734, 0, 128);  // очистка буфера
    MEMORY[0x8159A738] = 0;
    MEMORY[0x8159A734] = 24;     // тип данных = 24 (0x18)

    // sub_D1E74 — это ВЕРОЯТНО i2c_transfer или аналог!
    result = sub_D1E74(prev_state, 0, 256, format_string, buffer);
    if (result < 0) {
        printk("error...");
        goto error_path;
    }
    // ... store result
}
```

**sub_D1E74** — ЭТО МОЖЕТ БЫТЬ ФУНКЦИЯ I2C READ ЧЕРЕЗ ЯДЕРНЫЙ API!

### B) Прерывание от SM0

SM0 на MT7621 может генерировать прерывание по завершении транзакции. Обработчик прерывания мог бы заполнять буфер. Но поиск IRQ обработчиков рядом с PIC кодом не дал результатов.

### C) PIC отправляет данные автономно (PIC = I2C master)

Маловероятно — PIC16LF1509 обычно slave, а MT7621 SM0 — master.

## ОТВЕТЫ НА ВОПРОСЫ

### 1. Точная последовательность после bat_read
PIC_BAT_READ_CMD -> write {0x2F,0x00,0x01} -> state=1 -> worker loop 500ms -> sub_413C40 -> PIC_PARSE_BATTERY -> PIC_READ_DATA (swap queue) -> CALIB_LOOKUP chain -> PIC_BATTERY_MONITOR

### 2. Кто вызывает PIC_I2C_READ?
НИКТО через JAL! 0 xrefs. PIC_I2C_READ (0x412E78) вызывается через fall-through или JUMPOUT из state machine. Она прыгает в loc_413EE4 (внутри PIC_BATTERY_MONITOR) что подтверждает — это часть монолитной state machine.

### 3. sub_414138 — state machine processor
Это цепочка вызовов CALIB_LOOKUP, CALIB_INTERP, sub_407C14, sub_407D4C, sub_408C8C. Она НЕ содержит I2C операций. Это чисто вычислительный модуль для калибровки/интерполяции значений батареи.

### 4. Init последовательность 0x412520
После калибровки:
- Проверка 0x81599F98 (некий флаг)
- Если флаг=0: создание kernel thread (sub_4B0B0 = kthread_create)
- Переход в main loop (0x413534)
- **Нет скрытого I2C read между calibration и main loop**

### 5. sub_40F40C — LCD/touch
Эта функция обрабатывает LCD данные. Она вызывает PIC_READ_DATA (0x4080DC) для LCD, не для PIC батареи. GPIO DIR 0x620 используется для LCD bit-bang. НЕ связана с PIC battery.

### 6. PIC автономная отправка?
НЕТ. PIC не отправляет данные автономно. MT7621 должен явно читать. Команда {0x2F,0x00,0x01} говорит PIC "подготовь данные батареи", а потом MT7621 ЧИТАЕТ ответ.

### 7. Queue в PIC_READ_DATA
PIC_READ_DATA — это double-buffering swap. Один буфер заполняется producer (I2C read), другой читается consumer (CALIB_LOOKUP). Swap атомарно переключает буферы.

**Producer** — это sub_D1E74 (вызывается из sub_414DF0 / loc_414DF8), которая ПРЕДПОЛОЖИТЕЛЬНО является ядерной функцией I2C transfer.

### 8. Прерывание?
Не найдено явных IRQ обработчиков для PIC/I2C. Вся обработка через polling в worker thread.

## КРИТИЧЕСКИЙ ВЫВОД ДЛЯ НАШЕГО ДРАЙВЕРА

### Что мы делаем НЕПРАВИЛЬНО:

1. **Мы пишем {0x2F,0x00,0x01} и СРАЗУ пытаемся читать** — это НЕПРАВИЛЬНО!
2. Стоковое ядро ждёт **500ms** после write перед read.
3. Стоковое ядро использует **state machine** с polling.

### Что нужно сделать:

1. **WRITE**: SM0_CTL0=0x2A, SM0_DATA=0x2F, SM0_START=3, SM0_CTL1=0, записать {0x2F,0x00,0x01}
2. **ЖДАТЬ 500ms** (или хотя бы 100ms)
3. **READ**: SM0_CTL0=0x2A? Или 0x2B? (read address), SM0_START=count-1, SM0_CTL1=1
4. **Polling** SM0_STATUS bit2 (read done) с timeout 100000
5. **Читать** SM0_DATAIN побайтно

### Ключевые значения SM0 регистров (из стокового кода):

Для SX8650 (рабочий пример!):
```
WRITE:  CTL0=0x48, DATA=0x80, CTL1=2, START=0  -> udelay(150)
READ:   DATA=0x90, CTL1=2 -> udelay(150) -> CFG=0xFA -> START=0 -> START=1 -> CTL1=1
READ:   DATAIN -> hi nibble, DATAIN -> lo byte
RESET:  0x940 = 0x8064800E
```

Для PIC (I2C write):
```
WRITE:  CTL0=0x2A, DATA=cmd, START=count, CTL1=0
POLL:   STATUS & 1 == 0 (busy clear)
```

Для PIC (I2C read) - из PIC_I2C_READ:
```
READ:   START=count-1, CTL1=1
POLL:   STATUS & 1 (in loop with timeout 100000)
```

### НЕДОСТАЮЩЕЕ ЗВЕНО

**SM0_CTL0 для read** — в PIC_I2C_READ НЕТ установки SM0_CTL0! Это значит что CTL0 УЖЕ установлен в 0x2A от предыдущего write. А для read нужен адрес 0x2A|1 = 0x2B? Или SM0 делает это автоматически через CTL1=1?

**SM0_DATA для read** — аналогично, в PIC_I2C_READ нет записи в SM0_DATA. Для SX8650 пишется 0x90 (read command). Для PIC — нужна ли такая команда?

### ПРЕДЛОЖЕНИЕ: Скопировать протокол SX8650

SX8650 read РАБОТАЕТ. Для PIC нужно:

```c
// Step 1: Write address + command (как уже делаем)
SM0_CTL0 = 0x2A;            // PIC slave address
SM0_DATA = 0x2F;             // battery read command
SM0_START = 3;               // 3 bytes
SM0_CTL1 = 0;                // write mode
// poll SM0_STATUS bit1

// Step 2: Wait 500ms for PIC to prepare data

// Step 3: Read response (КОПИРУЕМ протокол SX8650!)
SM0_CTL0 = 0x2A;             // PIC slave address (для SM0 НЕ нужен |1?)
SM0_DATA = ???;               // Может не нужен для PIC
SM0_START = 0;                // reset
udelay(150);
SM0_START = 1;                // start read
udelay(150);
SM0_CTL1 = 1;                // read 1 byte
udelay(150);
byte1 = SM0_DATAIN;
udelay(150);
byte2 = SM0_DATAIN;
// ...
SM0_START = 0;                // stop
SM0_START = 1;
SM0_RESET = 0x8064800E;
```

## ДОПОЛНИТЕЛЬНЫЕ ОТКРЫТИЯ

### sub_D1E74 — ЗАГЛУШКА!

sub_D1E74 просто возвращает -14 (EFAULT). Это **не** I2C transfer функция.
```
d1e74: li $v0, 0xFFFFFFF2    // return -14 = -EFAULT
```

Значит loc_414DF8 (sub_414DF0) — это НЕ путь получения данных из I2C. Это общий выход state machine, который передаёт уже прочитанные данные в netlink/userspace через message queue (sub_415DC8 = message passing, sub_416BF0 = send notification).

### Вторая команда bat_read (0x413094)

Обнаружена ВТОРАЯ функция bat_read на 0x413094:
```c
void pic_bat_read_v2() {
    lock_mutex(0x80704C84);
    udelay(5000);

    // Отправляем {0x2F, 0x00, 0x02} — третий байт = 2!
    char cmd[3] = {0x2F, 0x00, 0x02};
    sub_413F78(0x2A, cmd, 3);

    udelay(5000);
    unlock_mutex(0x80704C84);
}
```

Третий байт **0x02** (вместо 0x01) — это возможно ДРУГОЙ тип запроса (charging status?).

### Данные между функциями

Между PIC_I2C_READ и PIC_I2C_WRITE (0x412EDC-0x412F10) находятся ДАННЫЕ (не код!):
```
0x412EDC: 0A 00 18 04 FB FF 84 0A 14 21 00 01 F1 21
```
Аналогично между PIC_I2C_WRITE и следующей (0x412FE4-0x41302C):
```
0x412FE4: 0F 00 18 02 FB FF 0F FF 84 E8 FD FF 01 21 00 10 01 EC 21
```
Это могут быть **таблицы калибровки** или **шаблоны I2C команд** для state machine.

## ОКОНЧАТЕЛЬНЫЙ ВЫВОД

### Путь данных PIC -> battery value:

```
1. PIC_BAT_READ_CMD: WRITE {0x2F, 0x00, 0x01} to PIC (addr 0x2A)
   |
   v
2. State = 1 (pending)
   |
   v  (500ms timer)
3. Worker thread main loop
   |
   v
4. sub_413C40 -> PIC_PARSE_BATTERY -> PIC_READ_DATA (queue swap)
   |
   v
5. PIC_BATTERY_MONITOR: проверяет struct->mode
   - mode 1: read pending -> CALIB_CALC2 -> PIC_PARSE_BATTERY -> sub_414CDC
   - mode 2: charging
   - mode 3: battery data ready -> sub_414138 (calibration) -> sub_413D94 (parse)
```

### КЛЮЧЕВОЕ: SM0 read для PIC НЕ найден в явном виде

PIC_I2C_READ устанавливает SM0_START и SM0_CTL1=1 (read mode), но сам read loop (loc_413EE4) прыгает в PIC_BATTERY_MONITOR, который делает CALIB_CALC/CALIB_LOOKUP. Возможно SM0 делает read автоматически после установки CTL1=1 + START, а данные появляются в SM0_DATAIN через hardware polling.

### ПРАКТИЧЕСКИЕ РЕКОМЕНДАЦИИ ДЛЯ НАШЕГО ДРАЙВЕРА

1. **Отправляем {0x2F, 0x00, 0x01}** через bit-bang I2C write (как сейчас — работает, мелодия подтверждает)

2. **Ждём 500ms** (PIC готовит данные)

3. **Для чтения ответа PIC** используем протокол аналогичный SX8650 touch read:
   ```c
   // Адрес PIC = 0x2A (write) / 0x2B (read)
   SM0_CTL0 = 0x2A;           // или 0x2B для read?
   SM0_START = 0;              // reset
   udelay(150);
   SM0_START = count - 1;      // из PIC_I2C_READ
   SM0_CTL1 = 1;               // read mode (из PIC_I2C_READ)
   // poll SM0_STATUS & 1
   udelay(150);
   byte = SM0_DATAIN;          // читаем данные
   ```

4. **SM0_CTL1 значения** (из анализа):
   - 0 = write mode
   - 1 = read 1 byte / read mode
   - 2 = write+read combined (используется в SX8650)

5. **SM0_START значения**:
   - Для write: count (количество байт)
   - Для read: count - 1 (из PIC_I2C_READ: `addiu $v1, $a1, -1; sw $v1, 0xBE000920`)

6. **SM0 reset** после операции: `SM0_0x940 = 0x8064800E` (из SX8650 read)

7. **SM0_CFG = 0xFA** нужно устанавливать перед read (из SX8650 read)

### СЛЕДУЮЩИЙ ШАГ

Попробовать в нашем lcd_drv.ko:
```c
// После write {0x2F, 0x00, 0x01}:
msleep(500);

// Read attempt:
iowrite32(0x2A, base + 0x908);  // CTL0 = PIC addr
iowrite32(0, base + 0x920);      // START = 0 (reset)
udelay(150);
iowrite32(0xFA, base + 0x900);   // CFG
iowrite32(0, base + 0x920);      // START = 0
udelay(150);
iowrite32(1, base + 0x920);      // START = 1 (begin)
udelay(150);
iowrite32(1, base + 0x91C);      // CTL1 = 1 (read)
udelay(150);
byte1 = ioread32(base + 0x914);  // DATAIN
udelay(150);
byte2 = ioread32(base + 0x914);  // DATAIN
iowrite32(0, base + 0x920);      // STOP
udelay(150);
iowrite32(1, base + 0x920);
iowrite32(0x8064800E, base + 0x940);  // RESET SM0
```
