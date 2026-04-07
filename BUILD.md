# Сборка Securifi Almond 3S — модули LCD/UI/Battery

## Требования

### Build server (Linux x86_64):
- OpenWrt SDK: клонировать `https://github.com/isublimity/openwrt_almond.git` ветка `almond-3s`
- Собрать toolchain: `make toolchain/install`
- Собрать kernel: `make target/linux/compile`

### Рабочая станция (Mac/Linux):
- [Zig](https://ziglang.org/) для кросс-компиляции userspace
- SSH доступ к build server и роутеру

## Структура файлов

```
modules/
├── lcd_drv.c          # Kernel module (LCD + Touch + PIC Battery)
├── lcd_server.c       # Userspace: render + touch + socket server  
├── data_collector.c   # Userspace: LTE/WiFi/VPN/Battery stats
├── lcd_ui.uc          # UI скрипт (ucode)
├── settings.lua       # UI конфигурация
├── pic_calib.h        # PIC calibration tables
├── sm0_shared.h       # SM0 register definitions
└── splash_4pda.h      # Boot splash data
```

## Сборка

### 1. Настройка
```bash
cp build_config.sh.example build_config.sh
# Заполнить:
#   BUILD_SERVER="user@your-server"
#   ROUTER="root@192.168.11.1"
```

### 2. Kernel module (lcd_drv.ko)
```bash
./build.sh kernel
```
Собирается на build server через SSH. Требует OpenWrt kernel headers.

### 3. Userspace бинарники
```bash
./build.sh userspace
```
Собирается **локально** через `zig cc`. Статическая линковка, mipsel-linux-musleabi.

### 4. Всё вместе
```bash
./build.sh all        # kernel + userspace
./build.sh deploy     # деплой на роутер
./build.sh firmware   # полная прошивка OpenWrt
```

## Ручная сборка (без build.sh)

### Kernel module:
```bash
# На build server:
cd /path/to/openwrt_almond
KDIR=build_dir/target-mipsel_24kc_musl/linux-ramips_mt7621/linux-6.12.74
CROSS=staging_dir/toolchain-mipsel_24kc_gcc-14.3.0_musl/bin/mipsel-openwrt-linux-musl-

cp modules/lcd_drv.c modules/pic_calib.h modules/sm0_shared.h modules/splash_4pda.h \
   package/lcd-gpio/src/

make -C $KDIR M=$(pwd)/package/lcd-gpio/src ARCH=mips CROSS_COMPILE=$CROSS modules
# Результат: package/lcd-gpio/src/lcd_drv.ko
```

### Userspace:
```bash
# Локально (Mac/Linux с zig):
zig cc -target mipsel-linux-musleabi -Os -static -lpthread \
    -o lcd_server modules/lcd_server.c

zig cc -target mipsel-linux-musleabi -Os -static \
    -o data_collector modules/data_collector.c
```

## Деплой

```bash
# Бинарники:
scp lcd_server data_collector root@192.168.11.1:/usr/bin/
scp modules/lcd_ui.uc root@192.168.11.1:/usr/bin/
scp modules/settings.lua root@192.168.11.1:/etc/lcd/

# Kernel module:
KVER=$(ssh root@192.168.11.1 'uname -r')
scp lcd_drv.ko root@192.168.11.1:/lib/modules/$KVER/

# Перезагрузить модуль:
ssh root@192.168.11.1 'rmmod lcd_drv; insmod /lib/modules/$(uname -r)/lcd_drv.ko'
```

## Версионирование

Формат: **V{YY}{MM}{NN}** (год, месяц, номер сборки)

При старте каждый модуль пишет в dmesg/stderr:
```
<module_name> V260401 by Sublimity — START
```

При обновлении версии — изменить `VERSION` / `LCD_DRV_VERSION` в:
- `lcd_drv.c` (строка `#define LCD_DRV_VERSION`)
- `lcd_server.c` (строка `#define VERSION`)
- `data_collector.c` (строка `#define VERSION`)

## Проверка

```bash
# Kernel module:
dmesg | grep "lcd_drv.*Sublimity"
# lcd_drv V260401 by Sublimity — START (fb=320x240, 153600 bytes)

# Userspace:
lcd_server 2>&1 | head -1
# lcd_server V260401 by Sublimity — START

data_collector 2>&1 | head -1
# data_collector V260401 by Sublimity — START

# Battery:
cat /tmp/lcd_data.json | grep battery
# "battery":{"adc":810,"percent":64,"charging":true,"valid":true}
```
