# PIC16LF1509 Firmware Complete Disassembly Analysis
# Securifi Almond 3S — Battery/Power/Buzzer Controller
# Дата анализа: 2026-03-24

## Обзор

Прошивка PIC16LF1509 (Enhanced Midrange, 14-bit) управляет:
- Мониторингом батареи через АЦП
- Управлением питания (power on/off/reboot)
- Бипером (мелодии)
- I2C slave (адрес 0x2A, write=0x54, read=0x55) для общения с MT7621

Размер кода: 0x0000-0x0B66 (2918 инструкций), остальное — 0x3FFF (стёрто).

## Карта памяти программ

| Диапазон | Описание |
|----------|----------|
| 0x0000-0x0001 | Reset vector: `movlp 0x08; goto 0x0816` (переход на main) |
| 0x0004-0x03F2 | ISR (Interrupt Service Routine) — обработка I2C и Timer1 |
| 0x03F3-0x0428 | Функция multiply_32x32 (умножение 32-бит) |
| 0x0429-0x043A | Функция delay_loop (задержка по счётчику) |
| 0x043B-0x043F | Функция short_delay (фиксированная короткая задержка ~29 циклов) |
| 0x0440-0x05B6 | Функция divide_32x32 (деление 32-бит) |
| 0x05B7-0x05D2 | Функция divide_16x16 (деление 16-бит) |
| 0x05D3-0x05D9 | Функция init_adc_scale (загрузка константы 0x3E80 = 16000) |
| 0x05DA-0x05F1 | Функция adc_read_raw (чтение одного АЦП канала) |
| 0x05F2-0x0600 | Функция adc_config (конфигурация АЦП модуля) |
| 0x0601-0x06E1 | Функция pwm_compute (вычисление PWM для бипера) |
| 0x06E2-0x06EA | Функция medium_delay (задержка ~450 циклов) |
| 0x06EB-0x072B | Функция beep_startup (мелодия при включении — 3 ноты) |
| 0x072C-0x0744 | Функция calibrate_wait (ожидание калибровки с задержкой) |
| 0x0745-0x076E | Функция multiply_16x16 (умножение 16-бит) |
| 0x076F-0x0779 | Функция timer1_config (конфигурация Timer1) |
| 0x077A-0x0781 | Функция interrupts_enable (включение прерываний GIE/PEIE) |
| 0x0782-0x0792 | Функция i2c_slave_init (инициализация I2C slave 0x54=0x2A) |
| 0x0793-0x0799 | Функция adc_read_channel (чтение АЦП канала N) |
| 0x079A-0x07D4 | Функция gpio_output_pwm (вывод PWM на GPIO с подсчётом) |
| 0x07D5-0x07F5 | Функция gpio_port_init (инициализация GPIO порта для бипера/LED) |
| 0x07F6-0x07FF | Padding/unused |
| 0x0800-0x080C | Функция peripheral_init (конфиг ANSEL/TRIS/OSCCON) |
| 0x080D-0x0815 | Функция memcpy (копирование данных FSR0→FSR1) |
| 0x0816-0x0842 | **MAIN** — точка входа, инициализация |
| 0x0843-0x0A79 | **MAIN LOOP** — основной цикл (бипер, калибровка, батарея) |
| 0x0A7E-0x0B30 | Таблица данных — нотные частоты и калибровочные значения |
| 0x0B32-0x0B66 | Функция ram_init (инициализация RAM из flash) |

## Карта переменных (RAM, Bank 0 = 0x20-0x7F)

### Регистры общего назначения (bank 0)
| Адрес | Имя | Описание |
|-------|-----|----------|
| 0x20 | temp_div | Временная для деления |
| 0x21-0x27 | isr_save_* | Сохранение контекста ISR (0x7d→0x21, 0x7c→0x22, 0x73→0x23, 0x72→0x24, 0x71→0x25, 0x70→0x26, не заполнено) |
| 0x28 | **cmd_state** | Состояние I2C команды (определяет что отдавать при master read) |
| 0x29 | **cmd_sub_state** | Подсостояние (0=жду 1-й байт данных, 1=жду 2-й байт) |
| 0x2A | **table_size_lo** | Размер таблицы / массива (low byte) |
| 0x2B | **table_size_hi** | Размер таблицы / массива (high byte) |
| 0x2C | **read_byte_idx** | Индекс байта при master read (0,1,2 — циклически) |
| 0x2D | **tmr1_counter** | Счётчик тиков Timer1 (0-0x17, переполнение каждые 24 тика) |
| 0x2E | **adc_ready_flag** | Флаг "АЦП данные готовы" (1=готовы) |
| 0x2F | **prev_charge_state** | Предыдущее состояние зарядки (для обнаружения изменений) |
| 0x30 | **tmr1_overflow** | Флаг переполнения Timer1 (24 тика = ~3 секунды) |
| 0x31 | **charge_state** | Текущее состояние зарядки (из GPIO pin bit 3 PORTE) |
| 0x32 | **buzzer_active** | Флаг активности бипера (копия 0x34) |
| 0x33 | **led_blink_mode** | Режим мигания LED (!=0 → мигает, bit4 XOR на PORTE) |
| 0x34 | **buzzer_request** | Запрос на включение бипера (1=вкл, 0=выкл) |
| 0x35 | **charge_changed** | Флаг "зарядка изменилась" (1=обнаружено изменение) |
| 0x36 | **charge_change_val** | Значение изменения зарядки |
| 0x37 | **charge_snapshot** | Снимок состояния зарядки для ADC отчёта |
| 0x38 | **adc_byte0** | ADC данные байт 0 (HIGH byte) — для master read |
| 0x39 | **adc_byte1** | ADC данные байт 1 (LOW byte) — для master read |
| 0x3A | **adc_byte2** | ADC данные байт 2 (статус/флаги) — для master read |
| 0x3B | **adc_byte2_hi** | ADC данные расширение (используется при вычислении) |
| 0x3C | **main_state** | Состояние главного цикла (1=note1, 2=idle, 3=note_table, 4=beep_melody) |
| 0x3D | **i2c_rx_byte** | Последний принятый I2C байт |
| 0x3E | **sspstat_masked** | SSP1STAT & 0x2D (маскированный статус I2C) |
| 0x3F | **gpio_mask_inv** | Инвертированная маска GPIO для порта |
| 0x40 | **gpio_port_lo** | Адрес GPIO порта (low byte) |
| 0x41 | **gpio_port_hi** | Адрес GPIO порта (high byte) |
| 0x42 | **gpio_mask** | Маска бита GPIO |
| 0x43 | **cmd1_write_cnt_lo** | Счётчик записей cmd 0x2D (low) |
| 0x44 | **cmd1_write_cnt_hi** | Счётчик записей cmd 0x2D (high) |
| 0x45 | **cmd1_data_lo** | Данные cmd 0x2D (low byte) |
| 0x46 | **cmd1_data_hi** | Данные cmd 0x2D (high byte) |
| 0x47 | **isr_state** | Тип ISR события (1=addr+write, 2=data+write, 3=addr+read, 4=nack+write, 5=nack+read) |
| 0x48-0x49 | **pwm_period** | Период PWM (16-бит) |
| 0x4A-0x4B | **pwm_step** | Шаг PWM (16-бит) |
| 0x4C-0x4D | **note_freq** | Частота ноты (16-бит) |
| 0x4E-0x4F | **note_base** | Базовая нота (16-бит) |
| 0x50-0x51 | **melody_repeat** | Счётчик повторов мелодии (16-бит) |
| 0x52-0x53 | **pwm_counter** | Счётчик PWM циклов (16-бит) |
| 0x54 | **note_index** | Индекс текущей ноты |
| 0x55-0x56 | **cmd2_data** | Данные cmd 0x2E (16-бит) |
| 0x57-0x58 | **cmd9_data** | Данные cmd 0x35 (16-бит) |
| 0x59-0x5A | **cmd7_data** | Данные cmd 0x33 (16-бит) |
| 0x5B-0x5C | **cmd8_data** | Данные cmd 0x34 (16-бит, управление бипером) |
| 0x5D-0x60 | **adc_config_regs** | Конфигурация АЦП (ADCON, channel и т.д.) |
| 0x61-0x62 | **cmd2_write_cnt** | Счётчик записей cmd 0x2E (16-бит) |
| 0x63-0x64 | **cmd3_data** | Данные cmd 0x2F (16-бит, кол-во байт для чтения) |
| 0x65-0x66 | **calc_temp** | Временные для вычислений (16-бит) |
| 0x67-0x68 | **gpio_reg_addr** | Адрес GPIO регистра (16-бит) |
| 0x69 | **gpio_bit_num** | Номер бита GPIO |
| 0x6A-0x6B | **pwm_freq** | Частота для PWM (16-бит) |
| 0x6C-0x6D | **pwm_duty** | Скважность PWM (16-бит) |
| 0x6E-0x6F | **adc_scaled** | Масштабированное значение АЦП (16-бит) |
| 0x70-0x7D | **temp** | Временные регистры / общий аккумулятор |

