# Сборка OpenWrt для Securifi Almond 3S

## Текущая стабильная сборка

- **Репо**: fildunsky_openwrt (форк OpenWrt 25.12.0)
- **Ядро**: 6.12.74
- **GCC**: 14.3.0
- **Коммит**: `00d105224d` (ramips: add support Securifi Almond 3S)

## Быстрая сборка прошивки

```bash
# На сервере сборки
cd /mnt/sata/var/openwrt/fildunsky_openwrt
./build-almond.sh
```

Результат: `bin/targets/ramips/mt7621/openwrt-*-sysupgrade.bin` (~11 MB)

## Сборка только модуля lcd_drv.ko

```bash
FILD=/mnt/sata/var/openwrt/fildunsky_openwrt
KDIR=$FILD/build_dir/target-mipsel_24kc_musl/linux-ramips_mt7621/linux-6.12.74
CROSS=$FILD/staging_dir/toolchain-mipsel_24kc_gcc-14.3.0_musl/bin/mipsel-openwrt-linux-musl-

make -C $KDIR M=$FILD/package/lcd-gpio/src ARCH=mips CROSS_COMPILE=$CROSS modules
```

## Сборка userspace (Mac, zig cc)

```bash
zig cc -target mipsel-linux-musleabi -Os -static -o lcd_render modules/lcd_render.c
zig cc -target mipsel-linux-musleabi -Os -static -o touch_poll modules/touch_poll.c
zig cc -target mipsel-linux-musleabi -Os -static -o data_collector modules/data_collector.c
```

Или: `./build.sh userspace`

## DTS — критические настройки

```dts
&i2c {
    status = "okay";
};

&ethphy0 {
    /delete-property/ interrupts;  /* КРИТИЧНО! Без этого IRQ #23 убивает LAN */
};

&state_default {
    lcd_jtag {
        groups = "jtag";
        function = "gpio";        /* LCD пины GPIO 13-17 */
    };
};
```

**ВАЖНО**: `&ethphy0 { /delete-property/ interrupts; }` — без этого SM0 touch операции убивают MT7530 IRQ #23 → LAN мёртв.

## Пакет lcd-gpio

```
package/lcd-gpio/
├── Makefile
└── src/
    ├── Makefile          # obj-m += lcd_drv.o
    ├── lcd_drv.c
    ├── splash_4pda.h
    └── pic_calib.h
```

```makefile
# package/lcd-gpio/Makefile
define KernelPackage/lcd-gpio
  SUBMENU:=Other modules
  TITLE:=ILI9341 LCD + SX8650 Touch + PIC16 Battery
  FILES:=$(PKG_BUILD_DIR)/lcd_drv.ko
  AUTOLOAD:=$(call AutoLoad,90,lcd_drv)
endef
```

## Пакеты в прошивке

| Категория | Пакеты |
|-----------|--------|
| LCD | kmod-lcd-gpio, i2c-tools, socat |
| LTE | kmod-usb-net-cdc-mbim, kmod-usb-acm, umbim, luci-proto-mbim |
| VPN | kmod-wireguard, wireguard-tools, openvpn-openssl, kmod-tun, xl2tpd, kmod-l2tp, kmod-pppol2tp |
| DNS | https-dns-proxy |
| UI | luci-ssl, luci-i18n-base-ru |
| Utils | nano, curl, tcpdump, openssh-sftp-server, picocom |

## Прошивка роутера

### U-Boot Recovery
1. PC IP: `192.168.1.3`, маска `255.255.255.0`
2. Зажать Reset + включить роутер
3. Открыть `http://192.168.1.1`
4. Загрузить sysupgrade.bin
5. Ждать ~10 мин → power cycle → ждать 11 мин (jffs2)

### После прошивки
```bash
scp first_setup.sh root@192.168.11.1:/tmp/
ssh root@192.168.11.1 'sh /tmp/first_setup.sh --all'
```

## Загрузка lcd_drv.ko

При boot:
1. `modules.d/99-lcd-drv` → загрузка модуля (ранняя, logo + boot console)
2. `S99lcd_ui` init → `echo touch_start > /dev/lcd` → touch + lcd_render + UI

Touch деферирован — SM0 операции безопасны после полной инициализации MT7530.

## /dev/lcd интерфейс

| ioctl | arg | Описание |
|-------|-----|----------|
| 0 | — | flush framebuffer, stop splash |
| 1 | int[3] | read touch {x, y, pressed} |
| 2 | u8[17] | read PIC battery (disabled) |
| 4 | 0/1/2 | backlight OFF/ON/splash |
| 5 | 0-5/99/100 | scene select/random/stop |
| write | "touch_start" | init SX8650 + start touch thread |
| write | raw bytes | framebuffer data (153600 bytes) |

## Известные ограничения

- **gpio_request() НЕЛЬЗЯ** — ломает MT7530 IRQ #23
- **PIC battery ОТКЛЮЧЁН** — SM0 операции конфликтуют с MT7530
- **daemon() из musl** ломает fd — использовать fork()+setsid()
- **Reboot не работает** — PIC16 контролирует питание, только кнопкой
- **Fibocom L860-GL** — нужен GPIO reset при каждом boot
