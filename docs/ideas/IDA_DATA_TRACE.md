# Полная трассировка данных батареи PIC16 из IDA

## Дата анализа: 2026-03-19 (обновлено 2026-03-19, сеанс #2)

## КРИТИЧЕСКОЕ ОТКРЫТИЕ

**PIC_BAT_READ_CMD и PIC_BAT_READ2 только ПИШУТ команду на PIC, но НИКОГДА НЕ ЧИТАЮТ ответ через I2C.**
Фактическое чтение данных батареи происходит через ОТДЕЛЬНЫЙ механизм: kernel worker thread
с состоянием-автоматом (state machine), который вызывает PIC_I2C_READ как часть
fall-through в огромной монолитной функции.

## ОБНОВЛЕНИЕ: Подтверждено в сеансе #2

### Read loop ВЕРИФИЦИРОВАН из raw bytes
IDA не смогла дизассемблировать область 0x412EDC-0x412F14 (показывала .byte),
но ручная декодировка raw MIPS LE инструкций ПОДТВЕРДИЛА read loop:
- `0x412EE4: lw $v1, 0x918($s0)` - poll SM0_POLLSTA
- `0x412EE8: andi $v1, $v1, 0x04` - проверка bit 2
- `0x412EFC: lw $v0, 0x914($s0)` - READ SM0_DATAIN

### Полная цепочка вызовов подтверждена:
```
PIC_BAT_READ_CMD (0x413138)
  -> sub_413F78(0x2A, {0x2F,0x00,0x01}, 3)  -- I2C WRITE
  -> MEMORY[0x80704D88] = 1                   -- set pending
  -> sub_48B38(4, val, 0x80704DCC, timer)     -- schedule work

Worker loop (sub_412400 @ 0x413534):
  -> checks MEMORY[0x80704D88] == 1
  -> jal sub_413C40 -> PIC_PARSE_BATTERY -> PIC_READ_DATA (queue swap)
  -> if 0x8159A6A0 != 0:
     -> PIC_BATTERY_MONITOR (0x413EE4) -> CALIB chain -> sub_414CDC
  -> else:
     -> state = 2, back to normal loop
```

### Ответ 0x55 объяснён:
0x55 = (0x2A << 1) | 1 = I2C read address byte. MT7621 SM0 не перезаписывает
SM0_DATA перед READ, используя 0x2A от предыдущего WRITE. Бит R/W=1 добавляется
аппаратно, давая 0x55 на шине. PIC ACK это и отправляет данные.

---

## Q1: Как PIC_I2C_READ (0x412E78) читает данные?

### Полный дизассемблер PIC_I2C_READ

```mips
; PIC_I2C_READ(a0=buffer, a1=byte_count)
; Аргументы: $a0 = адрес буфера для данных, $a1 = количество байт
;
412e78  addiu   $sp, -0x30
412e7c  lui     $v0, 0xBE00            ; базовый адрес palmbus
412e80  addiu   $v1, $a1, -1           ; count - 1
412ea4  sw      $v1, 0xBE000920        ; SM0_START = count-1
412ea8  li      $v1, 1
412eac  move    $s4, $a1               ; $s4 = byte_count
412eb0  move    $s6, $a0               ; $s6 = dest_buffer
412eb4  sw      $v1, 0xBE00091C        ; SM0_STATUS = 1 (READ mode!)
412eb8  beqz    $a1, loc_412F14        ; если 0 байт → пропуск
412ebc  nop

; Инициализация read loop
412ec0  lui     $s5, 1
412ec4  move    $s3, $zero             ; $s3 = current index (0)
412ec8  move    $s2, $zero             ; $s2 = byte counter (0)
412ecc  li      $s5, 0x186A0           ; timeout = 100000 (0x186A0)
412ed0  lui     $s0, 0xBE00            ; $s0 = SM0 base
412ed4  j       loc_412EDC             ; → read loop body
412ed8  move    $v0, $s5              ; $v0 = timeout counter

; === READ LOOP BODY (декодировано из raw bytes) ===
412edc  beq     $v0, $zero, 0x412F08   ; timeout → skip to next byte
412ee0  nop
412ee4  lw      $v1, 0x918($s0)        ; читаем SM0_POLLSTA
412ee8  andi    $v1, $v1, 0x4          ; проверяем бит 2 (DATAIN ready!)
412eec  beq     $v1, $zero, 0x412EDC   ; если не готов → повторяем poll
412ef0  addiu   $v0, $v0, -1           ; timeout--
; Бит готов! Читаем данные:
412ef4  jal     0x249210               ; udelay(10)
412ef8  addiu   $a0, $zero, 10         ; arg = 10 мкс
412efc  lw      $v0, 0x914($s0)        ; ЧИТАЕМ SM0_DATAIN!
412f00  addu    $s3, $s6, $s3          ; addr = buffer + index
412f04  sb      $v0, 0x0($s3)          ; buffer[index] = byte
412f08  addiu   $s2, $s2, 1            ; byte_counter++
412f0c  bne     $s2, $s4, 0x412ED4     ; if (counter != total) → next byte
412f10  addu    $s3, $s2, $zero        ; index = counter
```

### Алгоритм чтения:

