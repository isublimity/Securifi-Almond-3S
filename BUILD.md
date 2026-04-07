# Сборка Securifi Almond 3S

## Структура

```
modules/
├── lcd_drv.c          # Kernel module (LCD + Touch + PIC Battery)
├── lcd_render.c       # Framebuffer renderer (unix socket)
├── touch_poll.c       # Touch polling daemon
├── data_collector.c   # LTE/WiFi/VPN/Battery stats
├── lcd_ui.uc          # UI скрипт (ucode)
├── pic_calib.h        # PIC melody data (calibration tables)
├── sm0_shared.h       # SM0 register definitions
├── splash_4pda.h      # Boot splash data
└── Makefile           # kbuild Makefile для lcd_drv.ko
```

## Способ 1: OpenWrt пакеты (рекомендуемый)

Прошивка собирается из [fildunsky/openwrt](https://github.com/fildunsky/openwrt), ветка `almond-25.12`, ядро 6.12.

Пакеты тянут исходники из [isublimity/Securifi-Almond-3S](https://github.com/isublimity/Securifi-Almond-3S) при сборке:
- **kmod-lcd-gpio** — kernel module (lcd_drv.ko)
- **lcd-ui** — userspace (lcd_render, data_collector, touch_poll, lcd_ui.uc, init.d)

```bash
# На build server:
cd /mnt/sata/var/openwrt/fildunsky_openwrt
make menuconfig   # Включить: kmod-lcd-gpio, lcd-ui
make -j$(nproc)
# Результат: bin/targets/ramips/mt7621/openwrt-*-sysupgrade.bin
```

## Способ 2: build.sh (быстрая итерация)

Для разработки — собирает только изменённые компоненты без полной прошивки.

### Настройка
```bash
cp build_config.sh.example build_config.sh
# BUILD_SERVER="user@build-server"
# ROUTER="root@192.168.11.1"
```

### Команды
```bash
./build.sh kernel      # lcd_drv.ko через build server (GCC 14.3, kernel 6.12.74)
./build.sh userspace   # lcd_render, touch_poll, data_collector через zig cc
./build.sh all         # kernel + userspace
./build.sh deploy      # scp на роутер + restart
./build.sh firmware    # полная сборка прошивки на build server
```

### Kernel module
Собирается на build server через SSH:
```bash
scp modules/lcd_drv.c → build server
make -C KDIR M=package/lcd-gpio/src modules
scp lcd_drv.ko ← build server
```

### Userspace
Собирается локально через `zig cc` (static, mipsel-linux-musleabi):
```bash
zig cc -target mipsel-linux-musleabi -Os -static -o lcd_render modules/lcd_render.c
zig cc -target mipsel-linux-musleabi -Os -static -o data_collector modules/data_collector.c
zig cc -target mipsel-linux-musleabi -Os -static -o touch_poll modules/touch_poll.c
```

## Деплой

```bash
./build.sh deploy          # автоматически
# Или:
./install_to_router.sh     # настройка после прошивки (first_setup.sh)
```

## Прошивка роутера

### U-Boot WebPanel
1. Зажать Reset + включить питание
2. Открыть http://192.168.1.1 (PC IP: 192.168.1.3)
3. Загрузить sysupgrade.bin
4. Ждать ~10 мин → power cycle → ждать 11 мин (jffs2)

### Из OpenWrt
```bash
sysupgrade -n /tmp/openwrt-*-sysupgrade.bin
```

## Пакеты в прошивке

| Категория | Пакеты |
|-----------|--------|
| LCD | kmod-lcd-gpio, lcd-ui |
| LTE | kmod-usb-net-cdc-mbim, kmod-usb-acm, umbim, luci-proto-mbim |
| VPN | kmod-wireguard, wireguard-tools, openvpn-openssl, kmod-tun, xl2tpd, kmod-l2tp, kmod-pppol2tp |
| UI | luci-ssl, luci-i18n-base-ru, ucode, ucode-mod-fs/ubus/uci/uloop/socket/io |
| Utils | opkg, nano, curl |

## DTS — критические настройки

```dts
&i2c { status = "okay"; };
&ethphy0 { /delete-property/ interrupts; };  /* КРИТИЧНО для LAN! */
&state_default {
    lcd_jtag { groups = "jtag"; function = "gpio"; };
};
```
