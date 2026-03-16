# Fix1 — Проблемы первой сборки прошивки (2026-03-16)

## Проблема 1: WiFi не стартует автоматически

**Симптом**: WiFi модули загружены (`lsmod | grep mt76` показывает mt7615e), но `uci show wireless` пусто. Нет WiFi сетей.

**Причина**: `wifi config` не выполняется автоматически при первой загрузке. uci-defaults скрипт пытался настроить `wireless.radio0/radio1`, но эти секции ещё не существовали (создаются только после `wifi config`).

**Дополнительно**: При ручном `echo "14c3 7662" > /sys/bus/pci/drivers/mt7615e/new_id` WiFi определяется, но с ошибками:
- `Unable to change power state from D3cold to D0`
- `EEPROM WARNING` на строке eeprom.c:31
- `Firmware is not ready for download`

После этого `uci show wireless` показывает radio0 (5GHz) и radio1 (2.4GHz).

**Fix**: В uci-defaults добавить `wifi config` ПЕРЕД настройкой wireless:
```bash
# Генерируем базовый wireless config (создаёт radio0/radio1)
wifi config

# Ждём создания конфига
sleep 2

# Теперь настраиваем
uci set wireless.radio0.disabled='0'
...
```

**Fix альтернативный**: Добавить в прошивку файл `/etc/config/wireless` с дефолтным конфигом.

## Проблема 2: LAN порты NO-CARRIER

**Симптом**: Все 3 LAN порта показывают `NO-CARRIER` при подключённом кабеле. USB-LAN адаптер подключён к Mac.

**Причина**: Возможно MT7530 switch не инициализирован правильно, или IRQ #23 ошибка (`mt7530_irq_thread_fn — nobody cared`) сломала Ethernet switch.

**Лог**:
```
[  387.619567] irq 23: nobody cared (try booting with the "irqpoll" option)
[  387.931971] Disabling IRQ #23
```

**Fix**: Проверить DTS — правильно ли назначены порты MT7530. Попробовать `irqpoll` в kernel cmdline. Проверить физическое подключение — какой порт LAN, какой WAN.

## Проблема 3: i2c-mt7621 загружается автоматически

**Симптом**: Модуль `i2c-mt7621` загружается при старте, блокирует palmbus I2C для тачскрина.

**Причина**: DTS содержит `i2c` ноду, ядро автоматически загружает драйвер.

**Лог**:
```
[    2.683785] i2c-mt7621 1e000900.i2c: clock 100 kHz
```

**Fix варианты**:
1. Добавить `i2c-mt7621` в blacklist: `/etc/modules.d/blacklist-i2c`
2. В lcd-init.sh делать `rmmod i2c_mt7621` (уже реализовано)
3. Убрать/отключить i2c ноду в DTS для Almond 3S (правильный fix)

**Fix в прошивке**:
```bash
# Добавить в uci-defaults или в files/:
echo "blacklist i2c_mt7621" > /etc/modprobe.d/blacklist-i2c.conf
```

Или создать файл в сборке:
`target/linux/ramips/mt7621/base-files/etc/modprobe.d/blacklist-i2c.conf`:
```
blacklist i2c_mt7621
```

## Проблема 4: LCD splash показывается на 1.5 сек, потом чёрный

**Симптом**: При загрузке lcd_drv.ko показывает 4PDA splash на 1.5 секунды, потом экран чёрный (нет lcd_render и lcd_ui).

**Причина**: lcd_render и lcd_ui.lua не входят в прошивку — устанавливаются через deploy-router.sh.

**Fix**: Добавить lcd_render, lcd_ui.lua, lcdlib.so, lcd-init.sh в пакет lcd-gpio (или отдельный пакет). Тогда при загрузке lcd-init.sh автоматически запустит весь LCD стек.

## Проблема 5: EEPROM/Calibration WiFi

**Симптом**: `mt7615_eeprom_init` WARNING при загрузке WiFi.

**Причина**: EEPROM калибровочные данные MT7615 хранятся в разделе `factory` (MTD3, offset 0x40000-0x50000). Драйвер mt7615e ожидает данные в определённом формате.

**Fix**: Проверить что factory раздел не затёрт. Дамп: `dd if=/dev/mtd3 of=/tmp/factory.bin bs=64k count=1` и сверить с бэкапом.

## Проблема 6: uci-defaults не применяет wireless config

**Симптом**: `99-almond3s-setup` выполняется, LAN IP меняется на 192.168.11.1, LTE настраивается, но WiFi остаётся пустым.

**Причина**: В скрипте проверка `case "$(board_name)"` — функция `board_name` может быть недоступна в контексте uci-defaults.

**Fix**: Убрать проверку board_name или заменить на проверку файла:
```bash
#!/bin/sh
# Без проверки board_name — этот файл только в прошивке Almond 3S
wifi config
sleep 2
uci set wireless.radio0.disabled='0'
...
```

## Сводка: что исправить в следующей сборке

| # | Что | Где | Приоритет |
|---|-----|-----|-----------|
| 1 | `wifi config` перед настройкой wireless | uci-defaults | **КРИТИЧНО** |
| 2 | Убрать `case board_name` из uci-defaults | uci-defaults | **КРИТИЧНО** |
| 3 | Blacklist i2c-mt7621 | modprobe.d или DTS | Высокий |
| 4 | LCD стек в прошивку (lcd_render, lcd_ui, lcdlib) | Пакет или files/ | Высокий |
| 5 | Проверить LAN порты / MT7530 IRQ | DTS / kernel config | Средний |
| 6 | Проверить factory EEPROM для WiFi | Бэкап / MTD3 | Средний |
