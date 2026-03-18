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
# Ожидание: Almond3S (2.4G) + Almond3S-5G (5G)
```

### 3. LCD
```bash
lsmod | grep lcd
# Ожидание: lcd_drv loaded, дисплей показывает splash
```

### 4. LTE модем
```bash
ls /dev/ttyUSB* /dev/cdc-wdm0
# Ожидание: ttyUSB0-3 + cdc-wdm0
uqmi -d /dev/cdc-wdm0 --get-serving-system
# Ожидание: registration=registered
```

Если SIM "illegal state":
```bash
killall -9 uqmi
echo -ne "AT+CFUN=1,1\r\n" > /dev/ttyUSB2  # restart modem
sleep 20
ifup lte
```

### 5. VPN (OpenVPN)
```bash
pidof openvpn
tail -5 /tmp/ovpn.log
ip addr show tun0 | grep inet
curl -s ifconfig.me  # должен показать IP VPN сервера
```

### 6. WiFi клиенты через VPN

**Критическая проблема:** OpenWrt nftables НЕ добавляет tun0 (OpenVPN) в firewall автоматически. Три правила нужны:

1. **srcnat** — tun0 должен прыгать в srcnat_wan (masquerade)
2. **srcnat_wan** — masquerade для br-lan→tun0
3. **forward_lan** — разрешить forwarding lan→tun0

```bash
# Главное правило! Без него masquerade НЕ работает для tun0:
nft add rule inet fw4 srcnat oifname "tun0" jump srcnat_wan
# Masquerade:
nft insert rule inet fw4 srcnat_wan iifname "br-lan" oifname "tun0" meta nfproto ipv4 masquerade
# Forward:
nft insert rule inet fw4 forward_lan oifname "tun0" counter accept
```

Без `srcnat → srcnat_wan` jump: роутер сам ходит через VPN, пакеты клиентов доходят до tun0, но source IP не NAT-ится (192.168.11.x вместо 10.8.0.x) → ответы не возвращаются.

**Автоматизация** — в `/etc/openvpn/altair.ovpn`:
```
script-security 2
up /etc/openvpn/up.sh
```

`/etc/openvpn/up.sh`:
```bash
#!/bin/sh
sleep 2
nft add rule inet fw4 srcnat oifname "tun0" jump srcnat_wan 2>/dev/null
nft insert rule inet fw4 forward_lan oifname "tun0" counter accept 2>/dev/null
logger -t openvpn 'tun0 firewall rules applied'
```

### 7. LTE defaultroute

```bash
uci set network.lte.defaultroute='0'
uci commit network
```

Если defaultroute=1 — LTE добавит `default via wwan0` который перебьёт VPN routes. С defaultroute=0 — OVPN управляет default route через `0.0.0.0/1` + `128.0.0.0/1`.

### 8. WireGuard (альтернатива OVPN)

```bash
uci set network.wg0_peer.route_allowed_ips='1'  # default через WG
uci commit network
ifup wg0
```

WG и OVPN нельзя запускать одновременно с default route — только один.

### 9. Buzzer
```bash
# НЕ отправлять {0x41} — включает buzzer!
# Buzzer ON:  pic_i2c_cmd 0x34 0x00 0x03 (после 0x41 init)
# Buzzer OFF: pic_i2c_cmd 0x34 0x00 0x00
# lcd_drv НЕ включает buzzer (palmbus калибровка безопасна)
```

### 10. Modem GPIO Reset
```bash
# НЕ ИСПОЛЬЗОВАТЬ echo 0 > /sys/class/gpio/modem_reset/value
# Это убивает модем, возврат только через power cycle роутера!
```

## Конфигурационные файлы

| Файл | Содержимое |
|------|-----------|
| /etc/config/network | LAN, LTE (QMI), WG |
| /etc/config/wireless | WiFi AP 2.4+5GHz |
| /etc/config/firewall | Zones, forwarding, masquerade |
| /etc/openvpn/altair.ovpn | OpenVPN client config |
| /etc/init.d/openvpn-altair | OVPN autostart script |

## Типичные проблемы

| Симптом | Причина | Решение |
|---------|---------|---------|
| WiFi клиенты без интернета | tun0 не в firewall | nft insert masquerade для tun0 |
| LTE disconnected долго | SIM illegal state | AT+CFUN=1,1, killall uqmi, ifup lte |
| Buzzer пищит | Отправлен {0x41} | Power cycle (только так) |
| Модем пропал | GPIO reset | Power cycle роутера |
| WG 0 received | Routing loop или сервер down | Проверить endpoint route через wwan0 |
