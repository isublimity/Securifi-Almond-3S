# TODO — Securifi Almond 3S

## LCD UI

### Статус: РАБОТАЕТ

Dashboard + touch menu + 7 sub-pages на ucode (uloop/ubus/uci).

- [x] Dashboard: LTE/VPN/WiFi/system stats
- [x] Touch menu: 6 кнопок, 2 страницы
- [x] Sub-pages: VPN, LTE, WiFi, Info, IP
- [x] Screensaver: bouncing clock + backlight off
- [x] Anti-burn-in: pixel shift +-2px
- [x] Auto-restart on crash
- [ ] data_collector: AT-команды возвращают CSQ=0 (проблема с таймингом ttyACM0)
- [ ] Автозапуск при загрузке (init.d скрипт)
- [ ] Графики сигнала (история RSRP/SINR)
- [ ] SMS чтение (AT+CMGL через Fibocom)
- [ ] Баланс SIM (USSD)

## PIC16LF1509 Battery Monitoring

### Статус: WIP — ADC читается, нужна стабилизация

- [x] NEW SM0 manual mode (CTL0=0x01F3800F) — стабильный I2C read
- [x] GPIO bit-bang I2C write — ACK на все команды PIC
- [x] Бипер играет мелодию ({0x2D}+{0x2E}+{0x2F} через bit-bang)
- [x] ADC значения: 423=LOW (батарея), 591=NORMAL (зарядка)
- [ ] Стабильное live ADC обновление (bat_read через NEW mode write)
- [ ] Интеграция ADC в lcd_ui.uc (показывать уровень заряда)

Подробности: [ideas/README.md](ideas/README.md)

## LTE модем (Fibocom L860-GL)

### Статус: РАБОТАЕТ

- [x] MBIM подключение через umbim
- [x] Beeline SIM, 16-20 Mbit/s
- [x] LTE Watchdog (cron, auto-restart)
- [x] WireGuard VPN tunnel
- [ ] AT-порт ttyACM0 — нестабильный доступ из data_collector

## Buzzer

### Статус: НАЙДЕН, не интегрирован

Команда buzzer: `{0x34, state, 0x00}` через I2C на PIC (0x2A).
PIC умеет играть мелодии (не просто ON/OFF).

- [ ] Добавить buzzer ioctl в lcd_drv.ko
- [ ] Звук при нажатии кнопок

## U-Boot

### Статус: ИССЛЕДОВАНИЕ (ВЫСОКИЙ РИСК)

- [ ] USB Recovery (проверка файла на USB при загрузке)
- [ ] LCD в U-Boot (показывать "Loading..." при старте)
- Текущий U-Boot работает (от a43/fildunsky)
- Нужен CH341A + SOIC8 для восстановления при неудаче
