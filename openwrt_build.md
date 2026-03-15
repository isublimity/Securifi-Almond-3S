# Сборка OpenWrt для Securifi Almond 3S

## Репозитории

- **OpenWrt форк**: https://github.com/isublimity/openwrt_almond/commits/almond-3s/
- **Документация и модули**: https://github.com/isublimity/Securifi-Almond-3S

## Требования

- **Ветка**: [`almond-3s`](https://github.com/isublimity/openwrt_almond/tree/almond-3s) (базируется на `openwrt-24.10`)
- **Ядро**: 6.6.x (**ТОЛЬКО!** Ядро 6.12.x несовместимо с U-Boot — кирпич)
- **Сервер сборки**: ~10 ГБ RAM, ~30 ГБ диска, Linux x86_64
- **Тулчейн**: собирается автоматически (GCC 13.3, musl, mipsel_24kc)

## Быстрый старт

```bash
git clone https://github.com/isublimity/openwrt_almond.git
cd openwrt_almond
git checkout almond-3s
./build-almond-3s.sh          # полная сборка
./build-almond-3s.sh clean    # с очисткой
```

Результат: `bin/targets/ramips/mt7621/openwrt-*-securifi_almond-3s-*-sysupgrade.bin`

## Структура проекта в OpenWrt

### Определение устройства

Файл: `target/linux/ramips/image/mt7621.mk`

```makefile
define Device/securifi_almond-3s
  $(Device/dsa-migration)
  IMAGE_SIZE := 65408k
  DEVICE_VENDOR := Securifi
  DEVICE_MODEL := Almond 3S
  DEVICE_PACKAGES := kmod-usb3 kmod-usb-net-qmi-wwan kmod-usb-serial-option uqmi kmod-lcd-gpio
  SUPPORTED_DEVICES += securifi,almond-3s
endef
TARGET_DEVICES += securifi_almond-3s
```

### Device Tree

Файл: `target/linux/ramips/dts/mt7621_securifi_almond-3s.dts`

### Kernel module (lcd_drv)

Пакет: `package/lcd-gpio/`

```
package/lcd-gpio/
├── Makefile              # OpenWrt package makefile
└── src/
    ├── Makefile          # obj-m += lcd_drv.o
    ├── lcd_drv.c         # Исходник модуля
    └── lcd_gpio.c        # Старый модуль (устаревший)
```

**ВАЖНО**: Пакетный Makefile (`package/lcd-gpio/Makefile`) сейчас ссылается на `lcd_gpio.ko`. Для перехода на `lcd_drv.ko` нужно обновить:

```makefile
# package/lcd-gpio/Makefile — ОБНОВИТЬ:
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=lcd-gpio
PKG_VERSION:=2.0
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/lcd-gpio
  SUBMENU:=Other modules
  TITLE:=ILI9341 LCD + SX8650 Touch + PIC16 Battery
  FILES:=$(PKG_BUILD_DIR)/lcd_drv.ko
  AUTOLOAD:=$(call AutoLoad,90,lcd_drv)
endef

define Build/Compile
	$(KERNEL_MAKE) M=$(PKG_BUILD_DIR) modules
endef

$(eval $(call KernelPackage,lcd-gpio))
```

### Src Makefile

```makefile
# package/lcd-gpio/src/Makefile
obj-m += lcd_drv.o
```

## Сборка только kernel module (без полной пересборки)

```bash
# На сервере сборки
cd /mnt/sata/var/openwrt/fork/openwrt_almond

KDIR=build_dir/target-mipsel_24kc_musl/linux-ramips_mt7621/linux-6.6.127
CROSS=$(pwd)/staging_dir/toolchain-mipsel_24kc_gcc-13.3.0_musl/bin/mipsel-openwrt-linux-musl-

make -C $KDIR M="$(pwd)/package/lcd-gpio/src" ARCH=mips CROSS_COMPILE="$CROSS" modules
```

Результат: `package/lcd-gpio/src/lcd_drv.ko`

## Сборка userspace (на Mac через zig)

```bash
# lcd_render — рендерер дисплея
zig cc -target mipsel-linux-musleabi -O2 -static -o lcd_render lcd_render.c

# pic_test — тест PIC battery
zig cc -target mipsel-linux-musleabi -O2 -static -o pic_test pic_test.c

# lcd_touch_read — чтение тача
zig cc -target mipsel-linux-musleabi -O2 -static -o lcd_touch_read lcd_touch_read.c
```

Или через `build.sh`:
```bash
cd Securifi-Almond-3S
./build.sh all          # kernel module (на сервере) + userspace (zig)
./build.sh kernel       # только kernel module
./build.sh userspace    # только userspace
./build.sh deploy       # залить на роутер
./build.sh deploy-run   # залить + перезагрузить модуль
```

## Конфигурация прошивки (.config)

### Обязательные пакеты

| Пакет | Зачем |
|-------|-------|
| `CONFIG_TARGET_ramips_mt7621_DEVICE_securifi_almond-3s=y` | Целевое устройство |
| `CONFIG_PACKAGE_kmod-lcd-gpio=y` | Наш модуль дисплея (lcd_drv.ko) |
| `CONFIG_PACKAGE_kmod-usb3=y` | USB контроллер (модем, флешки) |
| `CONFIG_PACKAGE_kmod-usb-net-qmi-wwan=y` | QMI для LTE модема |
| `CONFIG_PACKAGE_kmod-usb-serial-option=y` | AT-порт модема (/dev/ttyUSB*) |
| `CONFIG_PACKAGE_uqmi=y` | Утилита управления QMI |

### I2C (для PIC battery — когда понадобится)

| Пакет | Зачем |
|-------|-------|
| `CONFIG_PACKAGE_kmod-i2c-core=y` | Ядро I2C подсистемы |
| `CONFIG_PACKAGE_kmod-i2c-mt7621=y` | MT7621 I2C контроллер |
| `CONFIG_PACKAGE_i2c-tools=y` | Утилиты отладки I2C |

**ВНИМАНИЕ**: `kmod-i2c-mt7621` конфликтует с тачскрином! Модуль захватывает SM0 контроллер и блокирует palmbus доступ. Решение: `rmmod i2c_mt7621` перед загрузкой lcd_drv.ko, или не включать в автозагрузку.

### VPN

| Пакет | Зачем |
|-------|-------|
| `CONFIG_PACKAGE_kmod-wireguard=y` | WireGuard в ядре |
| `CONFIG_PACKAGE_wireguard-tools=y` | wg утилита |
| `CONFIG_PACKAGE_luci-proto-wireguard=y` | LuCI интеграция WG |
| `CONFIG_PACKAGE_openvpn-openssl=y` | OpenVPN (требует kmod-tun!) |
| `CONFIG_PACKAGE_kmod-tun=y` | **TUN device — нужен для OpenVPN!** |
| `CONFIG_PACKAGE_luci-app-openvpn=y` | LuCI для OpenVPN |

### DNS и прочее

| Пакет | Зачем |
|-------|-------|
| `CONFIG_PACKAGE_https-dns-proxy=y` | DNS-over-HTTPS |
| `CONFIG_PACKAGE_luci=y` | Веб-интерфейс |
| `CONFIG_PACKAGE_luci-ssl=y` | HTTPS для LuCI |
| `CONFIG_PACKAGE_luci-i18n-base-ru=y` | Русский язык |
| `CONFIG_PACKAGE_socat=y` | Нужен для lcd_ui.lua (unix socket) |

### Утилиты

| Пакет | Зачем |
|-------|-------|
| `CONFIG_PACKAGE_htop=y` | Мониторинг процессов |
| `CONFIG_PACKAGE_nano=y` | Текстовый редактор |
| `CONFIG_PACKAGE_curl=y` | HTTP клиент |
| `CONFIG_PACKAGE_openssh-sftp-server=y` | SFTP доступ |

### Что НЕ нужно

| Параметр | Почему |
|----------|--------|
| `CONFIG_KERNEL_DEVMEM=y` | Не нужен — lcd_drv.ko использует ioremap, не /dev/mem |

## Деплой на роутер

### Полная прошивка (sysupgrade)

```bash
# Через LuCI
System → Backup/Flash → Flash new firmware → sysupgrade.bin (без сохранения настроек)

# Через U-Boot Recovery
# 1. Выключить роутер
# 2. PC IP = 192.168.1.3, маска 255.255.255.0
# 3. Зажать Reset + включить
# 4. http://192.168.1.1 → загрузить sysupgrade.bin
# 5. Ждать ~10 мин, экран мигнёт → power cycle
# 6. Ждать 11 мин (jffs2 init)
```

### Только модуль (без пересборки прошивки)

```bash
# На Mac
cd Securifi-Almond-3S
./build.sh deploy-run
```

Или вручную:
```bash
scp lcd_drv.ko root@192.168.11.1:/tmp/
ssh root@192.168.11.1 'rmmod lcd_drv; rmmod i2c_mt7621; insmod /tmp/lcd_drv.ko'
```

### Запуск LCD UI

```bash
ssh root@192.168.11.1
rmmod i2c_mt7621 2>/dev/null    # обязательно! конфликтует с тачем
/tmp/lcd_render &                # рендер-сервер (unix socket)
lua /tmp/lcd_ui.lua &            # UI с кнопками
```

## Известные проблемы

| Проблема | Причина | Решение |
|----------|---------|---------|
| Тач не работает | kmod-i2c-mt7621 захватывает SM0 | `rmmod i2c_mt7621` перед загрузкой lcd_drv |
| OpenVPN не ставится | Нет kmod-tun в прошивке | Добавить `CONFIG_PACKAGE_kmod-tun=y` в .config |
| lcd_render текст не рисуется | JSON парсер путал ключ "text" с значением cmd | Исправлено в текущей версии |
| lcd_render server mode не обновляет экран | Не было llseek в fops ядра | Исправлено: `.llseek = default_llseek` |
| Цвета #07E0 показывают чёрный | Парсер ожидал только #RRGGBB (7 символов) | Исправлено: поддержка #XXXX как raw RGB565 |
