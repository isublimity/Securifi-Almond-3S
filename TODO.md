# TODO — Securifi Almond 3S

## LCD UI

### Статус: СТАБИЛЬНО РАБОТАЕТ

- [x] Dashboard: LTE/VPN/WiFi/system stats с типом VPN (WG/OVPN/L2TP)
- [x] Touch menu: 6 кнопок, 2 страницы
- [x] Sub-pages: VPN (4 кнопки выбора), LTE (графики RSRQ/Traffic/Ping), WiFi, Info, IP, Traffic
- [x] VPN: WireGuard, OpenVPN, L2TP — выбор через UI, shell scripts
- [x] Линейные графики: RSRQ (качество), Traffic (RX/TX), Ping с порогами
- [x] WiFi клиенты: имя + IP + band (5G/2G) + signal + traffic
- [x] Screensaver: bouncing clock + backlight off
- [x] Anti-burn-in: pixel shift +-2px каждые 30 сек
- [x] Toast уведомления, анимация кнопок, splash при действиях
- [x] Автозапуск: modules.d (lcd_drv ранний) + S99 init.d (touch + UI)
- [x] Boot console: dmesg на экране при загрузке
- [x] data_collector: direct C serial I/O, auto-detect AT port (ACM0-2)
- [x] data_collector: CESQ (RSRP/RSRQ), XCCINFO (Band/PCI), VPN type detection
- [x] PID lock (data_collector + lcd_ui.uc — без дубликатов)
- [ ] SMS чтение (AT+CMGL через Fibocom)
- [ ] Баланс SIM (USSD)
- [ ] Buzzer при нажатии кнопок

## LTE модем (Fibocom L860-GL)

### Статус: РАБОТАЕТ (Cat16, CA до 5CC)

- [x] MBIM подключение через umbim
- [x] Beeline SIM, B3 (1800 MHz)
- [x] AT port auto-detect (ttyACM2 обычно)
- [x] XACT бенды: B1, B3, B7, B8, B20, B38
- [x] Carrier Aggregation поддерживается (XLEC: до 5CC)
- [x] GPIO reset при зависании модема
- [x] first_setup.sh: автонастройка APN + GPIO reset
- [ ] LTE watchdog как procd сервис
- [ ] Автопереключение на лучший Band

## VPN

### Статус: РАБОТАЕТ (3 типа)

- [x] WireGuard: Tina → Freak exit (82.22.184.238)
- [x] OpenVPN: Tina (sirius.ovpn, TCP 8443)
- [x] L2TP: настроен (xl2tpd)
- [x] Hotplug 90-wg-route: route только после handshake
- [x] Firewall: tun0 в wan zone (masquerade для OpenVPN)
- [x] UI: 4 кнопки выбора VPN + VPN OFF
- [ ] Xray/sing-box: не установлен (нет в прошивке)

## PIC16LF1509 Battery

### Статус: ОТКЛЮЧЁН (ломает MT7530 IRQ #23)

PIC SM0 операции убивают MT7530 Ethernet IRQ. Отключён в lcd_drv.ko.

- [x] ADC читается (423=LOW, 591=NORMAL)
- [x] Бипер работает (мелодии)
- [ ] Нужен способ I2C без SM0 конфликта (отдельный I2C контроллер или GPIO bit-bang с задержкой)

## Прошивка

### Статус: fildunsky_openwrt, ядро 6.12.74

- [x] DTS: `&ethphy0 { /delete-property/ interrupts; }` — критично для LAN
- [x] kmod-lcd-gpio: AutoLoad,90
- [x] GCC 14.3 (совпадает с ядром)
- [ ] Добавить sing-box/xray в сборку
- [ ] opkg не установлен (нужен для runtime пакетов)

## U-Boot

### Статус: ИССЛЕДОВАНИЕ (ВЫСОКИЙ РИСК)

- [ ] USB Recovery
- [ ] LCD в U-Boot
- Нужен CH341A + SOIC8 для восстановления
