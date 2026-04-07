# Fibocom L860-GL — Настройка на Almond 3S

## Модем
- **Модель**: Fibocom L860-GL (Cat16, LTE-A до 1 Гбит/с DL)
- **USB VID:PID**: 2cb7:0007
- **Интерфейс**: MBIM (НЕ QMI!)
- **USB дескриптор**: "MBIM + 3 CDC-ACM"
- **Драйвер ядра**: cdc_mbim (встроен в прошивку)
- **Управление**: umbim (установлен в прошивке)
- **Сетевой интерфейс**: wwan0

## Бенды LTE
B1/B2/B3/B4/B5/B7/B8/B12/B13/B14/B17/B18/B19/B20/B25/B26/B28/B29/B30/B32/B66/B71

Для России (Билайн/МТС/Мегафон): B3, B7, B20 — совместим.

## Текущий статус
- USB определяется: ✅
- cdc_mbim загружен: ✅
- wwan0 создан: ✅ (state DOWN)
- umbim установлен: ✅
- **SIM-карта**: нужно вставить!
- **Serial порты**: нужен `option` driver bind

## Настройка

### 1. Вставить SIM-карту
Слот miniPCIe с SIM-холдером на плате Almond 3S.

### 2. Активировать serial порты (для AT команд)
```bash
echo "2cb7 0007" > /sys/bus/usb-serial/drivers/option1/new_id
# Появятся /dev/ttyUSB1, /dev/ttyUSB3, /dev/ttyUSB5
```

Для автозагрузки добавить в `/etc/rc.local`:
```bash
echo "2cb7 0007" > /sys/bus/usb-serial/drivers/option1/new_id
```

### 3. Настроить сетевой интерфейс
```bash
uci set network.lte=interface
uci set network.lte.proto='mbim'
uci set network.lte.device='/dev/cdc-wdm0'
uci set network.lte.apn='internet'       # или ваш APN
uci set network.lte.pincode=''           # PIN если есть
uci set network.lte.defaultroute='0'     # НЕ default route (VPN приоритет!)
uci commit network
```

### 4. Добавить в firewall зону wan
```bash
uci add_list firewall.@zone[1].network='lte'
uci commit firewall
```

### 5. Поднять интерфейс
```bash
ifup lte
```

### 6. Проверка
```bash
# Статус MBIM
umbim -d /dev/cdc-wdm0 caps
umbim -d /dev/cdc-wdm0 subscriber
umbim -d /dev/cdc-wdm0 registration

# Сетевой интерфейс
ip addr show wwan0
ping -I wwan0 8.8.8.8

# AT команды (если serial порты активированы)
echo -ne "AT\r" > /dev/ttyUSB1
cat /dev/ttyUSB1
```

## Важно
- **uqmi НЕ работает** с этим модемом (QMI ≠ MBIM)
- **defaultroute='0'** — VPN туннель должен быть default route
- **GPIO 33** — аппаратный reset модема (#PERST, active low)
- Если модем не отвечает: `echo 0 > /sys/class/gpio/gpio545/value; sleep 1; echo 1 > /sys/class/gpio/gpio545/value`

## Отличия от Quectel EC21-E (штатный)
| Параметр | EC21-E | L860-GL |
|----------|--------|---------|
| Категория | Cat1 (10 Мбит) | Cat16 (1 Гбит) |
| Протокол | QMI | MBIM |
| Управление | uqmi | umbim |
| AT порт | /dev/ttyUSB2 | /dev/ttyUSB1 (после option bind) |
| Драйвер | qmi_wwan | cdc_mbim |