1. `SM0_START (0x920)` = byte_count - 1
2. `SM0_STATUS (0x91C)` = 1 (режим READ)
3. Цикл для каждого байта:
   - Poll `SM0_POLLSTA (0x918)` бит **0x04** (бит 2!) с timeout 100000
   - `udelay(10)` после готовности
   - Читаем `SM0_DATAIN (0x914)` — один байт
   - Сохраняем в `buffer[index]`
4. Повторяем для всех byte_count байт

**КЛЮЧЕВОЕ ОТЛИЧИЕ от нашего кода:**
- Стоковое ядро проверяет бит **0x04** (bit 2) в SM0_POLLSTA для READ
- Стоковое ядро проверяет бит **0x02** (bit 1) в SM0_POLLSTA для WRITE
- Стоковое ядро проверяет бит **0x01** (bit 0) в SM0_POLLSTA для completion check

---

## Q2: Кто вызывает PIC_I2C_READ?

PIC_I2C_READ по адресу 0x412E78 имеет **0 прямых JAL xrefs**. Она является частью
**монолитной функции-автомата**, которая занимает область ~0x412400 - 0x416000.

IDA видит эту функцию как несколько "sub-функций" из-за spaghetti code (J/BNE
между далёкими адресами), но реально это ОДИН kernel thread (kthread) с state machine.

PIC_I2C_READ вызывается через **fall-through** из worker loop когда state machine
переходит в состояние чтения данных.

Важно: `sub_413F78` (высокоуровневый I2C write wrapper) имеет xref из PIC_I2C_READ
по адресу 0x413F74 — это означает что PIC_I2C_READ ТАКЖЕ может инициировать I2C write
как часть combined write-read транзакции!

---

## Q3: Когда стоковое ядро ЧИТАЕТ данные с PIC?

### Рабочий цикл (Worker Loop) — sub_412400

Полная state machine:

```
ИНИЦИАЛИЗАЦИЯ (sub_412400):
  1. sub_D1E74() — создать kernel thread
  2. sub_53910() — зарегистрировать устройство
  3. sub_296938() — создать DMA/workqueue
  4. sub_4127DC() — hardware init
  5. sub_412878() — hardware init #2
  6. PIC_CALIB_LOOP(3) — калибровка PIC (I2C write 0x2A)
  7. sub_33AD8(0x3E8) — sleep 1 сек

MAIN LOOP (0x413534):
  Проверяем MEMORY[0x80704D88] (state variable):

  state == 1 → sub_413AF4:
    jal sub_413C40    ; → PIC_PARSE_BATTERY → PIC_READ_DATA (queue swap)
    if (MEMORY[0x8159A6A0]) → PIC_BATTERY_MONITOR()
    else → state = 2, continue loop

  state == 2 → sub_41380C:
    jal sub_414138(1)  ; Большая state machine обработки данных
    → loc_4144A8 (post-processing)

  state == 3 → sub_413584:
    jal sub_414138(value)
    set MEMORY[0x8159A6A0] = 1  ; pending flag для battery read
    jal sub_413D94              ; Parse battery response
    → loc_4144A8

  state == другое → return 0

МЕЖДУ ИТЕРАЦИЯМИ:
  sub_414094()          ; cleanup/prep
  DELAY LOOP:
    s1 = 0x1F4 (500)
    for (i=500; i>0; i--):
      udelay(1000)      ; 1 мс × 500 = 500 мс задержка!
  sub_414200()          ; Read PIC response (через DMA queue?)
  Проверяем $s0+0xC:
    == 2 → state 2 processing
    == 3 → state 3 processing
```

### Последовательность чтения батареи:

1. **PIC_BAT_READ_CMD(n)** вызывается (откуда-то извне, через ioctl или timer):
   - Устанавливает `MEMORY[0x80704D88] = 1` (state = PENDING_READ)
   - Отправляет I2C WRITE `{0x2F, 0x00, 0x01}` на адрес 0x2A
   - Запускает таймер `sub_48B38(4, timeout, callback, arg)`

2. Worker loop видит state == 1:
   - Вызывает `sub_413C40` → `PIC_PARSE_BATTERY` → `PIC_READ_DATA`
   - `PIC_READ_DATA` извлекает данные из **очереди** (queue swap), НЕ читает I2C
   - Если pending flag установлен → `PIC_BATTERY_MONITOR()`

3. **PIC_BATTERY_MONITOR** (0x413E78):
   - Проверяет значение в `struct+0xC` (тип запроса)
   - Проверяет значение в `struct+0x8` (raw value) — диапазоны 0x191..0x21E
   - Вызывает `CALIB_CALC2()` для конвертации raw → calibrated
   - Вызывает `PIC_PARSE_BATTERY()` для финальной обработки

---

## Q4: Что такое sub_414200?

```c
void sub_414200() {
  v1 = CALIB_LOOKUP(v0, MEMORY[0x8067DDD8]);
  sub_407C14(v1, MEMORY[0x8067DE04]);
  JUMPOUT(0x414DF8); // → переход к следующему потоку
}
```

Это часть state machine: обработка текущих данных через lookup таблицу калибровки.
Вызывается в main loop после 500мс задержки. Результат определяет следующее состояние.

---

## Q5: Что такое sub_4130F8?