### Банки 1 и 3 (SFR через BSR)
- Bank 1 (0x01): TRISA=0x0C, TRISB=0x0D, TRISC=0x0E, PIE1=0x11, T1CON=0x18, T1GCON=0x19, ADRESL=0x1B, ADRESH=0x1C, ADCON0=0x1D, ADCON1=0x1E
- Bank 3 (0x03): ANSELA=0x0C, ANSELB=0x0D, ANSELC=0x0E, локальные переменные функций
- Bank 4 (0x04): SSP1BUF=0x11, SSP1ADD=0x12, SSP1MSK=0x13, SSP1STAT=0x14, SSP1CON1=0x15, SSP1CON2=0x16, SSP1CON3=0x17
- Bank 7 (0x07): PWM регистры, CLC

## Детальный анализ ISR (0x0004-0x03F2)

### Сохранение контекста (0x0004-0x0012)
```
0x0004: Сохранить 0x70-0x74, 0x7C, 0x7D в 0x27-0x21 (reverse order)
```

### Проверка источника прерывания (0x0013-0x0015)
```
0x0013: movlp 0x00        ; убедиться что PCLATH=0
0x0014: btfss PIR1, 3     ; SSP1IF set?
0x0015: goto 0x0366       ; нет → проверить Timer1
```

### SSP1IF (I2C interrupt) — Основной обработчик (0x0016-0x0362)

#### Шаг 1: Чтение SSP1BUF если буфер полон (0x0016-0x001E)
```
0x0016: bcf PIR1, 3       ; очистить SSP1IF
0x0017: BSR=4
0x0018: btfss SSP1CON1, 6 ; SSPOV (overflow)?
0x0019: goto 0x001F       ; нет overflow
0x001A: movf SSP1BUF → W  ; чтение буфера (сброс overflow)
0x001B-001C: → 0x3D       ; сохранить в i2c_rx_byte
0x001D: BSR=4
0x001E: bcf SSP1CON1, 6   ; очистить SSPOV
```

#### Шаг 2: Определение типа I2C события (0x001F-0x0056)
Маскируем SSP1STAT & 0x2D для определения:
```
SSPSTAT bits used:
  bit 0 = BF (Buffer Full)
  bit 2 = R/W (1=read, 0=write)
  bit 3 = S (Start bit detected)
  bit 5 = D/A (1=data, 0=address)

Маска 0x2D = bits 0,2,3,5 = 0b00101101

Результат:  Значение   isr_state  Описание
0x09        → 1        Address + Write (slave addr received, write mode)
0x29        → 2        Data + Write (data byte received)
0x0D        → 3        Address + Read (slave addr received, read mode)
0x24        → 4        NACK + Write (master NACKed, write mode)
0x28        → 5        NACK + Read (master NACKed after read)
```

#### Шаг 3: Диспетчер по isr_state (0x0356-0x0362)
```
0x0356: if isr_state == 1 → goto 0x0058 (ADDRESS_WRITE)
0x035A: if isr_state == 2 → goto 0x006E (DATA_WRITE)
0x035E: if isr_state == 3 → goto 0x0313 (ADDRESS_READ / MASTER_READ)
0x0362: else              → goto 0x0351 (NACK — release CKP, clear)
```

### ADDRESS_WRITE handler (0x0058-0x006D)
```
0x0058: Чтение SSP1BUF → i2c_rx_byte (0x3D)
0x005C: Сравнить с 0x54 (write address = slave addr 0x2A << 1)
0x005F: если НЕ 0x54 → пропустить задержку
0x0060-0x0068: Если это 0x54 → задержка ~750 циклов
0x0069: bsf SSP1CON1, CKP   ; release clock (stretch release)
0x006B: bcf PIR1, SSP1IF
0x006D: goto ISR_EXIT
```

### DATA_WRITE handler (0x006E-0x030F) — ГЛАВНАЯ ТАБЛИЦА КОМАНД

Принимает данные от мастера. Читает SSP1BUF → 0x3D, затем разбирает по значению.

#### Команда 0x2D — НОТНАЯ ТАБЛИЦА 1 (cmd_state=1) [НЕ калибровка батареи!]
```
0x0072-0x0081: if i2c_rx_byte == 0x2D:
  - Очистить 0x29, 0x45, 0x46, 0x43, 0x44
  - cmd_state = 1
  - CKP release → ISR exit
```
Многобайтовый приём: первый байт → 0x45 (low), далее сдвиг: 0x45 = (0x45 << 8) | new_byte.
Записывает в массив по адресу 0x00A0 + (0x43:0x44 * 2).
Счётчик 0x43:0x44 инкрементируется. Если достиг 0x2A:0x2B → сбрасывает cmd_state=0.

**ВАЖНО**: Таблица содержит PWM частоты для мелодий, НЕ калибровку батареи!
pic_calib.h с linear ramp (4,5,6,7...) = мусор. Flash таблица 0x0A7E = ноты мелодии.

#### Команда 0x2E — НОТНАЯ ТАБЛИЦА 2 (cmd_state=2) [НЕ калибровка батареи!]
```
0x0082-0x0091: if i2c_rx_byte == 0x2E:
  - Очистить 0x55, 0x56, 0x61, 0x62, 0x29
  - cmd_state = 2
```
Аналогично 0x2D, но записывает в массив по адресу 0x0120 + (0x61:0x62 * 2).

#### Команда 0x2F — Запрос чтения АЦП (cmd_state=3)
```
0x0092-0x009F: if i2c_rx_byte == 0x2F:
  - Очистить 0x63, 0x64, 0x29
  - cmd_state = 3
```
Принимает 2-байтное значение — количество байт для чтения. По значению:
- 0x0001 → 0x3C = 1
- 0x0003 → 0x3C = 3
- 0x0002 → 0x3C = 2
- иначе → 0x3C не меняется, cmd_state = 0

**КРИТИЧЕСКИ ВАЖНО**: 0x2F с данными 0x00,0x02 (= 2 байта) ставит cmd_state=0 и 0x3C=2.
А 0x2F с данными 0x00,0x01 ставит 0x3C=1 (что запускает определённую ветвь main loop).

