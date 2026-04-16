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
- [x] Индикация батареи на dashboard (cmd 0x39 + 0x36 → live ADC)
- [ ] SMS чтение (AT+CMGL через Fibocom)
- [ ] Баланс SIM (USSD)
- [ ] Buzzer при нажатии кнопок
- [x] Оставшееся время работы от батареи (lookup table + linreg, int64)
- [x] NO BAT детекция (buf[5] bit5+bit6)
- [x] Процент исправлен (max ADC=750, было 1023)

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

- [x] WireGuard: настроен
- [x] OpenVPN: настроен
- [x] L2TP: настроен (xl2tpd)
- [x] Hotplug 90-wg-route: route только после handshake
- [x] Firewall: tun0 в wan zone (masquerade для OpenVPN)
- [x] UI: 4 кнопки выбора VPN + VPN OFF

## PIC16LF1509 Battery

### Статус: РЕШЕНО (7 апреля 2026)

- [x] PIC firmware dumped via PICkit 3 + MPLAB IPE v6.05
- [x] Full firmware analysis (30+ functions, 14 I2C commands, all RAM vars)
- [x] Cmd 0x39 (SSP REINIT) → cmd 0x36 (ADC READ) → read 6 bytes — РАБОТАЕТ
- [x] ADC 10-bit: (buf[1]<<2)|(buf[2]>>6), валидация buf[3]==0x02 && buf[4]==0x04
- [x] Charger detection: buf[5] bit0 (0x00=нет, 0x01/0x09=да)
- [x] No-battery detection: buf[5] bit5+bit6 (0x60 mask)
- [x] Calibration tables (pic_calib.h) = MELODY DATA, not battery!
- [x] LED control: cmd 0x32=ON, 0x31=OFF, 0x30=BLINK (tested!)
- [x] Buzzer mystery solved (ISR PORTE.5 LOW auto-play, Timer1 PORTC toggle)
- [x] Периодическое чтение в kernel (touch thread, каждые ~10 сек)
- [x] Battery drain test: 745→68 ADC за 4.13 часа (1213 строк)
- [x] Battery charge test: 499→802 ADC (788 строк)
- [x] Оставшееся время (lookup table + linreg int64, Battery_Drain_ALGO.md)
- [x] Процент исправлен: max ADC=750 (было 1023)
- [ ] Buzzer через I2C (multi-byte SM0 write сломан — только 1-byte работает)

## Прошивка

### Статус: OpenWrt 24.10.6 upstream, ядро 6.6.127, vermagic compatible

- [x] DTS: `&ethphy0 { /delete-property/ interrupts; }` — критично для LAN
- [x] kmod-lcd-gpio: AutoLoad,90
- [x] GCC 14.3 (совпадает с ядром)
- [x] opkg в сборке
- [x] VPN пакеты: WireGuard, OpenVPN, L2TP (xl2tpd)
- [x] OpenWrt package: lcd-gpio (kernel) + lcd-ui (userspace)
- [x] first_setup.sh + install_to_router.sh (единый скрипт, без дублей)
- [x] IRQ affinity исправлен: 30=USB, 33=WiFi2.4, 32=WiFi5
- [x] GPIO reset pulse 3s→1s
- [x] Прошивка собрана (11MB sysupgrade.bin, 8 апреля 2026)
- [x] MTD backup (BACKUP/2026-04-08/)
- [ ] lcd-ui пакет тянет исходники из GitHub при сборке
- [ ] Обновить fildunsky_openwrt до свежего upstream (elfutils патчи несовместимы)
- [ ] Перейти на стабилку 24.10.6 или 25.12.2 (snapshot протухает)
- [ ] Рассмотреть NCM+xmm вместо MBIM (на случай обновления прошивки модема)