```c
int sub_4130F8() {
  sub_414094();                    // Cleanup/preparation
  if (MEMORY[0x8159A6A0]) {       // Pending battery read flag
    MEMORY[0x8159A6A0] = 0;       // Clear flag
    PIC_BATTERY_MONITOR();         // Обработать данные батареи
  }
  MEMORY[0x80704D88] = 2;         // Set state = 2
  return ...;
}
```

sub_414094 — просто переход к loc_414DF8 (начало нового цикла DMA/thread).

---

## Q6: Формат 8-байтного ответа PIC

PIC возвращает: `55 00 00 00 39 3e 01 e6`

### Как PIC_PARSE_BATTERY (0x413CA0) интерпретирует данные:

```mips
; PIC_PARSE_BATTERY вызывает:
413cbc  jal     sub_414468    ; prep/lookup
413cc4  jal     PIC_READ_DATA ; извлечь данные из очереди
413cc8  lw      $a0, 8($s0)  ; загружаем struct+8 = RAW VALUE

; Проверяем: если struct+8 == -1 (0xFFFFFFFF) → return (нет данных)
413cd0  li      $a0, 0xFFFFFFFF
413cd4  bne     $v1, $a0, loc_413D0C

; Если данные есть → цепочка калибровочных lookup:
413d0c  CALIB_LOOKUP(result, MEMORY[0x8067DDBC])  ; lookup #1
413d14  CALIB_LOOKUP(result, MEMORY[0x8067DDC0])  ; lookup #2
413d24  CALIB_LOOKUP(result, MEMORY[0x8067DDC4])  ; lookup #3
413d34  CALIB_LOOKUP(result, MEMORY[0x8067DD88])  ; lookup #4
413d44  ; Результат → arg_30 (сохранённое значение)

; Проверяем флаги:
413d54  lw      $v0, 0x8159A6CC   ; charger connected?
413d58  bnez    → loc_413F38 (charger path)

413d64  lw      $a0, 0x8159A6D0   ; battery status
413d70  beq     $a0, 1 → loc_414230 (full/charging complete)

; Основной путь — калибровка напряжения:
413d7c  CALIB_INTERP(value, MEMORY[0x8067DDCC])
413d8c  bltz → error path
```

### Интерпретация байтов:

Структура данных PIC (struct+offset):
- **struct+0x0**: Command/Status byte (записывается как результат lookup)
- **struct+0x8**: **RAW VALUE** — это основное значение (напряжение/ADC).
  В `PIC_BATTERY_MONITOR` проверяется: `< 0x191` (low battery), `< 0x21E` (normal range)
  - **0x191 = 401** decimal — порог низкого заряда
  - **0x21E = 542** decimal — порог нормального заряда
- **struct+0xC**: Тип запроса/состояние (1=read pending, другое=process)

### Маппинг байтов ответа `55 00 00 00 39 3e 01 e6`:

| Байт | Значение | Назначение |
|------|----------|-----------|
| 0 | 0x55 | PIC header/ACK byte (фиксированный маркер) |
| 1 | 0x00 | Status byte (0=OK) |
| 2 | 0x00 | Reserved/zero |
| 3 | 0x00 | Reserved/zero |
| 4 | 0x39 | **Raw ADC high byte** |
| 5 | 0x3E | **Raw ADC low byte** |
| 6 | 0x01 | **Charger status** (1=charger connected) |
| 7 | 0xE6 | **Checksum** |

Raw ADC = (0x39 << 8) | 0x3E = 0x393E = **14654** — но это ЕСЛИ 16-bit.
Однако, struct+8 загружается как 32-bit word, значит формат может быть другой.

**ВАЖНО**: Данные проходят через `PIC_READ_DATA` (0x4080DC) который делает
queue swap — это означает что данные МОГУТ быть переформатированы при извлечении.

---

## Q7: Вызывает ли PIC_BAT_READ_CMD I2C READ после WRITE?

**НЕТ!** Вот полный код PIC_BAT_READ_CMD (0x413138):

```c
int PIC_BAT_READ_CMD(int seconds) {
    if (seconds <= 0) goto error;

    int timeout_ms = seconds * 1000;     // $s2 = ((a0*128 - a0*4) + a0) * 8 = a0*1000
    MEMORY[0x80704D88] = 1;              // state = PENDING

    mutex_lock(0x80704C84);              // sub_52A614
    udelay(5000);                        // sub_249210(0x1388)

    // I2C WRITE: {0x2F, 0x00, 0x01} → PIC addr 0x2A
    char cmd[3] = {0x2F, 0x00, 0x01};
    sub_413F78(0x2A, cmd, 3);            // I2C WRITE ONLY!

    udelay(5000);
    mutex_unlock(0x80704C84);            // sub_52A264

    // Schedule timer for response read
    int saved = MEMORY[0x8159A6BC];
    int timer = sub_33AD8(timeout_ms);
    sub_48B38(4, saved, 0x80704DCC, timer);  // schedule_delayed_work

    return;
}
```

**Ответ I2C READ происходит ПОЗЖЕ**, в worker loop, который:
1. Ждёт 500 мс (delay loop)
2. Вызывает `sub_414200` (DMA/queue read)
3. Обрабатывает данные через state machine