#### Команда 0x40 — Включить бипер
```
0x00A0-0x00A6: if i2c_rx_byte == 0x40:
  - buzzer_request (0x34) = 1
```

#### Команда 0x41 — Выключить бипер / сброс
```
0x00A7-0x00AD: if i2c_rx_byte == 0x41:
  - buzzer_active (0x32) = 0
  - buzzer_request (0x34) = 0
```

#### Команда 0x30 — Включить мигание LED + toggle PORTE bit 4
```
0x00AE-0x00B6: if i2c_rx_byte == 0x30:
  - led_blink_mode (0x33) = 1
  - PORTE XOR 0x10 (toggle bit 4)
```

#### Команда 0x31 — Выключить мигание LED, LED ON
```
0x00B7-0x00BE: if i2c_rx_byte == 0x31:
  - led_blink_mode (0x33) = 0
  - bsf PORTE, 4  (LED ON)
  - cmd_state = 0
```

#### Команда 0x32 — Выключить мигание LED, LED OFF
```
0x00BF-0x00C6: if i2c_rx_byte == 0x32:
  - led_blink_mode (0x33) = 0
  - bcf PORTE, 4  (LED OFF)
  - cmd_state = 0
```

#### Команда 0x33 — Установить размер таблицы 1 (cmd_state=7)
```
0x00C7-0x00D1: if i2c_rx_byte == 0x33:
  - CKP release
  - cmd_state = 7
```
Принимает 2 байта → 0x59:0x5A → копирует в 0x2A:0x2B (table_size)

#### Команда 0x34 — Управление бипером/режимом (cmd_state=8)
```
0x00D2-0x00DC: if i2c_rx_byte == 0x34:
  - CKP release
  - cmd_state = 8
```
Принимает 2 байта → сравнивает с 1, 2, 3:
- 0x0001 → bsf PORTA bit 0, bcf PORTA bit 1 (включить выход A)
- 0x0002 → bcf PORTA bit 0, bsf PORTA bit 1 (включить выход B)
- 0x0003 → bcf PORTA bit 0, bcf PORTA bit 1 (оба выключить)
- иначе → просто очистить cmd_state

#### Команда 0x35 — Установить повтор мелодии (cmd_state=9)
```
0x00DD-0x00E7: if i2c_rx_byte == 0x35:
  - CKP release
  - cmd_state = 9
```
Принимает 2 байта → 0x57:0x58 → копирует в 0x50:0x51 (melody_repeat)

#### Команда 0x36 — ЧТЕНИЕ АЦП! (cmd_state=0x0A)
```
0x00E8-0x013B: if i2c_rx_byte == 0x36:
```
**ЭТО КЛЮЧЕВАЯ КОМАНДА ДЛЯ LIVE ADC!**

```
1. Вызов adc_read_channel(0x0B)  — канал АЦП 0x0B (AN11)
2. Результат → 0x38 (high byte), 0x39 (low byte)
3. Вычисление 0x3A (статус-байт):
   - 0x37 (charge_snapshot) << 3
   - | (0x32 << 4)     ; buzzer_active
   - | (0x35 << 5)     ; charge_changed
   - | (0x36 << 6)     ; charge_change_val
   - | (PORTE bit 0)   ; порт зарядки
4. cmd_state = 0x0A
```

**Это ЕДИНСТВЕННОЕ место, где обновляются переменные 0x38/0x39/0x3A!**

#### Команда 0x37 — Запрос чтения (простой) (cmd_state=0x0B)
```
0x013C-0x0142: if i2c_rx_byte == 0x37:
  - cmd_state = 0x0B
```

#### Команда 0x38 — Подтверждение / завершение чтения
```
0x0143-0x014B: if i2c_rx_byte == 0x38:
  - if PORTE bit 0 → skip; else bcf PORTE bit 5
  - cmd_state = 0
```

#### Команда 0x39 — Переинициализация I2C slave
```
0x014C-0x0153: if i2c_rx_byte == 0x39:
  - Вызов i2c_slave_init (0x0782)
  - cmd_state = 0
```

#### Обработка данных для cmd_state 1-9 (0x0154-0x0304)
Для каждого cmd_state обрабатываются последующие байты данных.
(Описано выше в каждой команде.)

#### Fallback (0x0305-0x030E)
Если cmd_state не совпадает ни с чем:
```
- bcf SSPSTAT, BF
- CKP release
- Очистить все счётчики
- cmd_state = 0
```

#### Завершение DATA_WRITE (0x030F-0x0312)
```
0x030F: BSR=4, bcf SSPSTAT BF, bsf CKP
0x0312: goto ISR_EXIT (0x0363)
```

### MASTER_READ handler (0x0313-0x0350)

**Когда мастер читает данные (isr_state=3):**

#### Если cmd_state == 0x0A (после команды 0x36):
```
0x0313-0x0344:
  if read_byte_idx == 0: SSP1BUF = adc_byte0 (0x38)
  if read_byte_idx == 1: SSP1BUF = adc_byte1 (0x39)
  if read_byte_idx == 2: SSP1BUF = adc_byte2 (0x3A)

  CKP release, clear SSP1IF
  Wait until SSPSTAT.BF is set (poll loop!)
  Increment read_byte_idx
  if read_byte_idx >= 3: reset to 0, cmd_state = 0
```

**ВАЖНО**: read_byte_idx (0x2C) инкрементируется ПОСЛЕ каждого чтения мастером. Мастер получает 3 байта: [0x38, 0x39, 0x3A], затем цикл сбрасывается.

#### Если cmd_state == 0x0B (после команды 0x37):
```
0x0345-0x034F:
  SSP1BUF = 0x07 (фиксированная константа — firmware version?)
  CKP release, clear SSP1IF
  cmd_state = 0
```

### Timer1 ISR (0x0366-0x03E2)

```
0x0366: BSR=0
0x0367: btfss PIR1, TMR1IF
0x0368: goto 0x039A (→ проверка PWM/CLC)
0x0369: bcf PIR1, TMR1IF
0x036A: tmr1_counter++ (increment 0x2D)
```

#### Каждые 24 тика Timer1 (~3 секунды):
```
0x036E: if tmr1_counter == 0x18:
  - tmr1_counter = 0
  - tmr1_overflow = 1 (0x30 = 1)
```

#### Каждые 8 тиков — проверка состояния зарядки:
```
0x0375: if (tmr1_counter & 0x07) == 0:
  - charge_state = (PORTE bit 3) ? 1 : 0
  - if charge_state != prev_charge_state:
    - charge_changed = 1
    - charge_change_val = 1
  - else:
    - if charge_changed != 0:
      - charge_snapshot = charge_state
    - charge_changed = 0
  - prev_charge_state = charge_state
```

#### Мигание LED (0x038F-0x0398):
```
if led_blink_mode != 0:
  if (tmr1_counter & 0x03) == 0:
    - PORTE XOR 0x10 (toggle LED bit 4)
```

### Вторая ветвь Timer ISR — PWM/CLC (0x039A-0x03DE)

