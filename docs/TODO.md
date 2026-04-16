# TODO — Securifi Almond 3S

## LCD UI

### Статус: СТАБИЛЬНО РАБОТАЕТ

- [x] Dashboard: LTE/VPN/WiFi/system stats с типом VPN
- [x] Touch menu: 6 кнопок, 2 страницы
- [x] Sub-pages: VPN, LTE (графики), WiFi, Info, IP, Traffic
- [x] Линейные графики: RSRQ, Traffic, Ping
- [x] WiFi клиенты: имя + IP + band + signal + traffic
- [x] Screensaver: bouncing clock + battery % + WWAN traffic
- [x] Anti-burn-in: pixel shift + backlight off
- [x] Boot sequence: demoscene 3с → dmesg → UI
- [x] Anti-tearing: snapshot buffer + fb_writing flag
- [x] data_collector: AT serial I/O, CESQ, XCCINFO, XLEC
- [x] Батарея: %, remaining time, charge time, NO BAT
- [x] Info page: battery raw hex + ADC
- [ ] SMS чтение (AT+CMGL)
- [ ] Баланс SIM (USSD)
- [ ] Buzzer при нажатии кнопок

## LTE модем (Fibocom L860-GL)

### Статус: РАБОТАЕТ (Cat16, MBIM)

- [x] MBIM через umbim (GTUSBMODE=7, firmware Jul 2020)
- [x] AT port auto-detect (ttyACM2)
- [x] GPIO reset 1s pulse
- [x] first_setup.sh: автонастройка APN + reset
- [x] Interface name: wwan (стандартное)
- [ ] LTE watchdog как procd сервис
- [ ] **НЕ обновлять firmware модема!** (MBIM пропадёт)

## VPN

### Статус: РАБОТАЕТ (через opkg)

- [x] WireGuard: opkg install + secrets.sh
- [x] OpenVPN: opkg install
- [x] L2TP: opkg install (xl2tpd)
- [x] Hotplug 90-wg-route
- [x] VPN скрипты в first_setup.sh

## PIC16LF1509 Battery

### Статус: РЕШЕНО

- [x] cmd 0x39 (SSP REINIT) → cmd 0x36 (ADC READ) → read 6 bytes
- [x] Процент: `bat_table_lookup(adc) * 100 / 170` (по кривой разряда)
- [x] Full charge = ADC 800, dead = ADC 400
- [x] Charger: buf[5] bit0, No-battery: buf[5] bit5+bit6
- [x] Remaining time: discharge table (17 точек) + linreg
- [x] Charge time: charge table (17 точек, CV-phase)
- [x] Battery: 18650-251P, 2S, 3200mAh, 23.68Wh
- [x] PIC firmware fully reverse-engineered
- [ ] Buzzer через I2C (multi-byte SM0 write сломан)

## Прошивка

### Статус: OpenWrt 24.10.6 upstream, ядро 6.6.127

- [x] Upstream OpenWrt + almond3s.patch (PR #22141)
- [x] Vermagic `f31f6f85...` = official (opkg kmod compatible)
- [x] Single device build (securifi_almond-3s)
- [x] Пакеты lcd-gpio + lcd-ui из GitHub при сборке
- [x] Default IP 192.168.11.1
- [x] VPN через opkg (vermagic совпадает)
- [x] first_setup.sh + install_to_router.sh
- [ ] Включить WireGuard в прошивку (build-time crypto dependency issue)
- [ ] Рассмотреть NCM+xmm вместо MBIM
- [ ] CI/CD автосборка прошивки