PIC_BAT_READ2 (0x413094) — аналогично, только пишет `{0x2F, 0x00, 0x02}` и НЕ ставит таймер.

---

## Q8: Есть ли ОТДЕЛЬНАЯ команда чтения ответа?

**ДА!** Есть ДВЕ разные I2C транзакции:

### Транзакция 1: WRITE (команда)
```
sub_413F78(0x2A, {0x2F, 0x00, 0x01}, 3)
→ PIC_I2C_WRITE:
  SM0_DATA  = 0x2A        ; I2C адрес PIC (write mode)
  SM0_START = 3            ; 3 байта
  SM0_DATAOUT = first_byte ; данные
  SM0_STATUS = 0           ; WRITE mode
  ; Poll SM0_POLLSTA bit 0x02 для каждого байта
  ; udelay(1000) × 15 между байтами
```

### Транзакция 2: READ (ответ) — через worker thread
```
PIC_I2C_READ(buffer, count):
  SM0_START  = count - 1   ; кол-во байт - 1
  SM0_STATUS = 1           ; READ mode!
  ; Poll SM0_POLLSTA bit 0x04 для каждого байта
  ; udelay(10) после готовности
  ; Читаем SM0_DATAIN
  ; Сохраняем в buffer[index]
```

**КРИТИЧЕСКИ ВАЖНО**: SM0_DATA (0x908) НЕ перезаписывается перед READ!
Стоковое ядро оставляет 0x2A в SM0_DATA от предыдущего WRITE.
MT7621 SM0 автоматически использует addr+1 (0x55) для read mode,
что объясняет байт 0x55 в начале ответа!

---

## Полная картина: Data Path PIC → Kernel

```
1. PIC_BAT_READ_CMD(3)           ; Вызов из userspace/timer
   │
   ├─ MEMORY[0x80704D88] = 1     ; State = PENDING
   ├─ I2C WRITE {0x2F,0x00,0x01} ; "Подготовь данные батареи"
   └─ schedule_timer(3000ms)      ; Таймер на 3 сек

2. Worker Thread (каждые ~500мс):
   │
   ├─ state == 1?
   │  ├─ sub_413C40 → PIC_PARSE_BATTERY
   │  │   ├─ sub_414468 (prep)
   │  │   ├─ PIC_READ_DATA (queue swap)  ; Извлечь данные из HW FIFO
   │  │   ├─ if data != -1:
   │  │   │   ├─ CALIB_LOOKUP × 4       ; Калибровка
   │  │   │   ├─ Check 0x8159A6CC (charger?)
   │  │   │   ├─ CALIB_INTERP           ; Интерполяция
   │  │   │   └─ → sub_414CDC (dispatch)
   │  │   └─ if data == -1: return      ; Нет данных
   │  │
   │  ├─ if (pending_flag):
   │  │   └─ PIC_BATTERY_MONITOR        ; Обработка
   │  │       ├─ Check raw value ranges
   │  │       │   < 0x191 → LOW BATTERY (printk warning)
   │  │       │   < 0x21E → NORMAL (parse + display)
   │  │       │   == 1    → FULL/CHARGED
   │  │       ├─ CALIB_CALC2(raw) → calibrated_value
   │  │       └─ PIC_PARSE_BATTERY → result → sub_414CDC
   │  │
   │  └─ state = 2
   │
   ├─ 500ms delay (0x1F4 × udelay(1000))
   │
   └─ sub_414200 → check next state
```

---

## ПОЧЕМУ ДАННЫЕ СТАТИЧЕСКИЕ (55 00 00 00 39 3e 01 e6)?

### Гипотеза 1: Мы не ждём достаточно после WRITE
Стоковое ядро:
- Пишет `{0x2F, 0x00, 0x01}`
- Ждёт **5000 мкс** до и после I2C write
- Потом worker thread ждёт **500 мс** перед чтением
- Мы возможно читаем слишком рано

### Гипотеза 2: PIC не обновляет данные без правильной последовательности WAKE
Стоковое ядро при инициализации вызывает:
- `sub_4127DC` — hardware init (SM0 config)
- `sub_412878` — hardware init #2
- `PIC_CALIB_LOOP(3)` — отправляет калибровочные данные на PIC

Без калибровки PIC может возвращать фиксированные значения.

### Гипотеза 3: PIC_READ_DATA — это DMA/FIFO queue, не прямой I2C read
`PIC_READ_DATA` (0x4080DC) делает **swap указателей** в очереди:
```c
void PIC_READ_DATA(..., int *a4) {
    int temp = *a4;     // Сохраняем текущий
    *a4 = *v4;          // Берём из head
    *v4 = temp;         // Перемещаем в tail
    JUMPOUT(0x408DDC);  // → обработка данных
}
```
Это похоже на double-buffered DMA: один буфер заполняется через I2C (в фоне),
другой читается worker thread. I2C чтение может происходить через прерывание!

### Гипотеза 4: SM0 I2C READ адрес автоматически = 0x55
MT7621 SM0 при READ mode берёт адрес из SM0_DATA (0x2A = 0x54 >> 1)
и добавляет бит R/W = 1, получая 0x55 на шине.
Первый байт ответа (0x55) — это **адрес подтверждения от PIC**,
а не данные. Реальные данные начинаются с байта 1!

---