```
0x039A: BSR=7
0x039B: btfss [PWM_reg], 2 ; проверка CLC флага
0x039C: goto 0x03DF (→ проверить TMR6)

Если CLC флаг:
  0x039D-0x039F: buzzer_active = buzzer_request
  0x03A1: bcf CLC flag

  if adc_ready_flag (0x2E) != 0:
    - Вызов gpio_output_pwm(0x0C, 0x00, 0x02, 0x02, 0x01)
      Т.е. бипер: PORTC, bit 0, длина 2, 2 раза, enable=1
    - Если вернулся OK (0x70 != 0):
      - if tmr1_overflow (0x30) != 0:
        - if buzzer_request:
          - bsf PORTE bit 5
        - else:
          - bcf PORTE bit 5
        - tmr1_overflow = 0
        - if buzzer_request == 0:
          - bsf PORTE bit 4 (LED on)
      - else (tmr1_overflow == 0):
        - if PORTE bit 5 is clear:  ← SHUTDOWN DETECTED (кнопка питания!)
          - bsf PORTE bit 5       ← подтвердить shutdown
          - bcf PORTE bit 4       ← LED OFF
          - 0x3C = 4              ← main_state = BEEP_MELODY (мелодия выключения)
      - adc_ready_flag = 0

  Затем ВСЕГДА:
    - gpio_output_pwm(0x0C, 0x00, 0x02, 0x02, 0x00)
      Т.е. бипер: PORTC, bit 0, длина 2, 2 раза, enable=0
    - Если OK:
      - adc_ready_flag = 1
      - tmr1_overflow = 0
      - tmr1_counter = 0
```

### Восстановление контекста ISR и retfie (0x03E3-0x03F2)
```
Восстановить 0x21→0x7D, 0x22→0x7C, 0x23→0x73, 0x24→0x72, 0x25→0x71, 0x26→0x70, 0x27→не использ.
retfie
```

## Инициализация MAIN (0x0816-0x0842)

```
0x0816: call 0x0B32        ; ram_init — копирование таблиц из flash в RAM
0x0817: main_state = 2     ; начинаем в состоянии "idle"
0x081A: call 0x0800        ; peripheral_init — конфиг GPIO/ANSEL/OSCCON
0x081C: bcf PORTE, 5       ; SHUTDOWN signal OFF (питание включено)
0x081D: bsf PORTE, 4       ; LED ON (индикатор питания)
0x081E: T1GCON = 0x7A      ; Timer1 gate control
0x0822: Задержка ~3*8*119 = ~2856 циклов
0x082D-0x0833: gpio_reg_addr = 0x000E, gpio_bit_num = 2
0x0834: call gpio_port_init (0x07D5) ; инициализация GPIO порта
0x0838: call i2c_slave_init (0x0782) ; I2C slave, addr=0x2A
0x083A: call interrupts_enable (0x077A) ; GIE + PEIE + TMR6
0x083E: call timer1_config (0x076F) ; Timer1 setup
0x0840: bsf PORTA, 0; bcf PORTA, 1  ; начальное состояние выходов
0x0842: goto 0x0A69        ; → main loop dispatcher
```

## Основной цикл MAIN LOOP (0x0843-0x0A79)

### Диспетчер (0x0A69-0x0A79)
```
0x0A69: if main_state == 1 → goto 0x0843 (NOTE_PLAY_1)
0x0A6D: if main_state == 3 → goto 0x09F7 (NOTE_TABLE)
0x0A71: if main_state == 2 → goto 0x0A62 (IDLE → IDLE)
0x0A75: if main_state == 4 → goto 0x0A63 (BEEP_MELODY)
0x0A79: goto 0x0842        ; default → повтор
```

### Состояние 2 — IDLE (0x0A62)
```
0x0A62: goto 0x0A79 → goto 0x0842 → goto 0x0A69 (бесконечный цикл опроса)
```
**В состоянии IDLE программа просто крутится в цикле, ожидая прерывания.**
Нет никакого активного polling АЦП!

### Состояние 1 — NOTE_PLAY_1 (0x0843-0x08A1)
Проигрывание одной ноты фиксированной мелодии. Сложная последовательность проверок параметров из bank1 (PWM/калибровка). После проигрывания переходит к состоянию NEXT (0x09F6 → goto 0x0A79).

### Состояние 3 — NOTE_TABLE (0x09F7-0x0A61)
Проигрывание нот из таблицы. Итерирует через массив нот в RAM (по адресу 0x00A0 + index*2 и 0x0120 + index*2), вызывает pwm_compute для каждой ноты. Если все ноты проиграны — main_state = 2 (idle).

### Состояние 4 — BEEP_MELODY (0x0A63-0x0A68)
```
0x0A63: call beep_startup (0x06EB) ; проиграть стартовую мелодию (3 ноты)
0x0A66: main_state = 2              ; вернуться в idle
0x0A68: goto loop
```

## Функция adc_read_channel (0x0793-0x0799)

```
0x0793: call adc_config (0x05F2) ; конфигурация АЦП
0x0795: канал из bank3:0x27 → bank3:0x2E
0x0797: call adc_read_raw (0x05DA) ; чтение
0x0798: bcf ADCON0, 0   ; выключить АЦП
0x0799: return
```

## Функция adc_config (0x05F2-0x0600)
```
- 0x5D = 0xDA ; ADC config
- 0x5E = 0x05
- 0x5F = 0xAE
- 0x60 = 0x01
- ADCON1 (bank1:0x1E) = 0xF0 ; Right justified, Fosc/64, Vref=VDD
- ADCON0 (bank1:0x1D) = 0x00 ; cleared
- bsf ADCON0, 0  ; ADC ON (ADON)
```

## Функция adc_read_raw (0x05DA-0x05F1)
```
- ADCON0 &= 0x83 ; маска каналов (сохранить ADON и другие биты)
- bank3:0x2E << 2 → OR into ADCON0 (выбор канала)
- short_delay (для acquisition time)
- bsf ADCON0, 1 ; начать конверсию (GO/DONE)
- wait: btfss ADCON0, 1 → loop ; ждать завершения
- Результат: ADRESH:ADRESL → 0x71:0x70 (10-bit значение)
```

## Таблица данных (0x0A7E-0x0B30) — НОТНЫЕ ЧАСТОТЫ ДЛЯ МЕЛОДИЙ

**КРИТИЧЕСКАЯ НАХОДКА**: Это PWM частоты для мелодий, НЕ калибровка батареи!
pic_calib.h с linear ramp (4,5,6,7...) = МУСОР, подменявший реальные нотные данные.

16-битные значения (little-endian, по парам retlw):

```
Offset 0x0A7E: 0x0526  (1318)  ~ E6
Offset 0x0A80: 0x04DC  (1244)  ~ D#6
Offset 0x0A82: 0x0526  (1318)  ~ E6
Offset 0x0A84: 0x07B7  (1975)  ~ B6
Offset 0x0A86: 0x0496  (1174)  ~ D6
Offset 0x0A88: 0x0416  (1046)  ~ C6
Offset 0x0A8A: 0x06E0  (1760)  ~ A6
Offset 0x0A8C: 0x0000  (end marker)
```
Это частоты нот стартовой мелодии! Загружаются cmd 0x2D/0x2E в RAM 0x00A0/0x0120.

Остальные с 0x0A8C до 0x0ACD — нули (unused note slots).

```
Offset 0x0ACE: 0x00F0 (240) — 6 раз повторяется (0xACE-0xAD8)
Offset 0x0ADA: 0x01E0 (480)
Offset 0x0ADC: 0x05DC (1500)
Offset 0x0ADE-0x0B1D: все нули
Offset 0x0B1E: 0x0008
Offset 0x0B20-0x0B30: все нули
```

## Функция ram_init (0x0B32-0x0B66)

