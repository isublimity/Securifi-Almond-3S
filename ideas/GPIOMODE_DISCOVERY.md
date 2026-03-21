# КРИТИЧЕСКОЕ ОТКРЫТИЕ: GPIOMODE = 0x95A8

## Стоковый U-Boot устанавливает GPIOMODE = 0x95A8 ДО передачи управления ядру!

### Найдено в MTD1_BOO.BIN (стоковый U-Boot) через IDA:
```
0x1460: li  $v0, 0x95A8          ← GPIOMODE!
0x1474: sw  $v0, 0xBE000060      ← записывает в GPIOMODE
```

### Значения GPIOMODE:
| Прошивка | GPIOMODE | I2C пины (bits 2-3) |
|----------|----------|---------------------|
| **Стоковый U-Boot** | **0x95A8** | I2C→GPIO mode (bit 2=1) |
| **OpenWrt (текущий)** | **0x48580** | I2C в default (peripheral) mode |
| **Стоковое ядро 3.10** | 0x9580 | I2C→GPIO (ядро потом переключает на I2C) |

### Разница в битах:
```
0x95A8 = 1001 0101 1010 1000
  bit 3 (I2C_MODE): 1 = GPIO mode
  bit 5 (UART1_MODE): 1 = GPIO mode

0x48580 = 0100 1000 0101 1000 0000
  bit 3 (I2C_MODE): 0 = I2C peripheral mode
```

### Гипотеза:
Стоковый U-Boot ставит GPIOMODE с I2C→GPIO (для LCD bit-bang на тех же пинах?).
Потом стоковое ядро инициализирует SM0 I2C, который переключает пины обратно.
Но **начальное состояние SM0** после GPIO mode → I2C mode может отличаться
от состояния когда SM0 инициализируется сразу в I2C mode (как на OpenWrt).

### Следующий шаг:
Попробовать установить GPIOMODE=0x95A8 (или хотя бы bit 2=1) перед i2c-mt7621 init:
1. В lcd_drv.ko ДО SX8650 init
2. Или в DTS через pinctrl
3. Или в rc.local через devmem

### Полный реверс стокового U-Boot (228 функций):
- **GPIOMODE = 0x95A8** при LCD init (I2C→GPIO mode)
- **RSTCTRL**: 4 места — PCIe (bit 22), FE (bit 2), UART (bits 19-21). **НЕТ I2C reset (bit 16)!**
- **SM0 I2C**: НОЛЬ обращений! U-Boot не трогает SM0 вообще
- **LCD**: массивный GPIO 0x600/0x620 bit-bang (лого)
- **SPI flash**: 0xB00-0xB30 (загрузка ядра)
- **Ethernet FE**: 0xBE10xxxx (НЕ SM0!)

### Проверено и НЕ помогло:
- device_reset() → ручной RSTCTRL toggle (как стоковое ядро) = bus noise
- SM0 AUTO mode (CFG2=1) = bus noise
- i2c-ralink protocol (OLD regs) = bus noise (+ LAN сломался)
- SM0 NEW manual mode byte-by-byte = bus noise
- i2c_transfer через kernel = bus noise
- bit-bang GPIO = bus noise
- Все комбинации write/read протоколов = bus noise

### Дополнительные находки из стокового U-Boot:
- **НОЛЬ обращений к SM0 I2C** (0xBE000900-0xBE000960) — U-Boot не трогает I2C
- **Массивный LCD init**: GPIO 0x600/0x620 bit-bang (лого Almond)
- **RSTCTRL**: 0x034 toggle в нескольких местах (PCIe, UART)
- **SPI**: 0xB00-0xB30 (flash read)
- **0xBE100xxx**: Ethernet FE init (НЕ SM0!)
