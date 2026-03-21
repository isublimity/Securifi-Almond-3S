# Финальный анализ: бипер vs батарея (2026-03-20)

## Факты

### 1. Роутер был обесточен 1 ЧАС — PIC полностью сброшен!
- Но данные = `aa 54 a8 50 a0 40 80 00` — тот же "test pattern"
- Значит **это НЕ test mode!** Это **нормальный ответ PIC** через NEW manual mode
- PIC отвечает ОДИНАКОВО на NEW manual mode read независимо от состояния

### 2. `aa 54 a8 50 a0 40 80 00` = шумовой паттерн SM0 hardware
```
AA = 10101010
54 = 01010100  (сдвиг)
A8 = 10101000  (сдвиг)
50 = 01010000
A0 = 10100000
40 = 01000000
80 = 10000000
00 = 00000000
```
Каждый байт = предыдущий сдвинут. Это **SM0 bus noise** когда PIC
не загружает данные в SSPBUF. PIC ACKает адрес но не отправляет данные.

### 3. Реальные данные PIC мы получали ТОЛЬКО через SM0 auto mode:
- `55 00 00 00 39 3e 01 e6` — SM0 auto mode read (NEW regs, boot probe)
- `97 07 25 e5 39 3e 40 e6` — SM0 hybrid init (bit-bang write + SM0 read)
- ADC=591/423 — v0.28 (RSTCTRL + SM0 auto mode write + read)

### 4. NEW manual mode read = ВСЕГДА `aa 54 a8`
Не зависит от состояния PIC. Это SM0 hardware artifact.
PIC ACKает read address но НЕ ЗАГРУЖАЕТ данные в SSPBUF для manual mode.

### 5. Бипер ≠ батарея
- Команды {0x2D}, {0x2E}, {0x2F} ДОШЛИ до PIC (ACK + мелодия)
- Но мелодия = побочный эффект инициализации, не батарея
- {0x34,0x00,0x00} ACKed но мелодия НЕ остановилась
- Мелодия прекращается только при обесточивании

## Корневая причина

**PIC I2C slave firmware работает в ДВА РЕЖИМА:**

1. **Auto mode (старые SM0 регистры)**: PIC firmware автоматически загружает SSPBUF
   когда SM0 hardware тактирует шину в auto mode. PIC firmware различает auto mode
   по специфичному паттерну SM0_START/SM0_STATUS/SM0_DATAOUT.

2. **Manual mode (новые SM0 регистры)**: SM0 hardware тактирует побитово.
   PIC firmware НЕ РАСПОЗНАЁТ manual mode как валидный I2C — ACKает адрес
   (hardware MSSP), но firmware не загружает SSPBUF (нет SSPxIF?).

**SM0 auto mode — единственный метод который PIC понимает для READ.**
Но auto mode на 6.6/6.12 = SM0_CFG read-only, CTL0 модифицируется → данные
нестабильны (ff через N итераций).

## Что РЕАЛЬНО работало

| Версия | Метод | Данные | Длительность |
|--------|-------|--------|-------------|
| Boot probe | kernel i2c-mt7621 (manual mode) | D0/D1 кэш = real data | Один раз |
| v0.28 | RSTCTRL + SM0 auto write + auto read | ADC=591/423 LIVE! | 20 мин |
| v0.30 | RSTCTRL + auto read only | ADC=1439 static | Бесконечно |

**v0.28 давал LIVE данные 20 мин!** Потом SM0 auto write ломал PIC.

## План: LIVE батарея

### Вариант A: v0.28 с auto-recovery
v0.28 работал 20 мин. Потом PIC шёл в ff. Но что если:
1. Detect ff → автоматически rmmod + insmod lcd_drv (RSTCTRL при init)
2. PIC сбрасывается при RSTCTRL
3. Снова 20 мин live данных
4. Цикл: 20 мин live → detect ff → reload → 20 мин live

### Вариант B: SM0 auto read ТОЛЬКО, write через bit-bang
- bit-bang write bat_read (не ломает PIC)
- SM0 auto mode read с RSTCTRL (v0.28 read path)
- Без SM0 auto write → PIC не должен ломаться
- Но v0.30 давал static данные (1439) — нужен write для refresh

### Вариант C: SM0 auto write + read, раз в 5 мин
- SM0 auto mode write + read = LIVE данные
- Но ломает PIC через 20 мин при каждые 5 сек
- При каждые 5 мин: 20 мин / (5мин/5сек) = ~60 итераций
- Может хватить на ЧАСЫ

### Вариант D: Boot snapshot + periodic reboot
- D0/D1 при boot содержат real data
- cron: reboot каждый час → свежий snapshot
- Грубо но работает (reboot не работает на Almond — PIC контролирует питание)

### РЕКОМЕНДАЦИЯ: Вариант C
SM0 auto write + read раз в 5 МИНУТ (не 5 секунд).
При 5 мин интервале: ~240 read/write за 20 часов.
v0.28 сломался после ~240 операций (20мин × 12/мин).
При 5 мин: 240 операций = 20 ЧАСОВ стабильной работы!
Detect ff → RSTCTRL reset → продолжить.

## Почему мелодия не останавливается

Из IDA: PIC_BUZZER отправляет {0x34, 0x00, mode}. Но:
1. Мелодия запускается НЕ через {0x34} а через внутреннюю логику PIC
2. {0x34,0x00,0x00} = команда buzzer OFF, но мелодия ≠ buzzer
3. Мелодия = PIC firmware alarm sequence (PWM паттерн, встроенный в firmware)
4. Остановка мелодии: отправить {0x34,0x00,0x03} потом {0x34,0x00,0x00}?
   Или мелодия останавливается сама через время?
   Или нужна другая команда (не 0x34)?