Копирование данных из Flash (через computed goto + retlw) в RAM:
```
1. Flash 0x87F6, 0x102 байт → RAM 0x0028 (bank0 рабочие переменные, включая adc_byte0/1 defaults!)
2. Flash 0x8A7E, 0x150 байт → RAM 0x00A0 (НОТНАЯ ТАБЛИЦА 1 — PWM частоты мелодий)
3. Flash 0x8ACE, 0x150 байт → RAM 0x0120 (НОТНАЯ ТАБЛИЦА 2 — повторы/длительности)
4. Flash 0x8B1E, 0x113 байт → RAM 0x002A (дополнительные настройки)
```
**ВАЖНО**: п.1 инициализирует 0x38=0x39, 0x39=0x3E — это ЗАВОДСКИЕ значения adc_byte0/1,
которые мы читали как "данные батареи". На самом деле это flash defaults, НЕ ADC!

Примечание: адреса Flash с retlw-таблицами — 0x0A7E и далее (с учётом page).

## I2C Slave init (0x0782-0x0792)
```
SSP1MSK = 0xFE      ; mask — все биты адреса значимы
SSP1CON1 = 0x36     ; SSPEN=1, SSPM=0110 (I2C slave 7-bit)
SSP1CON2 = 0x01     ; SEN=1 (clock stretching)
SSP1CON3 = 0x00
SSP1STAT = 0x00
SSP1ADD = 0x54       ; slave address = 0x54 = 0x2A << 1
bcf PIR1, SSP1IF
bsf PIE1, SSP1IF     ; enable SSP1 interrupt
bsf INTCON, PEIE     ; peripheral interrupts enable
```

## ПОЛНЫЙ ПРОТОКОЛ I2C

### Адресация
- Slave address: 0x2A (7-bit)
- Write address: 0x54
- Read address: 0x55

### Однобайтовые команды (Master Write: [0x54, cmd])
| Cmd | Действие |
|-----|----------|
| 0x40 | Включить бипер (buzzer_request = 1) |
| 0x41 | Выключить бипер (buzzer_active = 0, buzzer_request = 0) |
| 0x30 | Toggle LED + включить мигание |
| 0x31 | LED ON, мигание OFF, cmd_state=0 |
| 0x32 | LED OFF, мигание OFF, cmd_state=0 |
| 0x38 | Подтверждение чтения / сброс PORTE |
| 0x39 | Переинициализация I2C slave |

### Многобайтовые команды (Master Write: [0x54, cmd, data...])

| Cmd | Данные | Действие |
|-----|--------|----------|
| 0x2D | N*2 байт | Загрузка НОТНОЙ таблицы 1 (RAM 0x00A0+) — PWM частоты мелодий |
| 0x2E | N*2 байт | Загрузка НОТНОЙ таблицы 2 (RAM 0x0120+) — повторы/длительности |
| 0x2F | 2 байта | Управление main_state: 0x0001=NOTE_PLAY, 0x0002=IDLE, 0x0003=NOTE_TABLE |
| 0x33 | 2 байта | Установить размер таблицы (0x2A:0x2B) |
| 0x34 | 2 байта | Управление выходами PORTA: 0x01=A on, 0x02=B on, 0x03=all off |
| 0x35 | 2 байта | Установить счётчик повторов мелодии (0x50:0x51) |
| **0x36** | **0 байт** | **ЧТЕНИЕ АЦП БАТАРЕИ И ОБНОВЛЕНИЕ 0x38/0x39/0x3A** |
| 0x37 | 0 байт | Запрос чтения firmware version (→ 0x07) |

### Чтение данных (Master Read: [0x55] + N bytes)

**После команды 0x36:**
- Master Read 1-й байт → 0x38 (ADC high)
- Master Read 2-й байт → 0x39 (ADC low)
- Master Read 3-й байт → 0x3A (status flags)
- После 3-го байта → cmd_state сброс, read_byte_idx сброс

**После команды 0x37:**
- Master Read 1 байт → 0x07 (firmware version)

### Формат status byte (0x3A)
```
bit 0:     PORTE.0 (состояние GPIO входа — зарядка подключена?)
bits 1-2:  не используются (0)
bits 3-5:  charge_snapshot (0x37) << 3
bit 4:     buzzer_active (0x32) — перекрывает bit 4 из charge
bits 5-6:  charge_changed (0x35), charge_change_val (0x36)
bit 7:     расширение (обычно 0)
```

## ОТВЕТ НА КРИТИЧЕСКИЙ ВОПРОС

### Два уровня проблемы

**Уровень 1 (ПРОШИВКА PIC)**: Обновление АЦП данных происходит ТОЛЬКО по команде 0x36.
**Уровень 2 (ЖЕЛЕЗО MT7621)**: NEW SM0 manual mode read не поддерживает clock stretching PIC.

### Уровень 1: Почему данные 0x38/0x39/0x3A обновляются только один раз?

**ПОТОМУ ЧТО ОБНОВЛЕНИЕ ПРОИСХОДИТ ТОЛЬКО ПО КОМАНДЕ 0x36!**

Вот полная цепочка:

1. Переменные 0x38, 0x39, 0x3A обновляются **ТОЛЬКО** в обработчике команды 0x36 (адрес 0x00EC-0x013B).
2. Команда 0x36 — это **однобайтовая запись** от мастера: `[0x54, 0x36]`.
3. При получении 0x36, PIC немедленно:
   - Вызывает `adc_read_channel(0x0B)` — читает текущее значение АЦП
   - Сохраняет результат в 0x38 (high), 0x39 (low)
   - Формирует status byte в 0x3A
   - Устанавливает cmd_state = 0x0A
4. После этого мастер может читать 3 байта (`[0x55, read, read, read]`).

**В основном цикле (IDLE state) НЕТ АВТОМАТИЧЕСКОГО POLLING АЦП!**

Основной цикл просто крутится пустым циклом, ожидая прерываний. АЦП НЕ читается периодически. Нет никакого таймера, который бы обновлял 0x38/0x39/0x3A автоматически.

### Правильный протокол для получения live ADC данных

**Шаг 1**: Отправить команду 0x36:
```
I2C Write: [0x54] [0x36]
```

**Шаг 2**: Прочитать 3 байта:
```
I2C Read: [0x55] → byte0 (ADC high), byte1 (ADC low), byte2 (status)
```

**Шаг 3**: Повторять шаги 1-2 каждые N секунд для live обновления.

**КАЖДЫЙ РАЗ перед чтением нужно отправлять 0x36!** Без отправки 0x36 повторное чтение вернёт СТАРЫЕ данные, потому что:
- cmd_state сбрасывается в 0 после чтения 3 байт
- При cmd_state=0 чтение попадает в fallback (0x0345 → cmd_state != 0x0A && != 0x0B → goto 0x0350)
- Данные 0x38/0x39/0x3A не обновляются

### Стоковый протокол (предполагаемый)

Стоковое ядро делает цикл:
```
1. Write [0x54, 0x41]     — init/reset (buzzer off)
2. Write [0x54, 0x34, 0x00, 0x00] — buzzer mode off
3. Калибровочные таблицы через 0x33/0x2D/0x2E
4. Цикл polling:
   a. Write [0x54, 0x36]   — ЗАПРОС ЧТЕНИЯ АЦП (обновляет 0x38/0x39/0x3A!)
   b. Read  [0x55] × 3     — получить данные
   c. Задержка ~2-10 секунд
   d. goto a
```

### Почему наш lcd_drv получает статичные данные

Текущая реализация в lcd_drv делает:
```
1. Write {0x41}           — OK (init)
2. Write {0x34, 0, 0}     — OK (buzzer off)
3. Калибровочные таблицы   — OK
4. Write {0x2F, 0, 2}     — запрос чтения (2 байта), cmd_state=3 → 0x3C=2 (idle)
5. Read 8 байт            — ПРОБЛЕМА! cmd_state уже сброшен!
```

