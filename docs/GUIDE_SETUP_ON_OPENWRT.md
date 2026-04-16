# Securifi Almond 3S — Руководство по сборке и настройке

## Компоненты системы

| Компонент | Тип | Файл | Описание |
|-----------|-----|------|----------|
| lcd_drv.ko | kernel module | modules/lcd_drv.c | LCD ILI9341 + SX8650 touch + PIC battery |
| lcd_render | userspace | modules/lcd_render.c | Unix socket JSON renderer → /dev/lcd |
| touch_poll | userspace | modules/touch_poll.c | Touch polling daemon |
| data_collector | userspace | modules/data_collector.c | LTE/WiFi/VPN/Battery stats → /tmp/lcd_data.json |
| lcd_ui.uc | ucode script | modules/lcd_ui.uc | UI логика (uloop + ubus + uci) |

## Сборка прошивки

### Базовая прошивка
Основа: upstream [OpenWrt 24.10.6](https://github.com/openwrt/openwrt) + [almond3s.patch](../openwrt-patch/almond3s.patch) ([PR #22141](https://github.com/openwrt/openwrt/pull/22141)), ядро 6.6.127.

Пакеты OpenWrt:
- **kmod-lcd-gpio** — kernel module (lcd_drv.ko, AutoLoad 90)
- **lcd-ui** — userspace (lcd_render, data_collector, touch_poll, lcd_ui.uc, init.d)

Исходники пакетов тянутся из [isublimity/Securifi-Almond-3S](https://github.com/isublimity/Securifi-Almond-3S) при сборке.

```bash
# На build server:
cd /path/to/fildunsky_openwrt
make menuconfig   # Включить kmod-lcd-gpio + lcd-ui
make -j$(nproc)
# Результат: bin/targets/ramips/mt7621/openwrt-*-sysupgrade.bin
```

### Сборка только модуля (без полной прошивки)
```bash
# build.sh scp'шит lcd_drv.c на build server и собирает через KDIR
./build.sh kernel
```

## Прошивка роутера

### Через U-Boot WebPanel
1. Зажать Reset + включить питание
2. Открыть http://192.168.1.1 (PC IP: 192.168.1.3)
3. Загрузить sysupgrade.bin
4. Ждать ~10 мин → power cycle → ждать 11 мин (jffs2)

### Через sysupgrade (из OpenWrt)
```bash
scp out/openwrt-*-sysupgrade.bin root@192.168.11.1:/tmp/
ssh root@192.168.11.1 'sysupgrade -n /tmp/openwrt-*-sysupgrade.bin'
```

## Настройка после прошивки

Бинарники уже в прошивке (пакеты). Скрипт настраивает систему:

```bash
# С Mac:
./install_to_router.sh              # полная настройка + VPN ключи
./install_to_router.sh --public     # без приватных ключей

# Или вручную:
scp first_setup.sh root@192.168.11.1:/tmp/
ssh root@192.168.11.1 'sh /tmp/first_setup.sh --all'
```

### Что делает first_setup.sh:
- **--setup_system**: hostname, timezone, LAN IP, IRQ affinity
- **--setup_wifi**: 5GHz (Almond-5G, ch165) + 2.4GHz (Almond, ch6)
- **--setup_lte**: GPIO reset модема, APN=internet, firewall
- **--setup_vpn**: VPN shell скрипты для LCD UI, WG hotplug
- **--setup_ui**: скачивает lcd_ui.uc, создаёт init.d
- **--setup_private**: sourced `/tmp/secrets.sh` (WG ключи, OpenVPN)

## Автозапуск (init.d)

Пакет `lcd-ui` устанавливает `/etc/init.d/lcd_ui`:

```sh
#!/bin/sh /etc/rc.common
START=99
STOP=10
USE_PROCD=1
start_service() {
    [ -c /dev/lcd ] || return
    echo -n "touch_start" > /dev/lcd 2>/dev/null
    sleep 1
    procd_open_instance lcd_render
    procd_set_param command /usr/bin/lcd_render
    procd_set_param respawn
    procd_close_instance
    procd_open_instance touch_poll
    procd_set_param command /usr/bin/touch_poll daemon_fg
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
    procd_set_param stderr 1
    procd_close_instance
}
stop_service() { killall touch_poll 2>/dev/null; }
```

## Проверки

### LCD дисплей
```bash
lsmod | grep lcd_drv           # модуль загружен?
ls /dev/lcd                     # device node?
ls /tmp/lcd.sock                # lcd_render socket?
```

### Battery (PIC16LF1509)
```bash
dmesg | grep "PIC bat:"         # данные от PIC?
cat /tmp/lcd_data.json | grep battery
# {"adc": 810, "percent": 64, "charging": true, "valid": true}
```

### LTE модем
```bash
ls /dev/cdc-wdm0                # MBIM device?
ip link show wwan0              # сетевой интерфейс?
```

### GPIOMODE
```bash
devmem 0x1E000060  # должно быть 0x00048580 (bit2=0 = SM0 → I2C)
```

## PIC Battery Protocol

### Проблема
i2c-mt7621 при загрузке ставит PIC MSSP в SSPOV state → NACK на всё.

### Решение: cmd 0x39 → cmd 0x36 → read 6 bytes
```
1. cmd 0x39 → SSP REINIT (очищает SSPOV)
2. cmd 0x36 → ADC READ (AN11)
3. wait 5ms
4. read 6 bytes
5. validate: buf[3]==0x02 && buf[4]==0x04
6. ADC = (buf[1] << 2) | (buf[2] >> 6), charger = buf[5]
```

## ЗАПРЕЩЕНО

- **gpio_request()** на bank0 → убивает MT7530 LAN (IRQ #23)
- **GPIOMODE bit2=1** → SM0 отключён от I2C пинов
- **RSTCTRL reset каждый цикл** → ломает PIC state

## Файлы на роутере

```
/lib/modules/6.6.127/lcd_drv.ko     — kernel module
/usr/bin/lcd_render                  — JSON renderer
/usr/bin/touch_poll                  — touch daemon
/usr/bin/data_collector              — stats collector
/usr/bin/lcd_ui.uc                   — UI script
/dev/lcd                             — LCD framebuffer + PIC ioctl
/tmp/lcd.sock                        — lcd_render unix socket
/tmp/lcd_data.json                   — данные (LTE, WiFi, VPN, battery)
```
