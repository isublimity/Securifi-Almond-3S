# Touch + LAN Coexistence Plan

## Проблема

MT7530 Ethernet switch использует I2C шину (SM0 контроллер на palmbus 0xBE000900) для PHY коммуникации. Наш тачскрин SX8650 подключён к тому же I2C контроллеру (addr 0x48). PIC16 батарея тоже (addr 0x2A).

**Три потребителя одного I2C контроллера:**
1. **i2c-mt7621** (kernel driver) — MT7530 Ethernet PHY
2. **lcd_drv.ko** (наш модуль) — SX8650 touch через palmbus direct
3. **lcd_drv.ko** — PIC16 battery через palmbus direct

### Что ломает LAN:
- `&i2c { status = "disabled"; }` в DTS → MT7530 не может общаться с PHY → carrier=0 → IRQ #23
- `rmmod i2c_mt7621` в runtime → тот же эффект
- Любое отключение I2C → LAN мёртв

### Что ломает Touch:
- i2c-mt7621 загружен → перехватывает SM0 регистры → palmbus direct touch read возвращает мусор
- SM0_CTL1 перезаписывается Linux I2C driver после каждой транзакции MT7530

## Варианты решения

### Вариант A: Linux I2C для Touch (рекомендуется)

Переписать SX8650 touch reading через Linux I2C subsystem (`i2c_transfer()`) вместо palmbus direct.

**Плюсы:**
- Нет конфликта — все устройства через один I2C stack
- Автоматическая синхронизация (kernel mutex)
- Совместимость с MT7530

**Минусы:**
- Нужно переписать sx8650_read_xy() в lcd_drv.ko
- Стоковый протокол SELECT(X/Y) может не работать через i2c_transfer (раньше пробовали — FF)
- Нужно тестировать

**Реализация:**
```c
// Вместо palmbus direct:
// gw(SM0_CTL1, 0x90644042);
// gw(SM0_DATA, 0x48);
// gw(SM0_DATAOUT, 0x80); // SELECT(X)
// ...

// Использовать:
struct i2c_msg msgs[2] = {
    { .addr = 0x48, .flags = 0, .len = 1, .buf = &cmd_select_x },
    { .addr = 0x48, .flags = I2C_M_RD, .len = 2, .buf = xy_data },
};
i2c_transfer(adapter, msgs, 2);
```

**Проблема:** Ранее i2c_transfer для SX8650 не работал (возвращал FF). Но мы тестировали с rmmod/disabled i2c. Нужно перетестировать с **включённым** i2c-mt7621.

### Вариант B: Мьютекс/Lock между I2C и Touch

lcd_drv.ko перед каждым palmbus touch read:
1. Берёт lock (disable i2c-mt7621 transactions)
2. Делает palmbus read (touch)
3. Восстанавливает SM0 регистры
4. Освобождает lock

**Плюсы:**
- Минимальные изменения в lcd_drv.ko
- palmbus протокол (который работает) сохраняется

**Минусы:**
- Нужен доступ к внутреннему lock i2c-mt7621 driver
- Может вызвать задержки в MT7530 (и LAN latency)
- Сложно реализовать без хаков в ядре

### Вариант C: Полная пересинхронизация SM0 после touch

После каждого palmbus touch read — восстановить SM0 регистры в состояние, ожидаемое i2c-mt7621:

```c
// After touch read:
gw(SM0_CTL1, 0x8064800E);  // Linux I2C default
gw(SM0_CFG, orig_cfg);      // Restore original
```

**Плюсы:**
- Простая реализация
- Работает если i2c-mt7621 не делает транзакции одновременно

**Минусы:**
- Race condition — MT7530 может начать I2C транзакцию во время touch read
- Уже реализовано (SM0_CTL1 восстанавливается) — но LAN всё равно падает

### Вариант D: Отдельный I2C bus для Touch

Если тачскрин SX8650 подключён к тем же I2C пинам что MT7530 (GPIO 3,4) — аппаратно невозможно разделить.

Но: MT7530 использует MDIO (GPIO 20,21), НЕ I2C! MT7530 на MDIO bus, не на I2C bus 0.

**Проверить:** может i2c-mt7621 НЕ используется MT7530 вообще? Может IRQ #23 возникает по другой причине?

### Вариант E: SX8654 mainline Linux driver

Использовать upstream `sx8654.c` driver из Linux kernel (`drivers/input/touchscreen/sx8654.c`).

**Плюсы:**
- Работает через Linux I2C (i2c_transfer)
- Поддерживает SX8650 (compatible)
- Input subsystem — стандартный /dev/input/eventX

**Минусы:**
- Нужно добавить в DTS: `&i2c { sx8650@48 { compatible = "semtech,sx8650"; reg = <0x48>; }; };`
- Нужен kmod-input-touchscreen в прошивке
- lcd_ui.lua нужно переписать на чтение /dev/input/eventX вместо ioctl

## Исследование (первый шаг)

**Проверить гипотезу: MT7530 не использует I2C bus 0.**

MT7530 подключён через MDIO (mdio-bus:1f). MDIO — это отдельный протокол на GPIO 20,21. I2C bus 0 (GPIO 3,4) используется только для SX8650 и PIC16.

Если это так — i2c-mt7621 не нужен для MT7530, и rmmod не должен ломать LAN.

Но rmmod ЛОМАЕТ LAN. Значит:
1. MT7530 driver использует i2c-mt7621 для чего-то (register access?)
2. Или rmmod i2c_mt7621 вызывает побочный эффект (reset SM0, изменение GPIOMODE?)

**Проверить на роутере (с i2c loaded, LAN работает):**
```bash
# Проверить что i2c-mt7621 реально делает
cat /sys/bus/i2c/devices/*/name
i2cdetect -y 0
# Проверить MDIO
cat /sys/bus/mdio_bus/devices/*/name
```

## Рекомендуемый план

1. **Сначала** — поставить рабочую прошивку (fildunsky 25.12 или r1+4)
2. **Проверить** — i2cdetect с загруженным i2c-mt7621 (что видно на bus 0?)
3. **Попробовать Вариант A** — i2c_transfer для SX8650 touch с **включённым** i2c
4. Если touch через i2c_transfer не работает — **Вариант E** (sx8654 mainline driver)
5. Если ничего — **Вариант C** (пересинхронизация) с acceptance of race condition