## РЕКОМЕНДАЦИИ ДЛЯ НАШЕГО КОДА

### 1. Правильная последовательность I2C

```c
// WRITE: отправить команду
SM0_DATA = 0x2A;           // PIC I2C address
SM0_DATAOUT = first_byte;  // Первый байт данных
SM0_START = byte_count;    // Количество байт
SM0_STATUS = 0;            // WRITE mode
// Poll POLLSTA bit 0x02 для каждого байта
// udelay(1000) × 15 между байтами (!)

// ПАУЗА 500мс (обязательно!)

// READ: прочитать ответ
// НЕ менять SM0_DATA! Оставить 0x2A
SM0_START = count - 1;     // count-1 (!)
SM0_STATUS = 1;            // READ mode
// Poll POLLSTA bit 0x04 (не 0x02!) для каждого байта
// udelay(10) после готовности бита
// Читать SM0_DATAIN
```

### 2. Ключевые отличия от нашего текущего кода:

| Параметр | Стоковое ядро | Наш код |
|----------|--------------|---------|
| SM0_START для READ | **count - 1** | count |
| POLLSTA бит для READ | **0x04 (bit 2)** | 0x02 (bit 1) |
| POLLSTA бит для WRITE | **0x02 (bit 1)** | ? |
| Задержка после poll | **udelay(10)** для READ, **udelay(1000)×15** для WRITE | ? |
| Пауза WRITE→READ | **500 мс** | слишком мало? |
| SM0_DATA для READ | **не менять** (оставить от WRITE) | перезаписываем? |

### 3. Калибровка при инициализации

Стоковое ядро вызывает `PIC_CALIB_LOOP(3)` при старте:
- Отправляет калибровочные таблицы на PIC
- Формат: big-endian 16-bit words, с заголовком 0x2E
- Два раунда калибровки с разными данными
- Без этого PIC может не обновлять данные

### 4. sub_41217C — SM0 I2C Init (инициализация тачскрина SX8650)

```
SM0_DATA = 0x48          ; SX8650 addr
SM0_START = 0            ; 0 байт
udelay(0x96)
SM0_DATAOUT = 0x83       ; SX8650 soft reset cmd
SM0_STATUS = 2           ; ???
udelay(0x96)
SM0_START = 0
udelay(0x96)
SM0_DATAOUT = 0x93       ; SX8650 config
SM0_STATUS = 2
udelay(0x96)
SM0_CTL1 = 0xFA          ; Регистр 0x900!
SM0_START = 0
SM0_START = 1
SM0_START = 1            ; повторно
SM0_STATUS = 1           ; read
; Читает SM0_DATAIN (0x914) дважды, собирает 12-bit значение
SM0_CTL1 (0x940) = 0x8064800E  ; SM0 конфигурация!
```

**ВАЖНО**: `SM0_CTL1 (0xBE000940) = 0x8064800E` — это конфигурация I2C!
Наш код может не инициализировать этот регистр правильно.

---

## Карта функций PIC battery

| Адрес | Имя | Описание |
|-------|-----|----------|
| 0x412400 | sub_412400 | Главная функция worker thread (init + main loop) |
| 0x4127DC | sub_4127DC | Hardware init #1 |
| 0x412878 | sub_412878 | Hardware init #2 |
| 0x41217C | sub_41217C | SM0 I2C init (SX8650 + SM0_CTL1 config) |
| 0x412288 | sub_412288 | SM0 command dispatch (0/1/2/3) |
| 0x412E78 | PIC_I2C_READ | I2C READ: SM0 byte-by-byte, poll bit 0x04 |
| 0x412F78 | PIC_I2C_WRITE | I2C WRITE: SM0 byte-by-byte, poll bit 0x02 |
| 0x413094 | PIC_BAT_READ2 | Отправка {0x2F,0x00,0x02} без таймера |
| 0x413138 | PIC_BAT_READ_CMD | Отправка {0x2F,0x00,0x01} + таймер |
| 0x4130F8 | sub_4130F8 | State handler: call sub_414094 + PIC_BATTERY_MONITOR if pending |
| 0x413288 | PIC_CALIB_LOOP | Калибровка PIC (два раунда write 0x2A) |
| 0x413C40 | sub_413C40 | Wrapper: PIC_PARSE_BATTERY(struct) |
| 0x413CA0 | PIC_PARSE_BATTERY | Главный парсер: PIC_READ_DATA + CALIB chain |
| 0x413D94 | sub_413D94 | Парсер ответа батареи (state 3) |
| 0x413E78 | PIC_BATTERY_MONITOR | Анализ: raw value → ranges → CALIB_CALC2 → PIC_PARSE_BATTERY |
| 0x413F78 | sub_413F78 | Высокоуровневый I2C write wrapper (сравнение строк + dispatch) |
| 0x414094 | sub_414094 | Cleanup → JUMPOUT к 414DF8 |
| 0x414138 | sub_414138 | Многоуровневый state handler с CALIB_INTERP |
| 0x414200 | sub_414200 | CALIB_LOOKUP + sub_407C14 → 414DF8 |
| 0x414468 | sub_414468 | Prep function: CALIB_LOOKUP2 + CALIB_LOOKUP |
| 0x414CDC | sub_414CDC | Dispatch: switch(s1) → battery status actions |
| 0x414DF8 | sub_414DF0 | "Перезапуск": memset + D1E74 (новый цикл потока) |
| 0x415ABC | sub_415ABC | Battery charger status dispatch (GPIO 31?) |
| 0x4080DC | PIC_READ_DATA | Queue swap (double-buffer) |

