# Сборка Securifi Almond 3S

## Структура исходников

```
modules/
├── lcd_drv.c          # Kernel module (LCD + Touch + PIC Battery + boot console)
├── lcd_render.c       # Framebuffer renderer (unix socket)
├── touch_poll.c       # Touch polling daemon
├��─ data_collector.c   # LTE/WiFi/VPN/Battery stats + time estimation
├── lcd_ui.uc          # UI скрипт (ucode)
├─��� pic_calib.h        # PIC melody data
├── sm0_shared.h       # SM0 register definitions
├── splash_4pda.h      # Boot splash data
└── Makefile           # kbuild Makefile
```

## Сборка прошивки (OpenWrt 24.10.6 upstream)

### Рецепт

```bash
# 1. Клонировать upstream OpenWrt
git clone --branch v24.10.6 --depth 1 https://github.com/openwrt/openwrt.git
cd openwrt

# 2. Применить DTS патч (PR #22141)
git apply /path/to/openwrt-patch/almond3s.patch

# 3. Добавить пакеты lcd-gpio + lcd-ui (Makefile с PKG_SOURCE_URL на GitHub)
# 4. Добавить uci-defaults (192.168.11.1, hostname, TZ)
# 5. Обновить feeds
./scripts/feeds update -a && ./scripts/feeds install -a

# 6. Конфигурация (official config → vermagic compatible)
wget https://downloads.openwrt.org/releases/24.10.6/targets/ramips/mt7621/config.buildinfo -O .config
# Single device:
sed -i 's/CONFIG_TARGET_ALL_PROFILES=y/# CONFIG_TARGET_ALL_PROFILES is not set/' .config
sed -i '/CONFIG_TARGET_DEVICE_/d' .config
sed -i '/CONFIG_TARGET_PER_DEVICE_ROOTFS/d' .config
make defconfig
sed -i 's/# CONFIG_TARGET_DEVICE_ramips_mt7621_DEVICE_securifi_almond-3s is not set/CONFIG_TARGET_DEVICE_ramips_mt7621_DEVICE_securifi_almond-3s=y/' .config
# Добавить пакеты (LCD, MBIM, LuCI, utils)
make defconfig

# 7. Сборка
make -j$(nproc)

# 8. Проверка vermagic
cat build_dir/target-*/linux-*/linux-*/.vermagic
# Должно быть: f31f6f85a36836e510d64a18a9a5f1bf (official 24.10.6 ramips/mt7621)
```

### Пакеты в прошивке

| Категория | Пакеты |
|-----------|--------|
| LCD | kmod-lcd-gpio, lcd-ui |
| LTE | kmod-usb-net-cdc-mbim, kmod-usb-acm, umbim, luci-proto-mbim |
| LuCI | luci-ssl, luci-app-package-manager |
| Utils | opkg, nano, curl |

### VPN (через opkg после прошивки)

```bash
opkg update
opkg install kmod-wireguard wireguard-tools luci-proto-wireguard
opkg install openvpn-openssl kmod-tun
opkg install xl2tpd kmod-l2tp kmod-pppol2tp ppp-mod-pppol2tp
```

Vermagic совпадает с official → opkg ставит kmod пакеты из upstream репозитория.

## Быстрая сборка (разработка)

```bash
cp build_config.sh.example build_config.sh
./build.sh kernel      # lcd_drv.ko через build server (GCC 13.3, kernel 6.6.127)
./build.sh userspace   # zig cc локально
./build.sh deploy      # scp на роутер + restart
```

## Прошивка роутера

```bash
# Через sysupgrade:
sysupgrade -n -F /tmp/openwrt-24.10.6-almond3s-sysupgrade.bin
# Ждать ~11 мин (jffs2 init на 64MB NAND)

# Через U-Boot WebPanel:
# Зажать Reset + включить → http://192.168.1.1 (PC IP: 192.168.1.3)
```

## После прошивки

```bash
./install_to_router.sh              # полная настройка + VPN ключи
./install_to_router.sh --public     # без приватных ключей
```

## DTS

Патч: `openwrt-patch/almond3s.patch` (из [PR #22141](https://github.com/openwrt/openwrt/pull/22141))

```dts
&i2c { status = "okay"; };
&ethphy0 { /delete-property/ interrupts; };  /* КРИТИЧНО для LAN! */
```