**Ошибка**: после `{0x2F, 0, 2}` PIC переходит в обработчик cmd_state=3, получает значение 0x0002, устанавливает 0x3C=2 и **cmd_state=0**. При последующем чтении cmd_state=0 — мастер не получает данные АЦП, а попадает в fallback.

### ИСПРАВЛЕНИЕ

**Нужно использовать команду 0x36 вместо 0x2F для polling!**

Правильная последовательность:
```c
// Инициализация (один раз)
i2c_write(0x2A, {0x41});           // reset
i2c_write(0x2A, {0x34, 0x00, 0x00}); // buzzer off

// Polling (каждые 10 секунд)
i2c_write(0x2A, {0x36});           // ЗАПРОСИТЬ ЧТЕНИЕ АЦП
delay_ms(5);                        // дать время на АЦП конверсию
uint8_t buf[3];
i2c_read(0x2A, buf, 3);            // прочитать 3 байта
// buf[0] = ADC high byte
// buf[1] = ADC low byte
// buf[2] = status (charge, buzzer, flags)
// ADC value = (buf[0] << 8) | buf[1]  — 10-bit ADC, 0-1023
```

### Уровень 2: Аппаратная проблема SM0

Даже если отправить правильную команду 0x36, **NEW SM0 manual mode read** на ядре 6.12.74 не работает для чтения PIC:

1. **PIC использует clock stretching** (SSP1CON2.SEN=1, SSP1CON1.CKP — бит 4).
   В каждом READ handler (0x0313) PIC делает:
   ```
   SSP1BUF = data;        // загрузить байт
   bsf SSP1CON1, CKP;     // release clock (отпустить SCL)
   bcf PIR1, SSP1IF;       // clear interrupt
   poll: btfss SSPSTAT, BF; goto poll;  // ждать пока мастер заберёт
   ```
   PIC ЗАДЕРЖИВАЕТ SCL LOW пока не загрузит следующий байт в SSPBUF!

2. **NEW SM0 manual mode** читает данные из D0/D1 регистров (0x950/0x954).
   Эти регистры содержат **snapshot** данных с шины, но НЕ поддерживают
   побайтовый handshake с clock stretching.

3. **OLD SM0 auto mode** (SM0_START/SM0_STATUS/SM0_DATAIN at 0x904-0x910)
   был спроектирован для побайтового I2C с clock stretching.
   На стоковом ядре 3.10 он работал. На ядре 6.12 — возвращает 0xFF.

4. **Корневая причина**: SM0_CFG (0x900) на MT7621 eco:3 является READ-ONLY.
   Стоковое ядро писало SM0_CFG=0xFA сразу после RSTCTRL reset — возможно,
   это работало на ранних ревизиях кремния. На eco:3 — не записывается.

### Возможные решения

**A. Использовать OLD SM0 auto mode с правильной инициализацией:**
   - Записать SM0_CTL1=0x90640042 (как стоковое ядро)
   - Отправить 0x36 через auto write
   - Задержка 5мс (АЦП конверсия ~15us, но PIC clock stretching)
   - Читать через SM0_START/SM0_DATAIN
   - ПРОБЛЕМА: SM0_DATAIN возвращает FF на eco:3

**B. Bit-bang I2C через GPIO** (как LCD дисплей):
   - GPIO 3 (SDA), GPIO 4 (SCL) — те же пины I2C bus 0
   - Вручную отпускать SCL после каждого байта
   - ПОДДЕРЖИВАЕТ clock stretching (проверять SDA/SCL уровни)
   - ПРОБЛЕМА: конфликт с i2c-mt7621 driver и touch thread

**C. Патч i2c-mt7621 для поддержки clock stretching:**
   - Добавить задержку/retry при NAK
   - Или переключить на auto mode для PIC операций
   - САМЫЙ ПРАВИЛЬНЫЙ путь

**D. Write 0x36 + Read из NEW SM0 с правильным таймингом:**
   - Текущие тесты (23 марта) показали что `{0x36}` write + NEW SM0 read
     возвращает `55 00 00` (echo адреса) — PIC не успевает загрузить SSPBUF
   - Нужно: write 0x36, STOP, delay, START read 0x55, delay (clock stretch),
     read 3 bytes — но NEW SM0 не умеет ждать clock stretch

**E. САМЫЙ ПЕРСПЕКТИВНЫЙ: Двухэтапная транзакция через OLD SM0 write + NEW SM0 read:**
   - Шаг 1: OLD SM0 auto write `{0x36}` (уже работает!)
   - Шаг 2: Задержка 5-10мс (PIC делает АЦП, устанавливает cmd_state=0x0A)
   - Шаг 3: NEW SM0 manual read 3 байта
   - ГИПОТЕЗА: после 0x36 PIC загружает ПЕРВЫЙ байт (0x38) в SSP1BUF и
     отпускает CKP. NEW SM0 read должен увидеть этот байт в D0.
   - Но после первого байта PIC удерживает SCL — NEW SM0 может не получить 2-й и 3-й.
   - НУЖНО ТЕСТИРОВАТЬ! Возможно с кратковременным переключением CKP.

### Важные заметки по прошивке

1. **Команда 0x36 не принимает дополнительных данных** — это однобайтовая команда. В ISR для 0x36, сразу вызывается `adc_read_channel` из контекста прерывания.

2. **АЦП канал AN11** (0x0B) — это батарейный вход.

3. **Чтение 3 байт строго после 0x36** — если прочитать меньше 3, read_byte_idx не сбросится и следующее чтение будет из середины.

4. **Если прочитать больше 3** — read_byte_idx обнулится после 3-го, cmd_state станет 0, и 4-й байт будет из fallback.

5. **Timer1** отслеживает состояние зарядки (PORTE bit 3) и мигание LED, но **НЕ запускает АЦП чтение**.

6. **Команда 0x2F** устанавливает main_state через 0x3C, что влияет на бипер (мелодии), а НЕ на чтение АЦП.

7. **PIC clock stretching — ключевой фактор**. PIC ЗАДЕРЖИВАЕТ SCL LOW между байтами чтения, пока не загрузит следующий байт в SSP1BUF. Master ОБЯЗАН ждать отпускания SCL. NEW SM0 manual mode может не поддерживать это.

8. **Команда 0x36 вызывает ADC ВНУТРИ ISR** — конверсия ~15us при Fosc/64 на PIC. Это значит что после записи 0x36 PIC БЫСТРО обновляет данные (ещё до завершения I2C транзакции). К моменту I2C read данные уже готовы.

## Полная карта функций