---

## Глобальные переменные

| Адрес | Назначение |
|-------|-----------|
| 0x80704D88 | State machine state (1=pending, 2=processing) |
| 0x80704C84 | Mutex для SM0 I2C bus |
| 0x80704C9C | Device enable flag |
| 0x8159A6A0 | Battery read pending flag |
| 0x8159A6BC | Timer/callback value |
| 0x8159A6CC | Charger connected flag |
| 0x8159A6D0 | Battery status (1=full) |
| 0x8159A6DC | Battery monitor state |
| 0x8159A6F0 | Battery raw value (cached) |
| 0x8159A6F4 | Battery processed value |
| 0x8159A700 | Battery display update flag |
| 0x8159A888 | Charger GPIO config |

---

## СЛЕДУЮЩИЕ ШАГИ

1. **Исправить SM0_START для READ**: использовать `count - 1`, не `count`
2. **Исправить POLLSTA бит для READ**: проверять **0x04**, не 0x02
3. **Добавить задержку 500мс** между WRITE и READ
4. **Не менять SM0_DATA** перед READ (оставить 0x2A от WRITE)
5. **Проверить SM0_CTL1 (0x940)**: стоковое ядро пишет **0x8064800E** (наш текущий 0x90644042 — ДРУГОЕ значение!)
   - `0x8064800E` в бинарном: 1000 0000 0110 0100 1000 0000 0000 1110
   - `0x90644042` в бинарном: 1001 0000 0110 0100 0100 0000 0100 0010
   - Разница: биты 28, 15, 6, 3, 1 — это может влиять на clock stretching, timing, ACK handling!
6. **Проверить SM0_CTL0 (0x900)**: стоковое ядро пишет **0xFA** при init (sub_41217C)
7. **Реализовать калибровку**: отправить калибровочные данные при инициализации
8. **Добавить udelay(10)** после POLLSTA bit ready перед чтением DATAIN
9. **Попробовать SM0_CTL1 = 0x8064800E** вместо 0x90644042 — это может быть ключом к живым данным!

---

## СЕАНС #2: Детальный анализ 9 задач (2026-03-19)

### Задача 1: sub_414200 (worker loop "read PIC response")

Декомпиляция IDA:
```c
void sub_414200() {
  v1 = CALIB_LOOKUP(v0, MEMORY[0x8067DDD8]);
  sub_407C14(v1, MEMORY[0x8067DE04]);
  JUMPOUT(0x414DF8);  // перезапуск цикла
}
```
**Вывод**: Это НЕ I2C read. Это пост-обработка данных через калибровочную lookup таблицу.
Вызывается в main loop после 500мс задержки. JUMPOUT(0x414DF8) ведёт к "перезапуску"
(memset + создание нового цикла потока).

### Задача 2: sub_413C40 (called when pending==1)

Дизассемблер:
```mips
sub_413C40:
  413c40  lui     $v0, 0x8139
  413c48  lw      $a0, 0x81391FBC    ; загрузить калибр. таблицу
  413c4c  li      $a1, 0x81391FAC    ; загрузить конфиг
  413c50  addiu   $a2, $sp, arg_10   ; локальный буфер
  413c54  jal     PIC_PARSE_BATTERY  ; вызвать парсер!
  413c58  li      $a3, 0x334         ; размер структуры (820 байт!)
  413c5c  lw      $ra, ...
  413c60  move    $v0, $zero
  413c6c  jr      $ra
```
**Вывод**: Это обёртка которая вызывает PIC_PARSE_BATTERY с калибровочными
таблицами из ROM. Аргументы:
- $a0 = таблица @ 0x81391FBC
- $a1 = конфиг @ 0x81391FAC
- $a2 = локальный буфер (на стеке)
- $a3 = 0x334 (820) — размер данных

### Задача 3: sub_414094 (called in worker loop)

```c
void sub_414094() {
  JUMPOUT(0x414DF8);  // просто прыжок к перезапуску
}
```
**Вывод**: Тривиальная функция — просто cleanup/переход к следующей итерации.

### Задача 4: sub_413D94 (parse battery data)

Дизассемблер:
```mips
sub_413D94:
  413d94  lw      $a1, -0x2234($s2)  ; калибр. данные
  413d98  lw      $a0, arg_30($sp)   ; raw value
  413d9c  jal     sub_408C8C         ; lookup/compare
  413da4  bltz    $v0, loc_413FF0    ; если < 0 → другой путь
  413dac  lw      $a1, 0x8067DDD0    ; другая таблица
  413db4  jal     CALIB_INTERP       ; интерполяция
  413dbc  bgez    $v0, loc_413FF0    ; если >= 0 → другой путь
  413dc4  lw      $a1, -0x2234($s2)
  413dc8  jal     sub_407D4C         ; финальная конвертация
```
**Вывод**: Парсер для state==3. Берёт raw value, пропускает через цепочку
CALIB_INTERP/sub_408C8C/sub_407D4C для конвертации сырого ADC в процент заряда.
Использует несколько калибровочных таблиц из ROM для разных диапазонов напряжения.

