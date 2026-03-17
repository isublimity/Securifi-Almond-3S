# Touch + LAN Coexistence Plan

## ПРИЧИНА НАЙДЕНА (2026-03-17)

**lcd_drv.ko убивал LAN двумя способами:**

### 1. GPIOMODE overwrite (бит 12 = MDIO→GPIO)
```c
gw(GPIOMODE_OFF, 0x95A8);  // Значение из стокового U-Boot
```
Бит 12 (MDIO shift) = 1 → GPIO 20-21 переключаются из MDIO в GPIO mode → MT7530 теряет MDIO связь → LAN мёртв.

На стоковой прошивке (ядро 3.10) MT7530 управлялся через прямые регистры, не через Linux MDIO bus. Поэтому 0x95A8 работал. На OpenWrt DSA — MDIO обязателен.

### 2. GPIO DIR full overwrite
```c
gw(GPIO_DIR_OFF, shadow_dir);  // shadow_dir содержит ТОЛЬКО LCD биты
```
Все остальные GPIO (0-12, 19-21, 28-31 кроме LCD) принудительно ставились в DIR=0 (input), ломая их конфигурацию.

## Исправление (реализовано)

### DTS: pinmux через pinctrl
```dts
&state_default {
    lcd_jtag {
        groups = "jtag";     /* GPIO 13-17: D0, WRX, RST, CSX, DCX */
        function = "gpio";
    };
    lcd_wdt {
        groups = "wdt";      /* GPIO 18: D1 */
        function = "gpio";
    };
    lcd_rgmii2 {
        groups = "rgmii2";   /* GPIO 22-33: D2-D7, backlight */
        function = "gpio";
    };
};
```
Pinctrl делает правильный read-modify-write GPIOMODE, не затрагивая MDIO.

### lcd_drv.c: маскированные записи DIR
```c
#define LCD_PIN_MASK (BIT_D0|...|BIT_D7|BIT_WRX|BIT_RST|BIT_CSX|BIT_DCX|BIT_BL)
static u32 base_dir;  // non-LCD DIR bits

static inline void gw_dir(u32 lcd_bits) {
    gw(GPIO_DIR_OFF, base_dir | (lcd_bits & LCD_PIN_MASK));
}
```
- Убран `gw(GPIOMODE_OFF, 0x95A8)` — pinctrl обрабатывает
- Все `gw(GPIO_DIR_OFF, ...)` заменены на `gw_dir(...)` с маской
- `base_dir` обновляется при каждом lcd_flush_fb()

## Touch через Linux I2C

SX8650 отвечает через Linux I2C (i2c_transfer/I2C_RDWR):
- Scan видит 0x48
- SELECT(X) + read = работает (ch=7, val=FFF = idle)
- SELECT(Y) + read = работает

Текущий lcd_drv.ko использует palmbus direct для touch (SM0_CTL1=0x90644042).
Это конфликтует с i2c-mt7621 (SM0_CTL1=0x8064800E).

### Следующий шаг: перевести touch на Linux I2C
```c
// Вместо palmbus direct:
struct i2c_msg msgs[2] = {
    { .addr = 0x48, .flags = 0, .len = 1, .buf = &cmd_select_x },
    { .addr = 0x48, .flags = I2C_M_RD, .len = 2, .buf = xy_data },
};
i2c_transfer(adapter, msgs, 2);
```

## PIC16 Battery

- Scan видит 0x2A
- Read через Linux I2C: отвечает `AA 54 A8 50 A0 40 80` (тестовый паттерн)
- Write через Linux I2C: чередующийся ACK/NACK на каждую транзакцию
- Калибровка 401 байт через I2C_RDWR: проходит (ACK), но PIC не обрабатывает
- PIC вероятно требует palmbus direct протокол для calibration write
- Для тестирования нужен CONFIG_DEVMEM=y или kernel module с доступом к palmbus

## GPIOMODE битовая карта MT7621

| Биты | Группа | GPIO | Функция по умолчанию |
|------|--------|------|---------------------|
| 1 | UART1 | 1-2 | UART1 |
| 2 | I2C | 3-4 | I2C |
| [4:3] | UART3 | 5-8 | UART3 |
| [6:5] | UART2 | 9-12 | UART2 |
| 7 | JTAG | 13-17 | JTAG |
| [9:8] | WDT | 18 | WDT RST |
| [11:10] | PCIE | 19 | PCIe RST |
| [13:12] | **MDIO** | 20-21 | **MDIO (НЕ ТРОГАТЬ!)** |
| 14 | RGMII1 | 49-60 | RGMII1 |
| 15 | RGMII2 | 22-33 | RGMII2 |
| [17:16] | SPI | 34-40 | SPI |
| [19:18] | SDHCI | 41-48 | SDHCI |
