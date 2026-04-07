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
- [ ] Индикация батареи на dashboard (требует решение live ADC)

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
- [ ] Xray/sing-box: не установлен (нет в прошивке)

## PIC16LF1509 Battery

### Статус: ПРОШИВКА ПОЛНОСТЬЮ ПРОАНАЛИЗИРОВАНА, live ADC не работает

**Прорыв 2026-03-24**: PIC firmware дампнута (PICkit 3, CP=OFF), полный дизассемблер анализ.

- [x] PIC firmware dumped via PICkit 3 + MPLAB IPE v6.05
- [x] Full firmware analysis (30+ functions, 14 I2C commands, all RAM vars)
- [x] Cmd 0x36 = ONLY way to get live ADC (reads AN11 channel)
- [x] Cmd 0x37 = firmware version (returns 0x07)
- [x] Calibration tables (pic_calib.h) = MELODY DATA, not battery!
- [x] LED control: cmd 0x32=ON, 0x31=OFF, 0x30=BLINK (tested!)
- [x] GPIO mapping confirmed (PORTE.0/3/4/5, PORTA.0/1, PORTC.0, AN11)
- [x] Buzzer mystery solved (ISR PORTE.5 LOW auto-play, not I2C cmd)
- [x] SM0CTL0 difference found (stock: 0x8064800E vs ours: 0x01F3800F)
- [x] RSTCTRL I2C reset safe for MT7530 LAN
- [x] Separate pic_battery.ko module — failed (SM0 sharing too fragile)
- [x] i2c-mt7621.c analyzed (same NEW SM0 regs, iowrite32 vs __raw_writel)
- [x] OLD SM0 auto read = dead end (SM0_CFG read-only on eco:3)
- [ ] **TEST**: cmd 0x36 write + NEW SM0 read 3 bytes (live ADC)
- [ ] **TEST**: cmd 0x37 read = 0x07 (verify read with cmd_state != 0)
- [ ] **TEST**: SM0CTL0=0x8064800E (stock) + cmd 0x36 cycle
- [ ] **FALLBACK**: GPIO bit-bang I2C with clock stretching support
- [ ] Убрать pic_calib.h из lcd_drv init (или пометить как melody data)
- [ ] Заменить {0x2F, 0, 2} на {0x36} в polling

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