### Задача 5: FULL disasm PIC_I2C_READ read loop

**ПОДТВЕРЖДЕНО из raw bytes** (IDA показывала .byte, ручная декодировка):
```mips
; Read loop body (0x412EDC - 0x412F14)
; Входные: $s0=0xBE00(base), $s4=total_bytes, $s6=dest_buf
; $s2=byte_counter(0), $s3=index(0), $v0=timeout(100000)

412edc: beq  $v0, $zero, +0x2C     ; timeout? → skip byte
412ee0: nop
412ee4: lw   $v1, 0x918($s0)       ; SM0_POLLSTA
412ee8: andi $v1, $v1, 0x04        ; bit 2 = DATAIN ready
412eec: beq  $v1, $zero, 0x412EDC  ; not ready → retry
412ef0: addiu $v0, -1              ; timeout--
; Data ready:
412ef4: jal  udelay                ; udelay(10)
412ef8: li   $a0, 10
412efc: lw   $v0, 0x914($s0)       ; READ SM0_DATAIN !!!
412f00: addu $s3, $s6, $s3         ; addr = buf + index
412f04: sb   $v0, 0($s3)           ; buf[index] = byte
412f08: addiu $s2, 1               ; counter++
412f0c: bne  $s2, $s4, loop_start  ; if counter != total → next
412f10: move $s3, $s2              ; index = counter
```

Считывает **byte_count байт**, по одному, с poll SM0_POLLSTA bit 2 и timeout.
SM0_DATAIN (0x914) содержит по одному байту за раз.

### Задача 6: Кто вызывает PIC_I2C_READ?

XRefs анализ:
- `idautils.XrefsTo(0x412E78)` → **0 результатов**
- `idautils.XrefsTo(0x413EE4)` → 2 результата:
  - `0x413ee0 -> 0x413ee4 type=21` (fallthrough от предыдущей инструкции)
  - `0x412ed4 -> 0x413ee4 type=19` (jump из PIC_I2C_READ init)

**НО!** Адрес 0x413EE4 - это НЕ read loop! Это PIC_BATTERY_MONITOR (часть worker).
PIC_I2C_READ прыгает на 0x413EE4 через `j loc_413EE4`, что НЕПРАВИЛЬНО!
Реальный read loop на 0x412EDC (IDA не разобрала).

PIC_I2C_READ (0x412E78) является ЧАСТЬЮ огромной монолитной функции sub_412400.
Она вызывается через внутренние переходы state machine, НЕ через JAL.

Адрес 0x412E74 (перед PIC_I2C_READ): `.byte 1` — это хвост предыдущей инструкции.

### Задача 7: sub_412B94 (called from sub_411CA8 init)

```c
void sub_412B94(int a1, ..., int a6) {
  sub_79644(0);  // вызов с аргументом 0
  jr $ra;        // return
}
```
**Вывод**: Простой вызов sub_79644(0) — скорее всего GPIO reset или hardware init.
Вызывается из sub_411CA8 (GPIO interrupt handler) при обработке PIC GPIO событий.

### Задача 8: Формат ответа `55 00 00 00 39 3e 01 e6`

**0x55 = PIC I2C read address (0x2A<<1|1)**
- MT7621 SM0 при READ автоматически берёт addr из SM0_DATA (установлен как 0x2A
  при WRITE) и ORит с R/W bit. Получается 0x55 = read addr.
- Первый байт, который мы читаем из SM0_DATAIN (0x914) = 0x55. Это НЕ данные PIC,
  а аппаратный echo slave address.

**0x393E**: поиск по константе в коде:
- `py_eval` нашёл: `0x4113a0 .byte 0x55` — единственное вхождение 0x55 в PIC area
- 0x393E / 0x3E39 НЕ найдены в коде вообще
- Значит 0x393E — это РЕАЛЬНЫЕ данные от PIC, не хардкод

**Структура ответа (УТОЧНЁННАЯ)**:
| Байт | Hex | Описание |
|------|-----|----------|
| 0 | 0x55 | SM0 echo slave read address (аппаратный, НЕ данные PIC) |
| 1-3 | 00 00 00 | Заголовок PIC ответа / status |
| 4 | 0x39 | Raw ADC high |
| 5 | 0x3E | Raw ADC low |
| 6 | 0x01 | Charger flag (1=подключен) |
| 7 | 0xE6 | Checksum |

**ПРОБЛЕМА**: Если байт 0 — echo addr, то данные начинаются с байта 1.
Но наш код читает все 8 байт и первый (0x55) попадает в буфер как "данные".
Стоковое ядро скорее всего **пропускает первый байт** или SM0 его не возвращает.

### Задача 9: Полная трассировка от bat_read WRITE до READ

