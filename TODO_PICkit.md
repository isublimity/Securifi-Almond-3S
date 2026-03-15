# TODO: PIC16LF1509 Battery — Варианты решения

## Вариант A: Перехват калибровки из стоковой прошивки (БЕЗ оборудования)

### Идея
Стоковое ядро 3.10.14 отправляет PIC 800 байт калибровки при загрузке. Мы можем:
1. Загрузить стоковую прошивку
2. Дамп калибровочных данных из kernel memory
3. Захардкодить эти 800 байт в нашем lcd_drv.ko
4. При загрузке OpenWrt — отправить калибровку PIC, затем читать батарею

### План действий

```
1. Прошить стоковую прошивку через U-Boot Recovery
   - У нас есть BACKUP/MTD0_ALL.BIN (полный дамп 64 МБ)
   - Или оригинальный sysupgrade если найдём

2. Загрузиться в стоковую ОС
   - Telnet: 10.10.10.254:23, admin/admin (после factory reset)
   - Или SSH если доступен

3. Найти модуль AlmondPic2 в памяти
   - cat /proc/kallsyms | grep -i pic
   - Найти адрес глобальных данных (аналог 0x8159A044)

4. Дамп калибровочных данных
   - devmem или dd if=/dev/mem (если доступен)
   - Или написать kernel module для дампа
   - Нужно 800 байт: 400 по адресу ~0x8159A044 + 400 по ~0x8159A1D4

5. Сохранить данные на USB или через сеть

6. Прошить обратно OpenWrt

7. Добавить в lcd_drv.ko:
   - Массив static const u8 pic_calib1[401] = { 0x03, ...данные... };
   - Массив static const u8 pic_calib2[401] = { 0x2E, ...данные... };
   - При загрузке: pic_i2c_write(pic_calib1, 401); pic_i2c_write(pic_calib2, 401);
   - Затем: pic_i2c_write({0x2F, 0x00, 0x02}, 3); pic_i2c_read(buf, 17);
```

### Риски
- Калибровка может быть **device-specific** (уникальна для каждого экземпляра)
- На стоковой прошивке может не быть devmem / /dev/mem
- Адреса kernel memory могут отличаться от IDA (KASLR маловероятен на 3.10.14, но ASLR возможен)
- I2C write 401 байт может не работать через Linux i2c_transfer (в стоке используется palmbus напрямую)

### Плюсы
- Не нужно оборудование
- Если калибровка универсальна — решение одноразовое

---

## Вариант B: Свая прошивка PIC (PICkit)

### Идея
Написать минимальную прошивку для PIC16LF1509 которая читает АЦП батареи и отдаёт по I2C.

### Что купить
| Что | Цена | Где |
|-----|------|-----|
| PICkit 2 клон | ~500₽ | AliExpress "PICkit 2 programmer" |
| Провода Dupont F-F | ~100₽ | Любой магазин электроники |
| **Итого** | **~600₽** | |

PICkit 3 тоже подойдёт но дороже. PICkit 2 клоны работают отлично с PICKitPlus и pk2cmd.

### ICSP подключение на плате Almond 3S

PIC16LF1509 ICSP пины (из даташита):
| ICSP | PIC пин | Функция |
|------|---------|---------|
| VPP/MCLR | RA3 (pin 4) | Program voltage / Master Clear |
| PGD | RB7 (pin 10) | Program Data |
| PGC | RB6 (pin 11) | Program Clock |
| VDD | VDD (pin 1) | Power 3.3V |
| VSS | VSS (pin 20) | Ground |

**ВАЖНО**: На плате RC5 подключён к MCLR через R507. Нужно найти эти пины на плате и припаять/подключить провода.

### Прошивка (SDCC)

