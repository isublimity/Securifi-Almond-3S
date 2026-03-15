# Device Packages — Securifi Almond 3S

## Текущее определение устройства

```makefile
# target/linux/ramips/image/mt7621.mk
define Device/securifi_almond-3s
  $(Device/dsa-migration)
  IMAGE_SIZE := 65408k
  DEVICE_VENDOR := Securifi
  DEVICE_MODEL := Almond 3S
  DEVICE_PACKAGES := kmod-usb3 kmod-usb-net-qmi-wwan kmod-usb-serial-option uqmi kmod-lcd-gpio
endef
```

## Критические пакеты

### WiFi (встроены в платформу ramips/mt7621)

Эти модули подтягиваются автоматически через target platform, НЕ нужно указывать в DEVICE_PACKAGES:

| Пакет | Чип | Зачем |
|-------|-----|-------|
| kmod-mt7615e | MT7615 | Основной WiFi драйвер (2.4 + 5 ГГц) |
| kmod-mt7615-firmware | MT7615 | Firmware для MT7615 |

### USB (DEVICE_PACKAGES)

| Пакет | Зачем | Критичность |
|-------|-------|-------------|
| **kmod-usb3** | xHCI контроллер USB 3.0/2.0 | **ОБЯЗАТЕЛЕН** — без него USB порт не работает (модем, флешки) |

### LTE модем (DEVICE_PACKAGES)

| Пакет | Зачем | Критичность |
|-------|-------|-------------|
| **kmod-usb-net-qmi-wwan** | QMI протокол для Quectel EC21-E | **ОБЯЗАТЕЛЕН** для LTE — создаёт сетевой интерфейс wwan0 |
| **kmod-usb-serial-option** | USB serial для AT-команд модема | **ОБЯЗАТЕЛЕН** — создаёт /dev/ttyUSB0-3, без него нет AT-порта |
| **uqmi** | Утилита управления QMI модемом | **ОБЯЗАТЕЛЕН** — подключение/отключение LTE, PIN, APN |

### Дисплей и тачскрин (DEVICE_PACKAGES)

| Пакет | Зачем | Критичность |
|-------|-------|-------------|
| **kmod-lcd-gpio** | Наш модуль lcd_drv.ko — ILI9341 дисплей + SX8650 тачскрин + PIC battery | **ОБЯЗАТЕЛЕН** — без него нет /dev/lcd, экран не работает |

### I2C (build-almond-3s.sh)

| Пакет | Зачем | Критичность |
|-------|-------|-------------|
| **kmod-i2c-core** | Ядро I2C подсистемы Linux | **ОБЯЗАТЕЛЕН** — lcd_drv.ko использует i2c_transfer() для PIC battery |
| **kmod-i2c-mt7621** | I2C контроллер MT7621 (palmbus SM0) | **ОБЯЗАТЕЛЕН** — физический I2C bus 0, без него нет /dev/i2c-0 |
| i2c-tools | Утилиты i2cdetect, i2cget, i2cset, i2ctransfer | Полезно для отладки, не критично |

## Рекомендуемое определение DEVICE_PACKAGES

```makefile
DEVICE_PACKAGES := kmod-usb3 \
    kmod-usb-net-qmi-wwan kmod-usb-serial-option uqmi \
    kmod-lcd-gpio kmod-i2c-core kmod-i2c-mt7621
```

### Почему именно так

- **kmod-usb3** — единственный USB порт роутера, через него модем и внешние устройства
- **kmod-usb-net-qmi-wwan + kmod-usb-serial-option + uqmi** — LTE модем Quectel EC21-E (miniPCIe → USB). Без всех трёх модем не заработает
- **kmod-lcd-gpio** — наш lcd_drv.ko. Подтягивается из `package/lcd-gpio/` в OpenWrt сборке. Делает: framebuffer /dev/lcd, touch ioctl, PIC battery ioctl
- **kmod-i2c-core + kmod-i2c-mt7621** — I2C шина. lcd_drv.ko использует palmbus напрямую для тачскрина, но i2c_transfer() для PIC battery. Без I2C модулей PIC чтение не работает

### Что НЕ нужно в DEVICE_PACKAGES

| Пакет | Почему не нужен |
|-------|----------------|
| kmod-mt7603 | Это другой чип WiFi (не используется в Almond 3S) |
| kmod-mt76x2 | Это MT76x2, в Almond 3S стоит MT7615 |
| kmod-mt7663-firmware-ap | Firmware для MT7663, не наш чип |
| -uboot-envtools | Минус = исключить. U-Boot env через MTD, не нужно |

## Дополнительные пакеты (в .config, не DEVICE_PACKAGES)

Эти пакеты добавляются через `build-almond-3s.sh` в `.config`:

| Категория | Пакеты | Зачем |
|-----------|--------|-------|
| VPN | wireguard-tools, kmod-wireguard, openvpn-openssl | WireGuard и OpenVPN |
| DNS | https-dns-proxy | DNS-over-HTTPS |
| LuCI | luci, luci-ssl, luci-i18n-base-ru | Веб-интерфейс + русский |
| LTE доп. | kmod-usb-net-cdc-mbim | MBIM протокол (альтернатива QMI) |
| Утилиты | htop, nano, curl, tcpdump, openssh-sftp-server | Отладка и удобство |

## Конфигурация ядра

| Опция | Нужна? | Зачем |
|-------|--------|-------|
| CONFIG_KERNEL_DEVMEM=y | **НЕТ** | Была нужна для lcd_gpio.ko (mmap /dev/mem из userspace). lcd_drv.ko использует ioremap внутри ядра — /dev/mem не требуется |

## Совместимость версий OpenWrt

| Версия | Ядро | Статус | Примечание |
|--------|------|--------|------------|
| **24.10.x** | **6.6.127** | **РАБОТАЕТ** | Единственная рабочая ветка. U-Boot совместим |
| 25.12.0 (main) | 6.12.71+ | **КИРПИЧ** | U-Boot НЕ грузит ядро 6.12.x. Роутер зависает. Восстановление только через U-Boot Recovery |

**ВАЖНО**: Использовать **ТОЛЬКО** ветку `almond-3s` (на базе `openwrt-24.10`). Ветка `main` (25.12.0) несовместима с U-Boot и превращает роутер в кирпич. Проверено экспериментально.

## Размер прошивки

- Flash: 64 МБ SPI-NOR (IMAGE_SIZE = 65408k ≈ 63.9 МБ)
- Текущая прошивка: ~6.7 МБ sysupgrade.bin
- Место для пакетов: более чем достаточно
