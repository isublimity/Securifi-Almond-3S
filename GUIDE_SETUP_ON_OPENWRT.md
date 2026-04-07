# Securifi Almond 3S — Руководство по сборке и настройке

## Компоненты системы

| Компонент | Тип | Файл | Описание |
|-----------|-----|------|----------|
| lcd_drv.ko | kernel module | modules/lcd_drv.c | LCD ILI9341 + SX8650 touch + PIC battery |
| lcd_render | userspace (static) | modules/lcd_render.c | Unix socket JSON renderer → /dev/lcd |
| touch_poll | userspace (static) | modules/touch_poll.c | Touch polling daemon |
| data_collector | userspace (static) | modules/data_collector.c | LTE/WiFi/VPN/Battery stats → /tmp/lcd_data.json |
| lcd_ui.uc | ucode script | modules/lcd_ui.uc | UI логика (uloop + ubus + uci) |
| settings.lua | config | modules/settings.lua | UI настройки |

## Сборка

### Требования
- Build server с OpenWrt SDK (fildunsky_openwrt)
- Zig compiler на Mac (для userspace cross-compilation)
- Настроенный `build_config.sh`

### Настройка build_config.sh
```bash
cp build_config.sh.example build_config.sh
# Заполнить:
BUILD_SERVER="user@build-server"
BUILD_DIR="/path/to/fildunsky_openwrt"  # не используется, FILD_DIR в build.sh
ROUTER="root@192.168.11.1"
```

### Команды сборки
```bash
cd Securifi-Almond-3S/

./build.sh kernel      # lcd_drv.ko через build server (GCC 14.3, kernel 6.12.74)
./build.sh userspace   # lcd_render, touch_poll, data_collector через zig cc
./build.sh deploy      # scp всё на роутер + restart

./build.sh firmware    # полная сборка OpenWrt firmware с lcd-gpio пакетом
./build.sh all         # kernel + userspace
```

### Результат
Бинарники в `out/`:
- `out/lcd_drv.ko` — kernel module
- `out/lcd_render` — статический MIPS binary
- `out/touch_poll` — статический MIPS binary
- `out/data_collector` — статический MIPS binary

## Прошивка firmware

### Через sysupgrade (из OpenWrt):
```bash
scp out/openwrt-*-sysupgrade.bin root@192.168.11.1:/tmp/
ssh root@192.168.11.1 'sysupgrade -n /tmp/openwrt-*-sysupgrade.bin'
```
**-n = без сохранения настроек** (чистая установка)

### Через U-Boot WebPanel:
1. Зажать Reset + включить питание
2. Открыть http://192.168.1.1 (Mac: sudo ifconfig en16 alias 192.168.1.3)
3. Загрузить sysupgrade.bin

### После прошивки
Нужно задеплоить userspace бинарники (они НЕ входят в firmware image):
```bash
./build.sh deploy
```

Или вручную:
```bash
scp out/lcd_render out/touch_poll out/data_collector root@192.168.11.1:/usr/bin/
scp modules/lcd_ui.uc root@192.168.11.1:/usr/bin/
scp modules/settings.lua root@192.168.11.1:/etc/lcd/settings.lua
ssh root@192.168.11.1 'chmod +x /usr/bin/lcd_render /usr/bin/touch_poll /usr/bin/data_collector'
```

## Запуск UI

### Порядок запуска (важен!):
```bash
# 1. lcd_drv.ko — загружается автоматически (AutoLoad 90)
# Если нет: insmod /lib/modules/$(uname -r)/lcd_drv.ko

# 2. lcd_render — unix socket сервер
lcd_render &

# 3. data_collector — сбор данных (battery, LTE, WiFi, VPN)
data_collector &

# 4. lcd_ui.uc — UI (подключается к lcd_render через /tmp/lcd.sock)
ucode /usr/bin/lcd_ui.uc &
```

### Автозапуск через init.d:
Файл `/etc/init.d/lcd_ui`:
```sh
#!/bin/sh /etc/rc.common
START=99
USE_PROCD=1

start_service() {
    procd_open_instance lcd_render
    procd_set_param command /usr/bin/lcd_render
    procd_set_param respawn
    procd_close_instance

    procd_open_instance data_collector
    procd_set_param command /usr/bin/data_collector
    procd_set_param respawn
    procd_close_instance

    sleep 2

    procd_open_instance lcd_ui
    procd_set_param command /usr/bin/ucode /usr/bin/lcd_ui.uc
    procd_set_param respawn
    procd_close_instance
}
```
```bash
chmod +x /etc/init.d/lcd_ui
/etc/init.d/lcd_ui enable
/etc/init.d/lcd_ui start
```

**НЕ запускать touch_poll** если lcd_ui запущен — они конфликтуют (оба читают touch events).

## Проверки

### 1. LCD дисплей
```bash
lsmod | grep lcd_drv           # модуль загружен?
ls /dev/lcd                     # device node?
dmesg | grep lcd_drv            # ошибки?
ls /tmp/lcd.sock                # lcd_render socket?
```