#### PIC_BAT_READ_CMD (0x413138) — полная декомпиляция:
```c
void PIC_BAT_READ_CMD(int seconds) {
    if (seconds <= 0) JUMPOUT(0x414164);  // error path

    int timeout = seconds * 1000;         // вычисление: ((s*128-s*4)+s)*8 = s*1000
    MEMORY[0x80704D88] = 1;               // STATE = PENDING_READ

    mutex_lock(0x80704C84);               // sub_52A614
    udelay(5000);                         // sub_249210(0x1388)

    // Формируем команду: {0x2F, 0x00, 0x01}
    char cmd[3];
    cmd[0] = 0x2F;                        // команда "battery read"
    cmd[1] = 0x00;                        // zero
    cmd[2] = 0x01;                        // subcommand = 1

    sub_413F78(0x2A, cmd, 3);             // I2C WRITE на PIC addr 0x2A

    udelay(5000);                         // ещё 5мс пауза
    mutex_unlock(0x80704C84);             // sub_52A264

    // Планируем отложенную работу
    int saved = MEMORY[0x8159A6BC];       // timer period
    sub_33AD8(timeout);                   // msleep/schedule_timeout
    sub_48B38(4, saved, 0x80704DCC, v0);  // schedule_delayed_work
}
```

#### sub_48B38 — schedule work:
```c
void sub_48B38(int a1, int a2, int a3) {
    // Это kernel schedule_delayed_work или mod_timer
    // a1=4, a2=timer_value, a3=0x80704DCC (work_struct address)
    // Disable/enable interrupts (_di/_ei)
    // Initialize work_struct fields
    // Clear 64 entries in array (zeroing callback slots)
    JUMPOUT(0x49A28);  // finalize scheduling
}
```

#### sub_33AD8 — sleep:
```c
void sub_33AD8() {
    ;  // пустая функция (оптимизирована компилятором?)
}
```
Может быть заглушка или inline в вызывающем коде.

#### Worker thread обработка (0x413AF4):
```mips
; Когда state == 1 (PENDING_READ):
413AF4: jal  sub_413C40          ; -> PIC_PARSE_BATTERY(calibration tables)
413AF8: nop
413AFC: lui  $v0, 0x815A
413B00: lw   $v1, 0x8159A6A0    ; load pending_flag
413B04: li   $a0, 2
413B08: beqz $v1, loc_413544    ; if !pending -> back to normal loop
413B0C: sw   $a0, 0x4D88($s1)   ; state = 2
413B10: jal  PIC_BATTERY_MONITOR ; обработать данные батареи!
413B14: sw   $zero, 0x8159A6A0  ; clear pending_flag
413B18: j    loc_414544          ; post-processing
```

#### КЛЮЧЕВОЙ ВЫВОД:
sub_413C40 вызывает PIC_PARSE_BATTERY, который вызывает PIC_READ_DATA (queue swap).
Это означает что **I2C READ уже произошёл ДО этого момента** через другой путь.

Возможно I2C READ происходит в нормальном цикле worker (state != 1):
1. Normal loop: sub_414094 → delay 500ms → sub_414200
2. sub_414200 включает PIC_I2C_READ через внутренние переходы
3. Данные помещаются в queue
4. Когда bat_read_cmd ставит state=1, worker берёт данные ИЗ ОЧЕРЕДИ

Или PIC_I2C_READ вызывается внутри PIC_BATTERY_MONITOR, так как
0x413B10 (jal PIC_BATTERY_MONITOR) прыгает на 0x413EE4, и цепочка
может включить fall-through к PIC_I2C_READ через state machine.

#### sub_414CDC — dispatch после PIC_BATTERY_MONITOR:
```c
int sub_414CDC(int a1, unsigned int cmd, int a3, int a4) {
    switch (cmd) {
        case 10: sub_415ABC(-2140667904); return 0;  // charger event
        case 11: sub_415BDC(); return 0;              // battery update
        case 12: MEMORY[0x8159A6F4]=0; MEMORY[0x8159A700]=1; return 0; // reset
        case 13: MEMORY[0x8159A700]=0; return 0;      // clear display
        case 14: MEMORY[0x8159A6F0]=a3; printk(...); return 0; // set raw value
        case 15: return 0;                             // noop
        case 160: sub_415C4C(-2140667904); return 0;  // special event
        default: printk(...); return 0;                // unknown command
    }
}
```
Это **dispatcher** — маршрутизирует результат PIC_BATTERY_MONITOR к нужному
обработчику. cmd=11 (0xB) обновляет battery display. cmd=14 (0xE) сохраняет
raw ADC значение в 0x8159A6F0.

---

## ИТОГОВАЯ ГИПОТЕЗА: Почему данные не меняются

1. **Наш код правильно ПИШЕТ команду** {0x2F, 0x00, 0x01} — PIC ACK'ает
2. **Наш код правильно ЧИТАЕТ ответ** — получаем 8 байт
3. **НО**: Стоковое ядро при инициализации вызывает PIC_CALIB_LOOP(3)
   который отправляет **калибровочные таблицы** на PIC. Без этих таблиц
   PIC может НЕ ЗАПУСКАТЬ ADC преобразование и возвращать кэшированные данные.
4. **Альтернатива**: Стоковое ядро отправляет cmd {0x2F,0x00,0x01} и потом
   ЖДЁТ 500мс перед READ. Мы возможно читаем слишком быстро (до того как
   PIC выполнил ADC).
5. **Третья возможность**: 0x39 0x3E — это РЕАЛЬНОЕ напряжение батареи,
   просто оно стабильно потому что батарея заряжается (charger flag=0x01).