| Адрес | Имя | Описание |
|-------|-----|----------|
| 0x0000 | reset_vector | movlp 0x08; goto 0x0816 |
| 0x0004 | isr_entry | Сохранение контекста, проверка SSP1IF/TMR1IF |
| 0x0016 | isr_ssp1_start | Обработка I2C interrupt |
| 0x0058 | isr_addr_write | Обработка I2C address+write |
| 0x006E | isr_data_write | Обработка I2C data+write (команды) |
| 0x0313 | isr_master_read | Отправка данных мастеру |
| 0x0351 | isr_nack | Обработка NACK |
| 0x0356 | isr_dispatch | Диспетчер по isr_state |
| 0x0363 | isr_exit_ckp | CKP release + goto exit |
| 0x0366 | isr_timer1 | Обработка Timer1 interrupt |
| 0x039A | isr_pwm_clc | Обработка PWM/CLC прерывания |
| 0x03E3 | isr_restore | Восстановление контекста + retfie |
| 0x03F3 | multiply_32x32 | Умножение 32x32 → 64 бит |
| 0x0429 | delay_loop | Программная задержка |
| 0x043B | short_delay | Фиксированная задержка ~29 циклов |
| 0x0440 | divide_32x32 | Деление 32/32 |
| 0x05B7 | divide_16x16 | Деление 16/16 |
| 0x05D3 | init_adc_scale | Загрузка константы 16000 (0x3E80) |
| 0x05DA | adc_read_raw | Чтение одного АЦП канала |
| 0x05F2 | adc_config | Конфигурация АЦП модуля |
| 0x0601 | pwm_compute | Вычисление PWM параметров |
| 0x06E2 | medium_delay | Задержка ~450 циклов |
| 0x06EB | beep_startup | Стартовая мелодия (3 ноты) |
| 0x072C | calibrate_wait | Ожидание калибровки |
| 0x0745 | multiply_16x16 | Умножение 16x16 |
| 0x076F | timer1_config | Конфигурация Timer1 |
| 0x077A | interrupts_enable | Включение GIE/PEIE/CLC |
| 0x0782 | i2c_slave_init | Инициализация I2C slave |
| 0x0793 | adc_read_channel | Чтение АЦП канала N |
| 0x079A | gpio_output_pwm | Вывод PWM на GPIO |
| 0x07D5 | gpio_port_init | Инициализация GPIO порта |
| 0x0800 | peripheral_init | Конфиг ANSEL/TRIS/OSCCON |
| 0x080D | memcpy | FSR0→FSR1 блочное копирование |
| 0x0816 | main | Точка входа |
| 0x0843 | main_note_play | Проигрывание ноты (state 1) |
| 0x0A62 | main_idle | Пустой цикл ожидания (state 2) |
| 0x0A63 | main_beep_melody | Стартовая мелодия (state 4) |
| 0x0A69 | main_dispatcher | Диспетчер main loop по main_state |
| 0x09F7 | main_note_table | Проигрывание таблицы нот (state 3) |
| 0x0B32 | ram_init | Инициализация RAM из flash |

## Детальный анализ Clock Stretching в PIC ISR

При master read (isr_state=3, handler 0x0313), PIC делает:

```
; Обработка одного байта (cmd_state=0x0A):
; Пример: read_byte_idx=0
0x031C: movf 0x38, W          ; ADC high byte → W
0x031D: movlb 4
0x031E: movwf SSP1BUF         ; → SSP1BUF = adc_byte0

; (общий код для всех 3 байтов)
0x0334: movlb 4
0x0335: bsf SSP1CON1, CKP     ; RELEASE CLOCK — SCL goes HIGH
                                ; Master теперь может clock-out байт
0x0336: movlb 0
0x0337: bcf PIR1, SSP1IF      ; Clear interrupt flag

; --- Ждём пока master прочитает байт (BF → 0) ---
0x0338: movlb 4
0x0339: btfss SSP1STAT, BF    ; BF=0? (buffer empty = byte clocked out)
0x033A: goto 0x033C           ; да → выйти из цикла
0x033B: goto 0x0338           ; нет → ждать (BF ещё =1)

; Master прочитал байт:
0x033C: movlb 0
0x033D: incf 0x2C             ; read_byte_idx++
0x033E: movlw 3
0x033F: subwf 0x2C, W         ; read_byte_idx - 3
0x0340: btfss STATUS, C       ; >= 3?
0x0341: goto 0x0344           ; нет → exit (ждём следующий interrupt для следующего байта)
0x0342: clrf 0x2C             ; read_byte_idx = 0
0x0343: clrf 0x28             ; cmd_state = 0 (все 3 байта отданы)
0x0344: goto 0x0350           ; → ISR exit
```

**ВАЖНОЕ УТОЧНЕНИЕ**: Этот ISR обрабатывает ОДИН байт за раз!

Последовательность для 3 байт:
1. Master отправляет READ address (0x55) → SSP1IF → ISR определяет isr_state=3
2. ISR: SSP1BUF = byte0 (0x38), CKP release, ждёт BF=0, exit
3. Master clocks out byte0, ACK → SSP1IF → ISR снова
4. ISR: SSP1BUF = byte1 (0x39), CKP release, ждёт BF=0, exit
5. Master clocks out byte1, ACK → SSP1IF → ISR снова
6. ISR: SSP1BUF = byte2 (0x3A), CKP release, ждёт BF=0, exit
7. Master clocks out byte2, NACK + STOP

**Критический момент clock stretching:**
- ПЕРЕД `bsf CKP` PIC удерживает SCL=LOW. Время в ISR до CKP release:
  - Первый вход: ~30 инструкций (контекст save + dispatch + byte load) = ~30us at 4MHz
  - Повторные: ~30-40us
- МЕЖДУ `bsf CKP` и `btfss BF`: PIC ждёт в poll loop — это НЕ clock stretch,
  PIC уже отпустил SCL
- Clock stretching = период между SSP1IF и bsf CKP = ~30-40us

**NEW SM0 manual mode** (i2c-mt7621): генерирует SCL через программное управление
регистрами SM0CTL0. Если SCL линия удерживается LOW (PIC stretch), мастер MT7621
МОЖЕТ не обнаружить это — зависит от реализации i2c-mt7621.

Проверка в i2c-mt7621.c: драйвер использует `SM0CTL0 |= READ_CMD`, затем
`poll(SM0CTL1, READY)`. Если готовность привязана к SCL release, то clock stretching
может работать. Если нет — мастер отправит clock pulses без ожидания и прочитает мусор.

**Стоковый SM0 auto mode** использовал SM0_START с аппаратным контролем SCL —
гарантированная поддержка clock stretching.

## Формат ответа при текущем чтении через NEW SM0

**Почему первый байт = 0x55 (адрес echo)?**

При ADDRESS_READ (isr_state=3, handler 0x0058):
```
0x0058: movf SSP1BUF → 0x3D  ; прочитать slave address из буфера
0x005C: if 0x3D == 0x54       ; это write addr?
0x0060-0x0068: delay ~750 cycles ; да → задержка
```
НО ЭТО HANDLER ДЛЯ isr_state=1 (ADDRESS+WRITE)! Для ADDRESS+READ (isr_state=3) dispatch идёт на 0x0313.

На самом деле при isr_state=3 (address+read):
```
0x0313: Сразу переходит к master read handler
```
PIC НЕ читает SSP1BUF при addr+read — буфер содержит slave address byte, но PIC его не забирает. Вместо этого PIC загружает данные в SSP1BUF (если cmd_state подходит).

**Но откуда 0x55 в данных?** — NEW SM0 manual mode читает raw bytes с шины. Первый байт на шине при I2C read = адрес slave + R/W бит (0x2A<<1|1 = 0x55). NEW SM0 захватывает ЭТО как первый "data byte", хотя это не данные, а адрес. Нормальный I2C мастер фильтрует адресный байт.

Когда NEW SM0 читает 8 байт без отправки 0x36 (как сейчас в lcd_drv):
```
Byte 0: 0x55 — I2C read address on the bus (captured by NEW SM0)
Byte 1-7: данные из PIC SSP1BUF (cmd_state=0 → fallback → не загружены → 0x00/old data)
```

Если NEW SM0 отправит 0x36, затем прочитает:
```
Byte 0: 0x55 — addr byte on bus
Byte 1: PIC должен загрузить 0x38 — НО нужен clock stretching handshake
         → если stretching не работает → NEW SM0 читает мусор или 0xFF
```

## Гипотеза: Почему boot init данные видны

