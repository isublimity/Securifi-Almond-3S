# PIC16LF1509 Functions (Stock Kernel 3.10.14 - IDA Decompilation)

Результат декомпиляции и дизассемблирования функций работы с PIC16LF1509
из стокового ядра Securifi Almond 3S (kernel 3.10.14, MIPS 1004Kc).

PIC-контроллер отвечает за управление питанием, мониторинг батареи, буззер.
Связь с MT7621 по шине I2C (SM0, auto mode) на адресе **0x2A** (42 decimal).

---

## Оглавление

1. [PIC_I2C_READ](#pic_i2c_read) - 0x412E78
2. [PIC_I2C_WRITE](#pic_i2c_write) - 0x412F78
3. [PIC_BAT_READ2](#pic_bat_read2) - 0x413094
4. [PIC_BAT_READ_CMD](#pic_bat_read_cmd) - 0x413138
5. [PIC_CALIBRATE](#pic_calibrate) - 0x413200
6. [PIC_CALIB_LOOP](#pic_calib_loop) - 0x413288
7. [PIC_BUZZER](#pic_buzzer) - 0x4133F0
8. [PIC_IOCTL](#pic_ioctl) - 0x413468
9. [PIC_PARSE_BATTERY](#pic_parse_battery) - 0x413CA0
10. [PIC_BATTERY_MONITOR](#pic_battery_monitor) - 0x413DCC
11. [PIC_READ_DATA](#pic_read_data) - 0x4080DC
12. [PIC_GET_READINGS](#pic_get_readings) - 0x40A210
13. [sub_413F78 (I2C высокоуровневая обёртка)](#sub_413f78) - 0x413F78
14. [Анализ протокола](#анализ-протокола)

---

## PIC_I2C_READ

**Адрес**: 0x412E78 | **Размер**: 0x100 (256 байт) | **Декомпиляция не удалась, дизассемблер**

Функция чтения данных по I2C (SM0 auto mode). Устанавливает режим чтения через SM0 регистры.

```mips
PIC_I2C_READ (ROM @ 0x412e78):
412e78  addiu   $sp, -0x30
412e7c  lui     $v0, 0xBE00
412e80  addiu   $v1, $a1, -1          ; длина-1
412ea4  sw      $v1, 0xBE000920       ; SM0_BYTECNT = len-1
412ea8  li      $v1, 1
412eb4  sw      $v1, 0xBE00091C       ; SM0_AUTOMODE = 1 (read)
412eb8  beqz    $a1, loc_412F14       ; если len==0, сразу wait
412ec0  lui     $s5, 1
412ecc  li      $s5, 0x186A0          ; timeout = 100000
412ed0  lui     $s0, 0xBE00
412ed4  j       loc_413EE4            ; -> цикл чтения байтов

; --- Ожидание завершения (len==0) ---
412f14  lw      $v0, 0xBE000918       ; SM0_STATUS
412f1c  andi    $v0, 1                ; бит BUSY
412f20  beqz    $v0, loc_412F54       ; если не busy -> выход
412f28  li      $v0, 0x186A0          ; timeout
; --- Цикл поллинга ---
412f40  lw      $v1, 0x918($a0)       ; SM0_STATUS
412f44  andi    $v1, 1                ; BUSY?
412f48  bnez    $v1, loc_412F38       ; если busy -> продолжить
412f4c  addiu   $v0, -1              ; timeout--

; --- Выход ---
412f54  lw      $s6, ...              ; восстановление регистров
412f70  jr      $ra
412f74  addiu   $sp, 0x30

; --- Ветка timeout/calibration (фрагмент) ---
413f40  lw      $a1, 0x8067DD8C       ; глобальная константа
413f48  jal     sub_408C6C            ; сравнение (fixed point?)
; ... переход к CALIB_INTERP если нужна интерполяция ...
```

**Ключевые регистры**:
- `0xBE000920` = SM0_BYTECNT (количество байт -1)
- `0xBE00091C` = SM0_AUTOMODE (1=read, 0=write)
- `0xBE000918` = SM0_STATUS (бит 0 = BUSY)

---

## PIC_I2C_WRITE

**Адрес**: 0x412F78 | **Размер**: 0x11C (284 байт) | **Декомпиляция + Дизассемблер**

Функция записи данных по I2C. Устанавливает адрес устройства 0x2A, данные, и запускает передачу.

### Декомпиляция:
```c
int __fastcall PIC_I2C_WRITE(unsigned __int8 a1, int a2, int a3)
{
  int result;

  MEMORY[0xBE000908] = 42;    // SM0_DATA = 0x2A (PIC I2C адрес)
  MEMORY[0xBE000920] = a3;    // SM0_BYTECNT = длина
  MEMORY[0xBE000910] = a1;    // SM0_DATAOUT = первый байт данных
  MEMORY[0xBE00091C] = 0;     // SM0_AUTOMODE = 0 (write)
  if ( a3 )                   // если есть данные для передачи
    JUMPOUT(0x413FEC);        // -> цикл записи байтов
  result = MEMORY[0xBE000918] & 1; // SM0_STATUS & BUSY
  if ( (MEMORY[0xBE000918] & 1) != 0 )
    JUMPOUT(0x41405C);        // -> ожидание завершения
  return result;
}
```

### Дизассемблер (ключевые строки):
```mips
412f84  li      $v1, 0x2A             ; PIC I2C address = 42 decimal
412fac  sw      $v1, 0xBE000908       ; SM0_DATA = slave addr
412fb4  sw      $a2, 0xBE000920       ; SM0_BYTECNT = len
412fb8  sw      $a0, 0xBE000910       ; SM0_DATAOUT = data byte
412fbc  sw      $zero, 0xBE00091C     ; SM0_AUTOMODE = 0 (write)
412fc0  beqz    $a2, loc_413030       ; если len==0, ждём завершения
412fd4  li      $s5, 0x186A0          ; timeout = 100000 итераций
```

**Ключевые регистры**:
- `0xBE000908` = SM0_DATA (адрес slave = 0x2A)
- `0xBE000910` = SM0_DATAOUT (данные для записи)
- `0xBE000920` = SM0_BYTECNT
- `0xBE00091C` = SM0_AUTOMODE (0=write)

---

## PIC_BAT_READ2

**Адрес**: 0x413094 | **Размер**: 0x64 (100 байт) | **Декомпиляция + Дизассемблер**

Чтение батареи с таймаутом 2 байта. Отправляет команду 0x2F (BAT_READ).

### Декомпиляция:
```c
int PIC_BAT_READ2()
{
  spin_lock(0x80704C84);        // захват мьютекса
  udelay(5000);                 // задержка 5мс
  sub_413F78();                 // I2C транзакция
  udelay(5000);                 // задержка 5мс
  return spin_unlock(0x80704C84);
}
```

### Дизассемблер (полный):
```mips
413094  addiu   $sp, -0x20
41309c  lui     $s0, 0x8070
4130a4  jal     sub_52A614           ; spin_lock(0x80704C84)
4130ac  jal     sub_249210           ; udelay(5000)
4130b4  li      $v0, 0x2F            ; команда BAT_READ = 0x2F
4130b8  addiu   $a1, $sp, var_8      ; буфер на стеке
4130bc  li      $a2, 3               ; длина = 3 байта
4130c0  sb      $v0, var_8($sp)      ; buf[0] = 0x2F
4130c4  li      $a0, 0x2A            ; I2C addr = 0x2A
4130c8  li      $v0, 2
4130cc  sb      $v0, var_8+2($sp)    ; buf[2] = 2 (кол-во байт для чтения)
4130d0  jal     sub_413F78           ; I2C wrapper (write cmd + read response)
4130d4  sb      $zero, var_8+1($sp)  ; buf[1] = 0
4130d8  jal     sub_249210           ; udelay(5000)
4130e0  jal     sub_52A264           ; spin_unlock(0x80704C84)
4130f0  jr      $ra
```

**Протокол**: Отправка `[0x2F, 0x00, 0x02]` на адрес 0x2A — команда чтения батареи с запросом 2 байт ответа.

---

## PIC_BAT_READ_CMD

**Адрес**: 0x413138 | **Размер**: 0xC8 (200 байт) | **Декомпиляция + Дизассемблер**

Чтение батареи с настраиваемым таймером. Устанавливает флаг `battery_read_pending = 1`.

### Декомпиляция:
```c
int __fastcall PIC_BAT_READ_CMD(int timeout_sec)
{
  int timeout_ms;
  int jiffies_val;

  if ( timeout_sec <= 0 )
    JUMPOUT(0x414164);          // ошибка: -1000 (0xFFFFFC18)

  timeout_ms = 1000 * timeout_sec; // перевод в миллисекунды
  MEMORY[0x80704D88] = 1;      // battery_read_pending = 1
  spin_lock(0x80704C84);
  udelay(5000);
  sub_413F78();                 // I2C: отправка BAT_READ cmd
  udelay(5000);
  spin_unlock(0x80704C84);
  jiffies_val = MEMORY[0x8159A6BC]; // текущие jiffies
  return mod_timer(4, jiffies_val, 0x80704DCC, msecs_to_jiffies(timeout_ms));
}
```

### Дизассемблер (ключевые строки):
```mips
413148  blez    $a0, loc_4131F8      ; if (sec <= 0) -> error
413150  sll     $v0, $a0, 2          ; v0 = sec * 4
413154  sll     $s2, $a0, 7          ; s2 = sec * 128
413158  subu    $s2, $v0             ; s2 = sec * 124
41315c  addu    $s2, $a0             ; s2 = sec * 125
413160  sll     $s2, 3               ; s2 = sec * 1000 (= timeout_ms)
413178  sw      $s1, 0x80704D88      ; battery_read_pending = 1
41318c  li      $v0, 0x2F            ; BAT_READ cmd
413190  li      $a0, 0x2A            ; I2C addr
413198  sb      $s1, var_10+2($sp)   ; buf[2] = 1
4131b8  lw      $a1, 0x8159A6BC      ; jiffies
4131c0  jal     sub_33AD8            ; msecs_to_jiffies(timeout_ms)
4131d8  jal     sub_48B38            ; mod_timer
```

**Протокол**: То же что BAT_READ2, но с таймером повторного чтения и флагом `0x80704D88`.

**Вычисление таймаута**: `sec * ((128 - 4 + 1) * 8) = sec * 1000` мс.

---

## PIC_CALIBRATE

**Адрес**: 0x413200 | **Размер**: 0x88 (136 байт) | **Декомпиляция + Дизассемблер**

Запуск калибровки PIC. Отправляет команду 0x33 (WAKE/INIT) с параметрами из структуры калибровки.

### Декомпиляция:
```c
void __fastcall PIC_CALIBRATE(int calib_struct)
{
  int num_entries;
  _BYTE buf[104];

  memset(buf, 0, 100);
  spin_lock(0x80704C84);
  udelay(5000);

  num_entries = *(char *)(calib_struct + 4);    // кол-во записей
  buf[1] = *(char *)(calib_struct + 4) >> 7;   // старший бит (знак/флаг)
  buf[0] = 0x33;                                // команда WAKE = 0x33
  buf[2] = num_entries;                         // кол-во записей
  sub_413F78(/* write cmd 0x33 */);

  buf[0] = 0x2D;                                // команда '-' = 0x2D (Table1)
  if ( num_entries <= 0 )
  {
    udelay(5000);
    sub_413F78(/* write 0x2D */);               // отправка пустой таблицы
    buf[0] = 0x2E;                              // команда '.' = 0x2E (Table2)
    JUMPOUT(0x414300);                          // -> отправка Table2 и выход
  }
  PIC_CALIB_LOOP(...);                          // -> запись таблиц калибровки
}
```

### Дизассемблер (ключевые строки):
```mips
413234  jal     sub_249620           ; memset(buf, 0, 100)
41323c  jal     sub_52A614           ; spin_lock(0x80704C84)
413244  jal     sub_249210           ; udelay(5000)
41324c  lb      $s2, 4($s0)          ; num_entries = calib_struct->entries_count
413250  li      $v1, 0x33            ; cmd = WAKE (0x33)
413254  sra     $v0, $s2, 8          ; high_byte = num_entries >> 8
413258  li      $a0, 0x2A            ; I2C addr
41325c  addiu   $a1, $sp, var_68     ; буфер
413260  li      $a2, 3               ; длина 3 байта
413264  sb      $v0, var_67($sp)     ; buf[1] = high_byte
413268  sb      $v1, var_68($sp)     ; buf[0] = 0x33
41326c  jal     sub_413F78           ; I2C write [0x33, hi, lo]
413270  sb      $s2, var_66($sp)     ; buf[2] = num_entries
413274  li      $v0, 0x2D            ; cmd = Table1 (0x2D = '-')
413278  blez    $s2, loc_413390      ; if entries <= 0, skip loop
41327c  sb      $v0, var_68($sp)     ; buf[0] = 0x2D
```

**Протокол калибровки**:
1. Отправка `[0x33, hi_byte, count]` — команда WAKE/начало калибровки
2. Если есть записи → PIC_CALIB_LOOP
3. Если нет записей → отправка `[0x2D]` (пустая Table1), затем `[0x2E]` (пустая Table2)

---

## PIC_CALIB_LOOP

**Адрес**: 0x413288 | **Размер**: 0x108 (264 байт) | **Декомпиляция + Дизассемблер**

Цикл записи калибровочных таблиц в PIC. Выполняет **byte-swap** (big-endian в little-endian).

### Декомпиляция (очищенная):
```c
void PIC_CALIB_LOOP(/* ... */)
{
  // --- Цикл 1: byte-swap Table1 из int[] в byte[] ---
  end_ptr = buf + 2 * count;
  dst = buf;
  src = table1_ptr;
  do {
    val = *src;
    *dst = BYTE1(val);     // старший байт (byte-swap!)
    dst[1] = val;          // младший байт
    dst += 2;
    src++;
  } while (dst != end_ptr);

  total_len = 2 * count;
  udelay(5000);
  sub_413F78(0x2A, &cmd_buf, total_len + 1); // I2C write Table1 (cmd=0x2D)

  // --- Цикл 2: byte-swap Table2 из calib_struct+0x1A4 ---
  cmd_buf[0] = 0x2E;      // команда Table2
  src2 = calib_struct + 0x1A4;
  do {
    val = *src2;
    *buf2 = BYTE1(val);   // byte-swap!
    buf2[1] = val;
    buf2 += 2;
    src2++;
  } while (buf2 != end_ptr);

  udelay(5000);
  sub_413F78(0x2A, &cmd_buf, total_len + 1); // I2C write Table2 (cmd=0x2E)
  udelay(5000);
  spin_unlock(0x80704C84);

  // Сохранение калибровочных данных в глобальные массивы
  global_struct->entries_count = calib_struct->entries_count;
  global_struct->field_10 = calib_struct->field_10;
  memcpy(0x8159A044, table1_ptr, 400);  // Table1 -> глобальный массив
  memcpy(0x8159A1D4, calib_struct + 0x1A4, 400); // Table2 -> глобальный массив
}
```

### Дизассемблер (ключевые строки):
```mips
413288  sll     $s4, $s2, 1          ; end_offset = count * 2
413290  addu    $s4, $s3, $s4        ; end_ptr = buf + end_offset
; --- Цикл byte-swap Table1 ---
413298  lw      $a0, 0($v1)          ; val = *src (32-bit int)
41329c  sra     $a1, $a0, 8          ; hi_byte = val >> 8
4132a0  sb      $a1, 0($v0)          ; *dst = hi_byte
4132a4  sb      $a0, 1($v0)          ; *(dst+1) = lo_byte
4132a8  addiu   $v0, 2               ; dst += 2
4132ac  bne     $v0, $s4             ; while (dst != end)
4132b0  addiu   $v1, 4               ; src += 4

4132b4  sll     $s2, 1               ; total = count * 2
4132c0  addiu   $s6, $s2, 1          ; len = total + 1 (вкл. cmd byte)
4132c4  li      $a0, 0x2A            ; I2C addr
4132cc  jal     sub_413F78           ; I2C write Table1
4132d0  move    $a2, $s6             ; len

4132d8  li      $v0, 0x2E            ; cmd = Table2 (0x2E = '.')
4132dc  sb      $v0, arg_10($sp)     ; buf[0] = 0x2E

; --- Цикл byte-swap Table2 ---
4132e4  lw      $v1, 0($v0)          ; val = *(calib+0x1A4+i)
4132e8  sra     $a0, $v1, 8          ; hi_byte
4132ec  sb      $a0, 0($s3)          ; byte-swap
4132f0  sb      $v1, 1($s3)
4132f4  addiu   $s3, 2
4132f8  bne     $s3, $s4             ; while (dst != end)

413310  jal     sub_413F78           ; I2C write Table2
413320  jal     sub_52A264           ; spin_unlock

; --- Сохранение глобальных данных ---
413330  li      $v0, 0x8159A030      ; глобальная структура
413348  sb      $a3, 4($v0)          ; entries_count
413350  sw      $v1, 0x10($v0)       ; field_10
413340  li      $a0, 0x8159A044      ; Table1 глобальный буфер
413344  li      $a2, 0x190           ; 400 байт
41334c  jal     sub_2492E0           ; memcpy(0x8159A044, table1, 400)
41335c  li      $a0, 0x8159A1D4      ; Table2 глобальный буфер
413360  jal     sub_2492E0           ; memcpy(0x8159A1D4, table2, 400)
413364  li      $a2, 0x190           ; 400 байт
```

**КРИТИЧЕСКИ ВАЖНО: Byte-swap**:
- Каждый 32-bit int из таблицы калибровки конвертируется в 2 байта: `[hi_byte, lo_byte]`
- `hi_byte = value >> 8` (второй байт 32-битного значения)
- `lo_byte = value & 0xFF` (младший байт)
- Это big-endian порядок для 16-bit значений из 32-bit массива

**Глобальные буферы**:
- `0x8159A030` — структура калибровки (entries_count по смещению +4, field_10 по +16)
- `0x8159A044` — Table1 (400 байт = 100 записей по 4 байта)
- `0x8159A1D4` — Table2 (400 байт = 100 записей по 4 байта)

---

## PIC_BUZZER

**Адрес**: 0x4133F0 | **Размер**: 0x60 (96 байт) | **Декомпиляция + Дизассемблер**

Управление буззером PIC. Отправляет команду 0x34 (BUZZER) с параметром режима.

### Декомпиляция:
```c
void __fastcall PIC_BUZZER(int mode)
{
  char buzzer_val;

  if (mode == 1) return;  // режим 1 = noop
  if (mode == 2) return;  // режим 2 = noop (другой путь)

  buzzer_val = 3;          // default
  if (mode != 3)
    buzzer_val = 0;        // если не 3, то 0

  // Формируем пакет: [0x34, 0x00, buzzer_val]
  buf[0] = 0x34;           // команда BUZZER
  buf[1] = 0x00;
  buf[2] = buzzer_val;
  sub_413F78(0x2A, buf, 3); // I2C write

  MEMORY[0x8159A035] = mode; // сохранение текущего режима
  udelay(5000);
  spin_unlock(0x80704C84);
}
```

### Дизассемблер (ключевые строки):
```mips
4133f0  beq     $s0, $v0, loc_413458  ; if mode==1 -> exit
4133f8  beq     $s0, $v0, loc_413460  ; if mode==2 -> exit
413400  xori    $v1, $s0, 3           ; v1 = (mode != 3)
413404  movn    $v0, $zero, $v1       ; if (mode!=3) val=0, else val=3
413414  li      $v0, 0x34             ; cmd = BUZZER (0x34)
413418  li      $a0, 0x2A             ; I2C addr
41341c  sb      $v1, arg_12($sp)      ; buf[2] = buzzer_val
413420  sb      $v0, arg_10($sp)      ; buf[0] = 0x34
413424  jal     sub_413F78            ; I2C write
413428  sb      $zero, arg_11($sp)    ; buf[1] = 0x00
413438  sb      $s0, 0x8159A035       ; сохранение режима буззера
```

**Протокол**: `[0x34, 0x00, val]` — команда буззера. val=3 для режима 3, val=0 для остальных.

---

## PIC_IOCTL

**Адрес**: 0x413468 | **Размер**: 0x3C (60 байт) | **Декомпиляция + Дизассемблер**

Диспетчер IOCTL — таблица переходов по коду команды (switch/case через jump table).

### Декомпиляция:
```c
void __fastcall PIC_IOCTL(_DWORD *cmd_struct)
{
  udelay(1000);                    // пауза 1мс
  if ( *cmd_struct >= 0x10u )      // если код >= 16 -> выход
    return;
  // jump table по *cmd_struct
  __asm { jr $v0 }                 // косвенный переход
}
```

### Дизассемблер (полный):
```mips
413468  addiu   $sp, -0x30
413470  move    $s0, $a0              ; cmd_struct
413474  li      $a0, 0x3E8            ; 1000 мкс
413488  sw      $zero, var_C($sp)     ; обнуление локальных
41348c  jal     sub_249210            ; udelay(1000)
413490  sh      $zero, var_8($sp)
413494  lw      $v0, 0($s0)           ; cmd_code = *cmd_struct
413498  sltiu   $v1, $v0, 0x10        ; if code < 16
41349c  bnez    $v1, loc_4134C4       ; -> jump table
; (else -> возврат)

4134c4  sll     $v0, 2               ; code * 4
4134c8  li      $v1, 0x80597AE0      ; jump table address
4134cc  addu    $v0, $v1, $v0        ; &table[code]
4134d0  lw      $v0, 0($v0)          ; handler = table[code]
4134d4  jr      $v0                  ; переход к обработчику
```

**Jump table**: `0x80597AE0` — 16 записей (коды 0-15), каждая указывает на функцию-обработчик.
Вероятные обработчики: PIC_CALIBRATE, PIC_BAT_READ_CMD, PIC_BUZZER, PIC_BAT_READ2 и др.

---

## PIC_PARSE_BATTERY

**Адрес**: 0x413CA0 | **Размер**: 0xF4 (244 байт) | **Декомпиляция не удалась, дизассемблер**

Парсинг данных батареи. Применяет калибровочные таблицы для преобразования сырых показаний.

### Дизассемблер (ключевые фрагменты):
```mips
413cbc  jal     sub_414468           ; подготовка (?)
413cc0  sw      $v0, 0($a0)         ; сохранение результата
413cc4  jal     PIC_READ_DATA        ; чтение сырых данных
413ccc  lw      $v1, 8($s0)         ; raw_value = data_struct->field_8
413cd0  li      $a0, 0xFFFFFFFF
413cd4  bne     $v1, $a0, loc_413D0C ; if (raw != -1) -> обработка
; (else -> return, данные невалидны)

; --- Цепочка калибровочных преобразований ---
413d0c  lw      $a1, 0x8067DDBC      ; calib_param_1
413d14  jal     CALIB_LOOKUP          ; lookup(raw, param1)
413d20  lw      $a1, 0x8067DDC0      ; calib_param_2
413d24  jal     CALIB_LOOKUP          ; lookup(result, param2)
413d30  lw      $a1, 0x8067DDC4      ; calib_param_3
413d34  jal     CALIB_LOOKUP          ; lookup(result, param3)
413d40  lw      $a1, 0x8067DD88      ; calib_param_4
413d44  jal     CALIB_LOOKUP          ; lookup(result, param4)

; --- Проверка состояния зарядки ---
413d54  lw      $v0, 0x8159A6CC      ; charge_state
413d58  bnez    $v0, loc_413F38      ; if (charging) -> другая ветка
413d64  lw      $a0, 0x8159A6D0      ; charge_mode
413d6c  li      $v0, 1
413d70  beq     $a0, $v0, loc_414230 ; if (mode==1) -> printk
413d7c  lw      $a1, 0x8067DDCC      ; interpolation_param
413d84  jal     CALIB_INTERP          ; интерполяция

; --- Ветка charging ---
413f38  li      $a0, 1
413f3c  bne     $v0, $a0, loc_4140A4 ; if (charge_state != 1) -> ...
```

**Логика парсинга**:
1. Чтение сырых данных через PIC_READ_DATA
2. Проверка валидности (raw != 0xFFFFFFFF)
3. Цепочка из 4 калибровочных преобразований (CALIB_LOOKUP)
4. Проверка состояния зарядки (0x8159A6CC)
5. Интерполяция (CALIB_INTERP) для получения финального значения

**Глобальные переменные**:
- `0x8159A6CC` — состояние зарядки (0=разряд, 1=заряд)
- `0x8159A6D0` — режим заряда

---

## PIC_BATTERY_MONITOR

**Адрес**: 0x413DCC | **Размер**: 0x164 (356 байт) | **Декомпиляция не удалась, дизассемблер**

Монитор батареи — основная функция периодического контроля. Вызывает PIC_PARSE_BATTERY.

### Дизассемблер (ключевые фрагменты):
```mips
; --- Цепочка калибровочных вычислений ---
413dd4  lw      $a1, 0x8067DDD4      ; param
413dd8  jal     CALIB_LOOKUP2         ; lookup2(raw, param)
413de4  lw      $a1, 0x8067DDD8      ; param
413de8  jal     CALIB_LOOKUP          ; lookup(result, param)
413df4  move    $s1, $v0             ; calibrated_value

; --- Серия вычислений CALIB_CALC/CALIB_CALC2 ---
413e04  lw      $a3, 0x8159A6D4      ; глоб. параметр
413e08  lw      $a0, 0x8159A6D8      ; глоб. параметр
413e14  jal     CALIB_CALC2
413e20  jal     CALIB_CALC
413e2c  jal     CALIB_CALC2           ; для calibrated_value
413e38  jal     CALIB_CALC
413e44  jal     CALIB_CALC2           ; для arg_30
413e50  jal     CALIB_CALC

; --- printk с результатами ---
413e78  li      $a0, 0x8067D8DC      ; format string
413e9c  jal     sub_525FC8           ; printk(...)

; --- Проверка диапазона напряжения ---
413ea4  lw      $s6, 0xC($s0)        ; charge_status
413ea8  li      $v0, 1
413eac  beq     $s6, $v0, loc_413ECC ; if (charging) -> ...
413eb4  lw      $v0, 8($s0)          ; voltage
413eb8  slti    $v1, $v0, 0x191      ; < 401 (3.21V?)
413ebc  bnez    $v1, loc_41433C      ; -> слишком низко
413ec0  slti    $v0, 0x21E           ; < 542 (4.34V?)
413ec4  bnez    $v0, loc_4142F0      ; -> нормальный диапазон -> PIC_PARSE_BATTERY
413ec8  li      $v0, 0xB             ; 11 = статус "высокое напряжение"

; --- Обработка при нормальном/высоком напряжении ---
413ecc  lw      $v1, 0x8159A6DC      ; prev_state
413ed4  li      $v0, 1
413ed8  beq     $v1, $v0, loc_413EF0 ; if (prev_state==1) -> обработка
413eec  li      $v0, 0xA             ; 10 = статус "норма"

; --- Вызов PIC_PARSE_BATTERY ---
413ef4  jal     CALIB_CALC2
413ef8  sw      $v0, 0($s0)          ; status = result
413f00  lw      $a0, 0x81391FBC      ; callback struct
413f0c  li      $a1, 0x81391FAC      ; callback table
413f10  move    $a2, $s0             ; data struct
413f14  jal     PIC_PARSE_BATTERY
413f18  li      $a3, 0x334           ; size = 820

; --- Сохранение состояния ---
413f20  sw      $s1, 0x8159A6D8      ; prev_calibrated_value = current
413f24  sw      $v0, 0x8159A6D4      ; prev_charge_status
413f28  j       sub_414CDC           ; -> завершение
413f2c  sw      $zero, 0x8159A6DC    ; reset state
```

**Пороги напряжения**:
- `< 0x191` (401) = **критически низкое** (~3.2V) → printk предупреждение
- `< 0x21E` (542) = **нормальный диапазон** → PIC_PARSE_BATTERY с status=0x0B (11)
- `>= 0x21E` = **высокое/полная зарядка** → CALIB_CALC2 + PIC_PARSE_BATTERY с status=0x0A (10)

**Глобальные переменные**:
- `0x8159A6D4` — предыдущий статус зарядки
- `0x8159A6D8` — предыдущее калиброванное значение
- `0x8159A6DC` — предыдущее состояние

---

## PIC_READ_DATA

**Адрес**: 0x4080DC | **Размер**: 0x2C (44 байт) | **Декомпиляция + Дизассемблер**

Чтение данных из внутреннего буфера (swap операция).

### Декомпиляция:
```c
void __fastcall PIC_READ_DATA(int a1, int a2, int a3, int *a4)
{
  int *v4;          // $s6 (другой буфер)
  int v5;

  v5 = *a4;         // tmp = *dst
  *a4 = *v4;        // *dst = *src (swap!)
  *v4 = v5;         // *src = tmp
  JUMPOUT(0x408DDC); // -> продолжение цикла
}
```

### Дизассемблер:
```mips
4080dc  lw      $v1, 0($s6)           ; v1 = *src
4080e0  lw      $v0, 0($a3)           ; v0 = *dst
4080e4  sw      $v1, 0($a3)           ; *dst = v1 (src -> dst)
4080e8  sw      $v0, 0($s6)           ; *src = v0 (swap)
4080ec  lw      $a3, arg_14($sp)      ; обновление указателя
4080f0  li      $v1, 1                ; flag = 1
4080f4  addu    $a3, $s1              ; a3 += step
4080f8  sw      $a3, arg_14($sp)      ; сохранение
4080fc  subu    $s6, $a2, $s1         ; s6 = a2 - step
408100  j       loc_408DDC            ; -> основной цикл
408104  sw      $v1, arg_20($sp)      ; flag = 1
```

**Назначение**: Часть системы обработки данных — swap элементов между буферами при чтении/записи калибровочных данных. Это не прямое I2C чтение, а операция над внутренними структурами.

---

## PIC_GET_READINGS

**Адрес**: 0x40A210 | **Размер**: 0x1F0 (496 байт) | **Декомпиляция**

Получение показаний батареи с применением калибровки. Основная функция чтения 3 каналов.

### Декомпиляция (упрощённая):
```c
int __fastcall PIC_GET_READINGS(/* множество параметров */)
{
  // Проверка валидности
  if (compare(val, 0) <= 0) {
    // Невалидные данные
    result = lookup(a11, MEMORY[0x8067DD8C]);
    *out3 = to_int(result);
    JUMPOUT(0x40B22C);
  }

  *out3 = default_val;

  // --- Канал 1 (ток?) ---
  if (out_current) {
    PIC_READ_DATA(channel1, ...);
    val1 = CALIB_LOOKUP(raw1, calib_table1);
    val1_scaled = multiply(val1, scale1);
    int_val1 = to_int(val1_scaled);
    PIC_READ_DATA(int_val1, ...);
    // Проверка знака
    if (compare(val1_scaled, 0) > 0) {
      adjusted = multiply(val1_scaled, MEMORY[0x8067DD8C]);
      int_val1 = -to_int(adjusted);
    }
    *out_current = -int_val1; // или int_val1
  }

  // --- Канал 2 (напряжение?) ---
  if (out_voltage) {
    PIC_READ_DATA(channel2, ...);
    val2 = CALIB_LOOKUP(raw2, calib_table2);
    val2_scaled = multiply(val2, scale2);
    int_val2 = to_int(val2_scaled);
    PIC_READ_DATA(int_val2, ...);
    if (compare(val2_scaled, 0) > 0) {
      adjusted = multiply(val2_scaled, MEMORY[0x8067DD8C]);
      int_val2 = to_int(adjusted);
    }
    *out_voltage = int_val2;
  }

  // --- Канал 3 (температура?) ---
  if (out_temp) {
    PIC_READ_DATA(channel3, ...);
    val3 = CALIB_LOOKUP(raw3, calib_table3);
    val3_scaled = multiply(val3, scale3);
    int_val3 = to_int(val3_scaled);
    PIC_READ_DATA(int_val3, ...);
    if (compare(val3_scaled, 0) > 0) {
      *out_temp = -int_val3;
      return;
    }
    // Дополнительная обработка через CALIB_LOOKUP2
    ...
  }

  return sub_40B400(...);
}
```

**Структура**: Читает до 3 каналов данных, к каждому применяет:
1. PIC_READ_DATA — получение сырого значения
2. CALIB_LOOKUP — калибровочное преобразование
3. multiply/scale — масштабирование
4. Проверка знака и инверсия при необходимости

---

## sub_413F78

**Адрес**: 0x413F78 | **Размер**: ~600+ байт (фрагментирована) | **Декомпиляция + Дизассемблер**

Высокоуровневая I2C обёртка. Используется всеми PIC-функциями для I2C транзакций.

### Декомпиляция:
```c
void sub_413F78()
{
  if ( compare_zero(val5, 0) )           // проверка timeout
  {
    if ( compare(val1, MEMORY[0x8067DDB8]) ) // сравнение с порогом
    {
      if ( compare_zero(val1, 0) <= 0 )  // если <= 0
      {
        goto normal_path;
      }
      if ( compare(val5, MEMORY[0x8067DDB8]) ) // ещё одна проверка
        JUMPOUT(0x414FC0);               // -> ошибка/переполнение
    }
    else if ( !compare_neq(val5, MEMORY[0x8067DDB8]) )
    {
      normal_path:
      if ( flag_s6 )
      {
        sub_407D4C(val5, val1);          // вычитание?
        if ( CALIB_INTERP() >= 0 )       // интерполяция
          goto success;
      }
      else
      {
        result = sub_407D4C(val5, val1);
        if ( compare(result, MEMORY[0x8067DDD8]) <= 0 )
          success:
          JUMPOUT(0x414CE4);             // -> успех
      }
      JUMPOUT(0x413EEC);                 // -> ошибка/retry
    }
  }
  JUMPOUT(0x413EF0);                     // -> таймаут
}
```

### Дизассемблер (ключевые фрагменты):
```mips
; --- Основная логика I2C write ---
413fec  lw      $fp, arg_68($sp)
413ff0  lw      $a1, 0x8067DDD0       ; param
413ff4  lw      $a0, arg_30($sp)      ; данные
413ff8  jal     sub_408C8C            ; сравнение
414004  bltz    $v0, loc_4140B0       ; -> другой путь
41400c  lw      $a1, 0x8067DDDC       ; param
414014  jal     CALIB_INTERP          ; интерполяция
41401c  bgez    $v0, loc_4140B0       ; -> успех

; --- Цепочка калибровки при необходимости ---
414028  lw      $a1, 0x8067DDD0
41402c  jal     sub_407D4C            ; вычитание
41403c  jal     CALIB_LOOKUP2         ; lookup2
41404c  jal     CALIB_LOOKUP          ; lookup
41405c  jal     sub_407C14            ; умножение/деление
414064  move    $s1, $v0              ; результат
414068  j       loc_414DF8            ; -> запись результата

; --- Повтор для второго диапазона ---
4140b0  lw      $a1, 0x8067DDDC
4140b8  jal     sub_408C8C
4140c4  bltz    $v0, loc_414130
4140d4  jal     CALIB_INTERP
4140dc  bgez    $v0, loc_414130
; ... аналогичная цепочка ...
```

**Назначение**: Комплексная обёртка над I2C транзакцией с:
- Проверкой диапазонов
- Калибровочной интерполяцией
- Обработкой таймаутов
- Автоматическим retry

---

## Поиск команды INIT (0x41)

Поиск immediate-значения `0x41` (65) в IDA не дал прямых совпадений в PIC-функциях. Команда `0x41` (INIT) может:

1. Использоваться через jump table PIC_IOCTL (код >15 отбрасывается, но команда 0x41 может быть ioctl-кодом из userspace)
2. Формироваться динамически из переменной
3. Находиться в другом модуле (AlmondTouch.ko, AlmondBattery.ko)
4. Быть командой из userspace-приложения (libAlmond.so), а не ядра

---

## Анализ протокола

### SM0 (I2C Auto Mode) Регистры MT7621

| Адрес | Регистр | Назначение |
|-------|---------|-----------|
| 0xBE000908 | SM0_DATA | I2C slave address (записывается 0x2A = 42) |
| 0xBE000910 | SM0_DATAOUT | Данные для записи (первый байт команды) |
| 0xBE000918 | SM0_STATUS | Статус: бит 0 = BUSY |
| 0xBE00091C | SM0_AUTOMODE | Режим: 0=write, 1=read |
| 0xBE000920 | SM0_BYTECNT | Количество байт (len-1 для чтения, len для записи) |

### I2C адрес PIC

**0x2A** (42 decimal, 7-bit) — жёстко закодирован в PIC_I2C_WRITE (инструкция `li $v1, 0x2A`).

### Командные байты (I2C write протокол)

| Байт | Hex | Назначение | Формат пакета |
|------|-----|-----------|---------------|
| `0x2D` | '-' | Table1 (калибровочные данные, часть 1) | `[0x2D, data...]` |
| `0x2E` | '.' | Table2 (калибровочные данные, часть 2) | `[0x2E, data...]` |
| `0x2F` | '/' | BAT_READ (чтение батареи) | `[0x2F, 0x00, count]` |
| `0x33` | '3' | WAKE/CALIB_START (начало калибровки) | `[0x33, hi_byte, count]` |
| `0x34` | '4' | BUZZER (управление буззером) | `[0x34, 0x00, mode]` |
| `0x41` | 'A' | INIT (инициализация — не найдена в ядре напрямую) | предположительно `[0x41, ...]` |

### Протокол BAT_READ (0x2F)

```
MT7621 -> PIC:  [0x2F, 0x00, N]     (запрос чтения N байт)
PIC -> MT7621:  [byte0, byte1, ...]  (ответ — N байт данных)
```

- `N=1` — однобайтовый ответ (PIC_BAT_READ_CMD)
- `N=2` — двухбайтовый ответ (PIC_BAT_READ2)

### Протокол калибровки

Последовательность:
1. **WAKE**: `[0x33, count_hi, count_lo]` — пробуждение PIC и указание кол-ва записей
2. **Table1**: `[0x2D, hi0, lo0, hi1, lo1, ...]` — первая таблица калибровки (byte-swapped!)
3. **Table2**: `[0x2E, hi0, lo0, hi1, lo1, ...]` — вторая таблица калибровки (byte-swapped!)

**ВАЖНО**: Данные в таблицах калибровки передаются в **big-endian** порядке:
- Исходный 32-bit int конвертируется: `[value >> 8, value & 0xFF]`
- Каждая таблица до 200 записей (100 int * 2 байта = 200 + 1 cmd byte)

### Byte-swap в PIC_CALIB_LOOP

```
Исходный int32: 0x00001234
Передаётся как: [0x12, 0x34]  (big-endian для 16-bit значения)

Формула:
  hi_byte = (value >> 8) & 0xFF   (sra $a1, $a0, 8; sb $a1, 0($v0))
  lo_byte = value & 0xFF          (sb $a0, 1($v0))
```

### Структура калибровки (calib_struct)

```
Смещение  Размер  Назначение
+0x00     4       (используется sub_414468)
+0x04     1       entries_count (кол-во записей калибровки)
+0x10     4       field_10 (доп. параметр)
+0x14     400     Table1 (100 x int32)
+0x1A4    400     Table2 (100 x int32)
```

Общий размер: **0x334** (820 байт) — подтверждено `li $a3, 0x334` при вызове PIC_PARSE_BATTERY.

### Глобальные переменные батареи

| Адрес | Назначение |
|-------|-----------|
| 0x80704C84 | spinlock (мьютекс для I2C операций) |
| 0x80704D88 | battery_read_pending (флаг ожидания чтения) |
| 0x80704DCC | timer struct (для mod_timer) |
| 0x8159A030 | глобальная структура калибровки |
| 0x8159A034 | entries_count (глобальная копия) |
| 0x8159A035 | buzzer_mode (текущий режим буззера) |
| 0x8159A040 | field_10 (глобальная копия) |
| 0x8159A044 | Table1 (400 байт — глобальная копия) |
| 0x8159A1D4 | Table2 (400 байт — глобальная копия) |
| 0x8159A6BC | jiffies (текущее время ядра) |
| 0x8159A6CC | charge_state (0=разряд, 1=заряд) |
| 0x8159A6D0 | charge_mode |
| 0x8159A6D4 | prev_charge_status |
| 0x8159A6D8 | prev_calibrated_value |
| 0x8159A6DC | prev_state |

### Пороги напряжения батареи

| Значение | Hex | Описание |
|----------|-----|----------|
| 401 | 0x191 | Критически низкое напряжение (~3.2V) |
| 542 | 0x21E | Граница нормального/высокого (~4.3V) |

При voltage < 401 — printk предупреждение о низком заряде.
При voltage >= 542 — считается полностью заряженной.

### Вспомогательные функции

| Адрес | Назначение |
|-------|-----------|
| sub_249210 | udelay() — микросекундная задержка |
| sub_249620 | memset() |
| sub_2492E0 | memcpy() |
| sub_33AD8 | msecs_to_jiffies() |
| sub_48B38 | mod_timer() |
| sub_52A614 | spin_lock() |
| sub_52A264 | spin_unlock() |
| sub_525FC8 | printk() |
| sub_407C14 | fixed-point multiply/divide |
| sub_407D4C | fixed-point subtract |
| sub_408C6C | fixed-point compare (bgtz = >) |
| sub_408CF8 | fixed-point compare (equality) |
| sub_408D04 | fixed-point compare (not equal) |
| sub_408C8C | fixed-point compare (signed) |
| CALIB_LOOKUP | калибровочный lookup по таблице |
| CALIB_LOOKUP2 | калибровочный lookup (вариант 2) |
| CALIB_INTERP | калибровочная интерполяция |
| CALIB_CALC | калибровочное вычисление |
| CALIB_CALC2 | калибровочное вычисление (вариант 2) |

### Логика мониторинга батареи (PIC_BATTERY_MONITOR)

```
1. Чтение сырых данных из PIC (через BAT_READ)
2. Серия калибровочных преобразований:
   raw -> CALIB_LOOKUP2 -> CALIB_LOOKUP -> calibrated_value
3. Вычисление параметров через CALIB_CALC/CALIB_CALC2
4. printk с результатами мониторинга
5. Проверка диапазона напряжения:
   - < 401: критически низкое → предупреждение
   - 401-541: нормальное → PIC_PARSE_BATTERY(status=11)
   - >= 542: высокое/полное → PIC_PARSE_BATTERY(status=10)
6. Сохранение состояния в глобальные переменные
```

### Выводы для реализации в OpenWrt

1. **I2C Auto Mode**: Нужно использовать прямой доступ к SM0 регистрам (palmbus 0xBE000900+), НЕ стандартный Linux i2c-dev. Стоковое ядро работает в обход i2c-mt7621 драйвера.

2. **Адрес PIC = 0x2A** (42): Подтверждено жёстким кодированием в PIC_I2C_WRITE.

3. **Spinlock 0x80704C84**: Все I2C операции с PIC защищены мьютексом. В нашем lcd_drv.ko нужна аналогичная защита при разделении I2C шины с тачскрином.

4. **udelay(5000)**: Между всеми I2C операциями вставлена задержка 5мс. Это важно для стабильности связи с PIC.

5. **Калибровка необходима**: Без таблиц калибровки (Table1/Table2) показания батареи будут сырыми АЦП-значениями, не переведёнными в вольты/проценты.

6. **Byte-swap при калибровке**: При записи таблиц в PIC, 16-bit значения передаются в big-endian формате (`[hi, lo]`), хотя MT7621 — little-endian.

7. **Команда 0x41 (INIT)**: Не найдена в ядре. Возможно используется из userspace (libAlmond.so) или другого модуля.
