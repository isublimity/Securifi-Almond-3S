# First Boot Check & Setup

## Порядок проверки после прошивки

### 1. LAN
```bash
ip link show | grep 'state UP'
# Ожидание: eth0, lan2, br-lan = UP
```

### 2. WiFi
```bash
iwinfo | grep ESSID
# Ожидание: Almond-5G (5G, ch165) + Almond (2.4G, ch6)
```

### 3. LCD
```bash
lsmod | grep lcd
# Ожидание: lcd_drv loaded, дисплей показывает splash/boot console
ls /dev/lcd
```

### 4. LTE модем (Fibocom L860-GL)
```bash
ls /dev/cdc-wdm0 /dev/ttyACM*
# Ожидание: cdc-wdm0 + ttyACM0-2
umbim -n -d /dev/cdc-wdm0 caps
# Ожидание: MBIM responds
```

Если модем не определяется — GPIO reset:
```bash
MODEM_GPIO="/sys/devices/platform/1e000000.palmbus/1e000600.gpio/gpiochip1/gpio/modem_reset/value"
echo 1 > "$MODEM_GPIO"; sleep 3; echo 0 > "$MODEM_GPIO"
sleep 15
ifup lte
```

### 5. VPN
```bash
# WireGuard:
ifup wg0
wg show wg0
ip route | grep wg0

# OpenVPN:
openvpn --config /etc/openvpn/sirius.ovpn --daemon
ip addr show tun0 | grep inet

# Проверка:
curl -s ifconfig.me   # IP VPN сервера
```

WG и OVPN нельзя запускать одновременно с default route — только один.

### 6. Firewall для VPN
```bash
# tun0 (OpenVPN) должен быть в wan zone:
uci show firewall | grep vpn
# network vpn = tun0, proto none
```

WG hotplug (`/etc/hotplug.d/iface/90-wg-route`) автоматически добавляет route после handshake.

### 7. Battery (PIC16LF1509)
```bash
dmesg | grep "PIC bat:"
cat /tmp/lcd_data.json | grep battery
# {"adc": 810, "percent": 64, "charging": true, "valid": true}
```

### 8. GPIOMODE (критично!)
```bash
devmem 0x1E000060
# Должно быть 0x00048580 (bit2=0 = SM0 → I2C пины)
# Если bit2=1 — PIC и SX8650 touch не работают!
```

### 9. LCD UI процессы
```bash
ps w | grep -E "lcd_render|data_col|touch_poll|ucode"
# Ожидание: 4 процесса (lcd_render, data_collector, touch_poll, ucode lcd_ui.uc)
```

## Настройка

```bash
# С Mac — одной командой:
./install_to_router.sh              # полная настройка + VPN ключи
./install_to_router.sh --public     # без приватных ключей

# Или вручную:
scp first_setup.sh root@192.168.11.1:/tmp/
ssh root@192.168.11.1 'sh /tmp/first_setup.sh --all'
```

## Типичные проблемы

| Симптом | Причина | Решение |
|---------|---------|---------|
| WiFi клиенты без интернета через VPN | tun0 не в firewall | `first_setup.sh --setup_vpn` |
| Модем пропал | Нужен GPIO reset | `sh /etc/lcd/scripts/lte_reset.sh` |
| Buzzer пищит | Отправлен cmd 0x41 | Power cycle (только так) |
| LAN не работает | gpio_request на bank0 | Проверить DTS: ethphy0 delete interrupts |
| Battery valid:false | SSPOV в PIC | Cmd 0x39 перед каждым read (lcd_drv делает автоматически) |
| Touch не реагирует | touch_start не отправлен | `echo -n "touch_start" > /dev/lcd` |
| WG 0 received | Routing loop или сервер down | Проверить endpoint route через wwan0 |
