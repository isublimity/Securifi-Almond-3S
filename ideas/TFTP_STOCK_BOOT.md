# Загрузка стокового ядра 3.10.14 через TFTP

## Цель
Загрузить стоковое ядро в RAM (без прошивки!) и проверить, работает ли PIC battery read
на нашем MT7621 eco:3. Если да — разница в ядре/драйвере. Если нет — PIC firmware проблема.

## Стоковое ядро
- Файл: `BACKUP/MTD4_KER.BIN`
- Формат: uImage (LZMA compressed)
- Size: 14.7 MB
- Load: 0x80001000
- Entry: 0x80523DC0
- Kernel: Linux 3.10.14

## Шаг 1: Подготовка Mac

```bash
# Копировать ядро в TFTP директорию
sudo cp /Users/igor/Yandex.Disk.localized/projects/almond/BACKUP/MTD4_KER.BIN /private/tftpboot/stock.bin
sudo chmod 644 /private/tftpboot/stock.bin

# Запустить TFTP сервер
sudo launchctl load -F /System/Library/LaunchDaemons/tftp.plist

# Проверить что работает
sudo launchctl list | grep tftp
# или
sudo lsof -i :69
```

## Шаг 2: Настроить сеть

Подключить Mac к LAN порту роутера напрямую (не через свитч).

```bash
# Установить IP на Mac для U-Boot recovery сети
# System Preferences → Network → USB-LAN adapter:
#   IP: 192.168.1.3
#   Mask: 255.255.255.0
#   Router: 192.168.1.1
```

## Шаг 3: Войти в U-Boot

1. Выключить роутер (долгое нажатие кнопки питания)
2. Подключить USB-Serial (если есть) к UART роутера (115200 8N1)
3. Зажать Reset + включить роутер
4. В serial console (или через HTTP) войти в U-Boot

**Без serial console**: U-Boot a43 поддерживает recovery через HTTP (http://192.168.1.1),
но НЕ поддерживает TFTP boot через HTTP. Нужен serial console.

**Если есть serial console** (UART2, 115200):
- Нажать любую клавишу при загрузке чтобы остановить автобут
- Появится U-Boot prompt: `MT7621 #`

## Шаг 4: Загрузить ядро через TFTP

В U-Boot console:

```
# Установить IP
setenv ipaddr 192.168.1.1
setenv serverip 192.168.1.3

# Загрузить ядро в RAM (НЕ прошивает flash!)
tftpboot 0x80100000 stock.bin

# Загрузить стоковое ядро
bootm 0x80100000
```

**ВАЖНО**: это загружает ядро в RAM. После перезагрузки — вернётся OpenWrt.
Flash НЕ затрагивается.

## Шаг 5: Проверить PIC battery

После загрузки стокового ядра:
- Стоковая rootfs не будет работать (flash layout другой)
- Но стоковые модули AlmondPic2_main встроены в ядро
- PIC init произойдёт автоматически при загрузке

Проверить:
```bash
# Если shell доступен через serial:
dmesg | grep -i pic
dmesg | grep -i bat
cat /proc/almond_pic    # если есть

# Проверить SM0 регистры
devmem 0x1E000900       # SM0_CFG — записалось ли 0xFA?
devmem 0x1E000914       # SM0_DATAIN
devmem 0x1E000950       # N_D0
devmem 0x1E000954       # N_D1
```

## Альтернатива: Без serial console

Если нет serial console и нужно проверить без U-Boot:

### Вариант A: Прошить стоковую прошивку через U-Boot HTTP
1. Hold Reset + power on → http://192.168.1.1
2. Загрузить MTD4_KER.BIN (28 MB) ← ПЕРЕЗАПИШЕТ OpenWrt!
3. Проверить PIC
4. Перепрошить OpenWrt обратно

**РИСК**: потеря OpenWrt настроек. Но U-Boot остаётся, можно перепрошить.

### Вариант B: Initramfs OpenWrt с PIC debug
Собрать OpenWrt initramfs image с lcd_drv.ko встроенным, загрузить через TFTP.
Преимущество: OpenWrt shell + PIC debug, без перезаписи flash.

## Нужно
- **Serial console** (USB-UART TTL 3.3V) для TFTP boot
  - UART2 на плате: TX=GPIO9, RX=GPIO10
  - Baudrate: 115200

## Остановить TFTP сервер после завершения
```bash
sudo launchctl unload -F /System/Library/LaunchDaemons/tftp.plist
```