```c
// pic_battery.c — Простая прошивка PIC16LF1509
// I2C slave addr 0x2A, отдаёт напряжение батареи
//
// Компиляция: sdcc --use-non-free -mpic14 -p16lf1509 pic_battery.c
// Прошивка: pk2cmd -PPIC16LF1509 -F pic_battery.hex -M

#include <pic16lf1509.h>

// I2C буфер ответа
volatile unsigned char i2c_buf[8];
volatile unsigned char i2c_idx;

void interrupt isr(void) {
    if (PIR1bits.SSP1IF) {
        unsigned char temp = SSP1BUF;  // read to clear BF

        if (!SSP1STATbits.D_NOT_A) {
            // Address match
            i2c_idx = 0;
        }

        if (SSP1STATbits.R_NOT_W) {
            // Master reads — send battery data
            SSP1BUF = i2c_buf[i2c_idx++];
            if (i2c_idx >= 8) i2c_idx = 0;
        }

        SSP1CON1bits.CKP = 1;  // release clock
        PIR1bits.SSP1IF = 0;
    }
}

void adc_read(void) {
    unsigned int raw;

    ADCON0bits.GO = 1;
    while (ADCON0bits.GO);

    raw = (ADRESH << 8) | ADRESL;

    // raw = 10-bit ADC, Vref=VDD=3.3V
    // Battery через делитель: Vbat = raw * 3.3 / 1024 * divider_ratio
    // Пока отдаём raw
    i2c_buf[0] = 0xBA;          // magic: "BAttery"
    i2c_buf[1] = ADRESH;        // ADC high
    i2c_buf[2] = ADRESL;        // ADC low
    i2c_buf[3] = raw >> 2;      // 8-bit approximation
    i2c_buf[4] = 0;             // reserved
    i2c_buf[5] = 0;
    i2c_buf[6] = 0;
    i2c_buf[7] = 0;
}

void main(void) {
    // Oscillator: 16 MHz internal
    OSCCONbits.IRCF3 = 1;
    OSCCONbits.IRCF2 = 1;
    OSCCONbits.IRCF1 = 1;
    OSCCONbits.IRCF0 = 1;

    // I2C Slave: addr 0x2A, 7-bit mode
    TRISBbits.TRISB4 = 1;  // SDA = input
    TRISBbits.TRISB6 = 1;  // SCL = input
    SSP1ADD = 0x54;         // slave addr = 0x2A << 1
    SSP1MSK = 0xFE;
    SSP1CON1 = 0x36;        // SSPEN + I2C slave 7-bit
    SSP1CON2bits.SEN = 1;   // clock stretching
    SSP1CON3bits.BOEN = 1;  // buffer overwrite enable

    // ADC: channel AN0 (RA0), right-justified, Vref=VDD
    ANSELAbits.ANSA0 = 1;  // RA0 = analog
    TRISAbits.TRISA0 = 1;  // RA0 = input
    ADCON0 = 0x01;          // AN0, ADON
    ADCON1 = 0xA0;          // right-justified, FOSC/32, VDD ref

    // Interrupts
    PIE1bits.SSP1IE = 1;    // SSP interrupt enable
    INTCONbits.PEIE = 1;    // peripheral interrupts
    INTCONbits.GIE = 1;     // global interrupts

    // Init buffer
    i2c_buf[0] = 0xBA;

    while (1) {
        adc_read();
        __delay_ms(500);
    }
}
```

### После прошивки

На стороне MT7621 (lcd_drv.ko) — простой read:
```c
// PIC теперь отвечает: BA [ADRESH] [ADRESL] [8bit] 00 00 00 00
ret = pic_i2c_read(buf, 8);
if (buf[0] == 0xBA) {
    uint16_t raw_adc = (buf[1] << 8) | buf[2];
    // Vbat = raw_adc * 3.3 / 1024 * divider_ratio
}
```

### Плюсы
- Полный контроль над PIC
- Простой и понятный протокол
- Работает без калибровки
- Можно добавить: температуру, статус зарядки, shutdown команду

### Минусы
- Нужен PICkit (~600₽)
- Нужно найти ICSP пины на плате
- Оригинальная прошивка PIC будет стёрта (backup невозможен если CP=ON)

---

## Вариант C: Внешний АЦП / делитель напряжения

### Идея
Подключить батарею через резистивный делитель к свободному GPIO MT7621 и читать напрямую (через внешний I2C АЦП типа ADS1115 или MCP3008).

### Сложность
Высокая — нужна пайка, внешний чип, нет свободного АЦП на MT7621.

---

## Рекомендация

**Вариант A** (стоковая прошивка) — попробовать первым, бесплатно.
**Вариант B** (PICkit) — если A не сработает, заказать PICkit 2 на AliExpress.