При загрузке, init_sequence отправляет `{0x2F, 0, 2}` через OLD SM0 auto write.
Это устанавливает cmd_state=3, затем PIC обрабатывает данные 0x00, 0x02:
- 0x00 → first byte of 2-byte value (cmd_state=3, sub=0 → sub=1)
- 0x02 → second byte → value = 0x0002 → main_state=2 (idle), cmd_state=0

Но PIC НЕ делает ADC read при команде 0x2F! Значит boot данные `39 3E` — это значения
из ram_init (flash → RAM). Таблица в flash (0x0B32) инициализирует RAM 0x28+ из данных
в flash 0x87F6. Значения 0x38=0x39, 0x39=0x3E — это ЗАВОДСКИЕ ЗНАЧЕНИЯ из flash, НЕ
реальные измерения АЦП!

**Вывод**: `55 00 00 00 39 3E 40 E6` — это:
- 0x55 = addr echo
- 00 00 00 = неинициализированные / нулевые переменные
- 0x39 = заводское значение adc_byte0 (из flash ram_init)
- 0x3E = заводское значение adc_byte1 (из flash ram_init)
- 0x40/0x01 = adc_byte2 status (Timer1 ISR обновляет charge_state → 0x37)
- 0xE6/0xC6 = возможно из других переменных (offset в 8-byte read)

Байт 6 (0x40 vs 0x01) МЕНЯЕТСЯ потому что Timer1 ISR обновляет charge_snapshot (0x37)
независимо от ADC — через проверку PORTE bit 3 (charging status GPIO).

## Итоговые выводы

### Результаты анализа прошивки:

1. **Данные АЦП обновляются ТОЛЬКО по команде 0x36** — нет автоматического polling. Timer1 ISR отслеживает только GPIO зарядки, не АЦП.

2. **Правильный протокол**: Write `{0x36}` → Read 3 bytes → повторять каждые N секунд. Стоковый протокол (из IDA): `{0x2F, 0x00, 0x01}` + 500ms delay + OLD SM0 read — это ДРУГАЯ команда.

3. **Текущий lcd_drv использует `{0x2F, 0, 2}`** — это команда установки main_state (мелодии), НЕ чтение АЦП. При значении 0x0002 устанавливает main_state=2 (idle) и cmd_state=0 — read handler не отдаёт данные.

4. **Исправление на уровне прошивки**: заменить `{0x2F, 0, 2}` на `{0x36}` и читать 3 байта (не 8).

5. **НО есть аппаратная проблема**: NEW SM0 manual mode может не поддерживать clock stretching PIC. Даже с правильной командой 0x36, master read может не получить корректные данные.

6. **Данные `39 3E` — заводские значения из flash**, а НЕ реальные ADC. ram_init (0x0B32) копирует flash → RAM при старте. Byte 6 (0x40/0x01) меняется потому что Timer1 обновляет charge_snapshot (PORTE bit 3) — это GPIO, не ADC.

7. **Стоковый протокол из IDA использовал `{0x2F, 0x00, 0x01}`** — по анализу PIC, это устанавливает cmd_state=3, data=0x0001, что в handler cmd3 проверяет значение: 0x0001 → 0x3C=1 (main_state=1, NOTE_PLAY). Не связано с ADC! Возможно, стоковое ядро использовало ДРУГУЮ последовательность для ADC, которую IDA не показал.

8. **Команда 0x34 с данными 0x0001/0x0002/0x0003** управляет PORTA (выходы), НЕ бипером напрямую. Это могут быть: зарядка вкл/выкл, внешний buzzer enable, или другая периферия.

9. **Мелодии**: cmd 0x33 (table size) + 0x2D/0x2E (note tables) + 0x35 (repeat count) + main_state. Стартовая мелодия (0x06EB): 3 ноты с частотами 3500/3500/3500 Hz (0x0DAC/0x0DAC/0x0DAC через вычисление).

### Необходимые действия (обновлено 2026-03-24):

1. **ТЕСТ**: Отправить `{0x36}` через OLD SM0 auto write, delay 10ms, затем NEW SM0 read 4 байта (1 addr echo + 3 data). Если clock stretching работает в NEW SM0, данные будут свежими.

2. **SM0CTL0 разница найдена и протестирована**:
```
Наш SM0CTL0:   0x01F3800F — SCL_STRETCH=1, ODRAIN=0, clk_div=0x1F3
Stock SM0CTL0: 0x8064800E — SCL_STRETCH=0, ODRAIN=1, clk_div=0x064
```
Stock: SCL_STRETCH **ВЫКЛЮЧЕН**! Наш: **ВКЛЮЧЁН**!
Тест со stock CTL0 в lcd_drv: данные изменились с `39 3e 43 ee` на `ff ff ff ff` — другое поведение, но clock stretching всё равно не работает.

3. **GPIO bit-bang I2C** на GPIO 3/4 из kernel module с ручной проверкой SCL уровня — следующий вариант если SM0 не заработает.

4. **БЕЗОПАСНЫЙ ТЕСТ**: Отправить `{0x37}` (firmware version) + прочитать 1 байт. Ответ должен быть 0x07. Это проверит работает ли вообще read с cmd_state != 0.

5. **RSTCTRL I2C reset**: протестирован — БЕЗОПАСЕН для MT7530 LAN, но бесполезен (SM0_CFG read-only на eco:3).

6. **Отдельный pic_battery.ko**: ПРОВАЛИЛСЯ — SM0 sharing слишком хрупкий. Только lcd_drv с PIC внутри работает стабильно.

---

## GPIO маппинг PIC16LF1509 (подтверждённый)

### PORTE
| Бит | I/O | Назначение | Доказательство |
|-----|-----|-----------|----------------|
| 0 | INPUT | **Зарядка подключена** | Читается в cmd 0x36 → ADC status byte bit 0 |
| 3 | INPUT | **Состояние заряда** | TMR1 ISR мониторит каждые ~125ms → charge_state |
| 4 | OUTPUT | **LED индикатор (ACTIVE LOW!)** | bsf=OFF, bcf=ON. cmd 0x32=ON, 0x31=OFF, 0x30=blink. Подтверждено! |
| 5 | OUTPUT | **Сигнал выключения** | LOW = кнопка питания зажата → мелодия + LED off + shutdown |

### PORTA
| Бит | I/O | Назначение | Доказательство |
|-----|-----|-----------|----------------|
| 0 | OUTPUT | **Выход A** | cmd 0x34 data=0x01: set. Init: set |
| 1 | OUTPUT | **Выход B** | cmd 0x34 data=0x02: set. Init: clear |

Вероятно: управление BQ24133 зарядкой (CE/EN пины) или внешний relay.

### PORTC
| Бит | I/O | Назначение | Доказательство |
|-----|-----|-----------|----------------|
| 0 | OUTPUT | **Бипер PWM** | gpio_output_pwm(0x0C, bit0) генерирует звук |

### ADC
| Канал | Пин | Назначение |
|-------|-----|-----------|
| AN11 | RB5? | **Напряжение батареи** | cmd 0x36 → adc_read_channel(0x0B) |

### Поведение при включении/выключении
1. **Включение**: PIC init → PORTE.5=LOW (clear) → PORTE.4=HIGH (LED ON) → I2C slave ready
2. **Выключение** (кнопка): PORTE.5 detected LOW by ISR → мелодия 3 ноты → LED OFF → shutdown
3. **Init от MT7621** (`{0x41}`): сброс buzzer/state. Если PORTE.5 ещё LOW → ISR запускает мелодию. Стоковое ядро обходило это быстрой отправкой `{0x2F,0,2}` (main_state=2=idle).