### 2. Touch
```bash
dmesg | grep "touch DOWN"       # нажатия видны?
# Если touch не работает — проверить что lcd_ui запущен и touch_poll НЕ запущен
```

### 3. Battery (PIC16LF1509)
```bash
dmesg | grep "PIC bat:"         # данные от PIC?
dmesg | grep "PIC ADC"          # парсинг ADC?

# Формат: ff XX YY 02 04 ZZ
# XX = ADC[9:2], YY = ADC[1:0]<<6, ZZ = charger (01=yes)
# Валидация: byte[3]==0x02 && byte[4]==0x04
# ADC = (XX << 2) | (YY >> 6), 10-bit, 0-1023
# ~5-10% reads corrupted — фильтруются

# JSON проверка:
cat /tmp/lcd_data.json | grep battery
# {"adc": 810, "percent": 64, "charging": true, "valid": true}
```

### 4. LTE модем (Fibocom L860-GL)
```bash
ls /dev/cdc-wdm0                # MBIM device?
ip link show wwan0              # сетевой интерфейс?

# GPIO reset модема:
echo 33 > /sys/class/gpio/export
echo out > /sys/class/gpio/gpio33/direction
echo 0 > /sys/class/gpio/gpio33/value; sleep 1
echo 1 > /sys/class/gpio/gpio33/value
```

### 5. GPIOMODE (критически важно!)
```bash
# Бит 2 GPIOMODE ДОЛЖЕН быть 0 (SM0 подключён к I2C пинам)
# Если 1 — PIC и SX8650 не работают!
devmem 0x1E000060  # должно быть 0x00048580 (bit2=0)
```

## PIC Battery Protocol (КЛЮЧЕВОЕ)

### Проблема
OpenWrt i2c-mt7621 при загрузке ставит PIC MSSP в SSPOV state.
PIC NACKает все read addresses пока SSPOV не очищен.

### Решение: cmd 0x39 (SSP REINIT) перед каждым read
```
1. cmd 0x39 → PIC переинициализирует MSSP (очищает SSPOV)
2. cmd 0x36 → PIC запускает ADC conversion (AN11)
3. wait 5ms
4. read 6 bytes (START=5, MODE=1)
5. validate: byte[3]==0x02 && byte[4]==0x04
```

### SM0 регистры (lcd_drv write protocol):
```c
gw(SM0_CTL1, 0x90644042);  // ODRAIN + clk_div=100 + bit14 + EN
gw(SM0_CFG, 0xFA);          // WRITE-ONLY timing (readback=0!)
gw(SM0_DATA, PIC_ADDR);     // 0x2A
gw(SM0_START, 1);           // byte count
gw(SM0_DATAOUT, cmd);       // command byte
gw(SM0_STATUS, 0);          // MODE=0 triggers write!
```

### ЗАПРЕЩЕНО:
- **gpio_request()** → ломает IRQ #23 → LAN carrier=0
- **rmmod i2c_mt7621** → ломает MT7530 LAN
- **GPIOMODE bit2=1** → SM0 отключён от I2C пинов
- **RSTCTRL reset каждый цикл** → ломает PIC state

### LED команды (1-byte, работают):
| Cmd | Эффект |
|-----|--------|
| 0x30 | LED blink |
| 0x31 | LED on |
| 0x32 | LED off |

### Multi-byte write (3+ bytes) — НЕ РАБОТАЕТ НАДЁЖНО
SM0 auto mode с START>1 доставляет только первый byte.
RGB LED и buzzer через melody tables требуют multi-byte → TODO.

## Troubleshooting

### Battery всегда valid:false
1. Проверить `dmesg | grep "PIC bat:"` — есть ли reads с `02 04`?
2. Если все reads corrupted → проверить GPIOMODE bit2
3. data_collector читает ioctl 2 на /dev/lcd → должен получить последний valid read

### LCD белый/чёрный экран
1. `lsmod | grep lcd` — модуль загружен?
2. `ls /tmp/lcd.sock` — lcd_render запущен?
3. `ps | grep ucode` — lcd_ui запущен?

### Touch не реагирует
1. **Убить touch_poll** если запущен (конфликт с lcd_ui)
2. `dmesg | grep "touch DOWN"` — kernel видит нажатия?
3. lcd_ui должен быть единственным, кто читает touch

### LAN не работает (carrier=0)
1. Проверить DTS: `&ethphy0 { /delete-property/ interrupts; }`
2. **Никогда** не использовать gpio_request() в lcd_drv
3. **Никогда** не rmmod i2c_mt7621

## Файлы на роутере

```
/lib/modules/6.12.74/lcd_drv.ko    — kernel module
/usr/bin/lcd_render                 — JSON renderer
/usr/bin/touch_poll                 — touch daemon (НЕ запускать с lcd_ui!)
/usr/bin/data_collector             — stats collector
/usr/bin/lcd_ui.uc                  — UI script
/etc/lcd/settings.lua               — UI config
/dev/lcd                            — LCD framebuffer + PIC ioctl
/tmp/lcd.sock                       — lcd_render unix socket
/tmp/lcd_data.json                  — данные (LTE, WiFi, VPN, battery)
```
