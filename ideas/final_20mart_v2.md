# PIC Battery — Итоги 20 марта 2026 (финал)

## ПРОРЫВ: Стоковое ядро 3.10 РАБОТАЕТ на eco:3!
- ADC=0x2CF (719) = 100% батарея, LIVE данные
- SM0_CFG=0x00 и на стоке тоже (READ-ONLY не причина)
- Собрали ВСЕ SM0 регистры через restdebug (Go REST agent)

## Пойманная PIC транзакция на стоке
```
WRITE: DATA=0x2A STRT=1 STAT=0 CTL0=0x80648006
READ:  DATA=0x2A STRT=5 STAT=1 CTL0=0x8064800E DIN=0x09 ← РЕАЛЬНЫЕ ДАННЫЕ!
```

## Что проверено на OpenWrt 6.6.127

### i2c_transfer через kernel i2c-mt7621:
| Метод | ret | Данные |
|-------|-----|--------|
| write {2F,00,01} + delay + read 6 | **0 (OK!)** | aa 54 a8 (bus noise) |
| combined write+read (2 msgs) | -6 NACK | нули |
| read-only 6 bytes | **0 (OK!)** | aa 54 a8 (bus noise) |
| write 1 byte + read 6 (2 msgs) | -6 NACK | нули |

### i2c_transfer РАБОТАЕТ (ret=0, PIC ACKает), но данные = bus noise!
PIC не загружает SSPBUF несмотря на ACK.

### SM0 auto mode: ff ff ff (конфликт с kernel driver)
### Bit-bang: aa 54 a8 (bus noise)
### NEW manual mode: aa 54 a8 (bus noise)

## Ключевое отличие: i2c-mt7621 driver
- **Ядро 3.10**: Стоковый AlmondPic2 driver владеет SM0 монопольно, SM0 AUTO mode
- **Ядро 6.6**: i2c-mt7621 kernel driver владеет SM0, NEW MANUAL mode
- i2c-mt7621 использует ДРУГОЙ режим SM0 (manual vs auto)
- PIC firmware может требовать AUTO mode timing

## Калибровка на стоке: count=2 (не пустая!)
На стоке: 0x81391FAC = 0xB8220002 (count=2)
Мы отправляем пустые таблицы {0x2D}/{0x2E}

## СЛЕДУЮЩИЙ ШАГ: Пропатчить i2c-mt7621

Вместо борьбы с SM0 из userspace/lcd_drv — пропатчить сам i2c-mt7621 driver:
1. Добавить SM0 AUTO mode support (как в ядре 3.10)
2. Или: увеличить clock stretching timeout
3. Или: добавить PIC-специфичный slow mode (5ms между байтами как в стоке)
4. Пересобрать ядро на build server

Файл: `drivers/i2c/busses/i2c-mt7621.c` в OpenWrt kernel tree.

## SM0 AUTO mode patch — НЕ ПОМОГЛО
Пропатчили i2c-mt7621: SM0 permanent AUTO mode (как стоковое ядро).
Результат: **тот же bus noise** (ff aa 15 7f). LAN при этом работает!

AUTO mode на kernel 6.6 ≠ AUTO mode на kernel 3.10.
Разница в init последовательности SM0 контроллера:
- Kernel 6.6: `device_reset()` → CTL0 = clk_div | EN | SCL_STRETCH
- Kernel 3.10: RSTCTRL manual → CTL0 = 0x90640042

## Нужен осциллограф
Единственный способ увидеть разницу — сигналы на SDA/SCL.

## Инструменты созданные сегодня
- **restdebug** (Go, 6.3MB) — REST agent, порт 7777, exec/mem/upload/download/gpio/sm0/poll
- **memdebug** (C, 92KB) — TCP agent, порт 5555
- **sm0_dump** (C, 48KB) — SM0 register dumper
- **at_cmd** (C) — AT modem commands
- **mdcli.sh** — Mac client для upload/build/exec

## Файлы
- `stock_dumps/` — все дампы стоковой прошивки (kallsyms, SM0, poll, calib, dmesg)
- `STOCK_DUMP_20mart.md` — анализ стоковых SM0 регистров
- `KERNEL_DIFF.md` — сравнение ядер 3.10 vs 6.6
- `modules/restdebug/main.go` — Go REST agent
- `modules/memdebug.c` — TCP debug agent
