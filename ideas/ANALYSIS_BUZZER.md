# Анализ: почему заработал бипер и план получения батареи

## 1. Что именно мы отправили

```
pic_final.c выполнил через bit-bang (GPIO sysfs):
  {0x33, 0x00, 0x00}  → NACK  (WAKE — PIC отклонил, был в test mode)
  {0x2D}               → ACK  (пустая Calibration Table 1)
  {0x2E}               → ACK  (пустая Calibration Table 2)
  {0x2F, 0x00, 0x01}  → ACK  (bat_read polling mode)

Потом через NEW SM0 manual mode:
  bat_read {0x2F, 0x00, 0x01} → ACK (write result=0)

Итого PIC получил: {0x2D}, {0x2E}, {0x2F,0x00,0x01} × 2
```

## 2. Почему заиграла мелодия

### Мы НЕ отправляли {0x34} (buzzer command)!

Мелодия = **внутренняя логика PIC firmware**, а не прямая команда buzzer.

### Из IDA анализа (PIC_CALIBRATE, 0x413200):
```c
// Стоковый flow при count=0:
udelay(5000);
sub_413F78(0x2A, {0x2D}, 1);    // пустая Table1
buf[0] = 0x2E;
// → отправка Table2 и выход
```

Стоковая прошивка ТОЖЕ отправляет пустые {0x2D} + {0x2E} при count=0.
Это **нормальный startup path**.

### Вероятная причина мелодии:

**{0x2F, 0x00, 0x01}** (bat_read polling) запустил мониторинг батареи.
PIC firmware обнаружил одно из:
- LOW battery (ADC < порога) → alarm мелодия
- Startup завершён → notification мелодия
- Battery monitoring started → confirmation звук

### Из IDA PIC_BATTERY_MONITOR (0x413DCC):
```
if raw_adc < 0x191 (401):
    → CRITICAL LOW → printk warning
    → Возможно: запуск alarm мелодии через внутренний PWM
```

**PIC firmware имеет встроенный alarm для LOW battery.** Мы видели ADC=423 — это
чуть выше CRITICAL (401) но в зоне LOW. PIC мог решить что батарея в опасности
и запустить alarm.

### Почему WAKE NACKed но {0x2D}/{0x2E}/{0x2F} ACKed?

PIC в test mode (от предыдущих SM0 auto mode операций) отклоняет WAKE (0x33)
но ПРИНИМАЕТ калибровочные команды (0x2D, 0x2E) и bat_read (0x2F).

Это значит: **test mode НЕ блокирует все команды** — только WAKE.
PIC firmware в test mode продолжает обрабатывать калибровку и bat_read.

## 3. Что мы узнали о PIC firmware

1. **PIC умеет играть мелодии** — не просто ON/OFF buzzer через PWM
2. **bat_read {0x2F,0x00,0x01}** запускает мониторинг с alarm-ами
3. **Пустые таблицы {0x2D}+{0x2E}** — валидная инициализация (count=0)
4. **PIC firmware реагирует на LOW battery** автоматически (мелодия)
5. **Test mode не полностью блокирует PIC** — команды кроме WAKE проходят

## 4. План получения LIVE батареи

### Проблема:
Все предыдущие попытки READ давали:
- `aa 54 a8` — test mode (PIC испорчен SM0 auto mode)
- `55 00 00 00 39 3e` — static (не обновляется)
- `ff ff ff ff` — SM0 corrupted

### Что мы ещё НЕ пробовали:

**NEW manual mode read ПОСЛЕ полной инициализации на ЧИСТОМ PIC**

Все наши "чистые" тесты на свежем PIC использовали SM0 auto mode read,
который СРАЗУ портил PIC. Мы НИКОГДА не делали:
1. Power cycle (чистый PIC)
2. bit-bang init: {0x2D} + {0x2E} + {0x2F,0x00,0x01}
3. {0x34, 0x00, 0x00} (стоп мелодии)
4. NEW manual mode read (CTL0=0x01F3800F)
5. Подождать и прочитать снова — меняются ли данные?

### Конкретный план (pic_battery_final.c):

```c
// Phase 1: Init PIC через bit-bang
bb_write({0x2D});           // пустая Table1
bb_write({0x2E});           // пустая Table2
bb_write({0x2F,0x00,0x01}); // bat_read polling
sleep(1);
bb_write({0x34,0x00,0x00}); // стоп мелодии

// Phase 2: Первое чтение через NEW manual mode
new_read(buf, 8);           // CTL0=0x01F3800F, CFG2=0
// Ожидаем: НЕ test pattern, НЕ static — СВЕЖИЕ ADC данные!

// Phase 3: Loop — read каждые 5 сек, 5 минут
for(i=0; i<60; i++) {
    new_read(buf, 8);
    printf("ADC=%d\n", (buf[2]<<8)|buf[3]);  // или другой offset
    sleep(5);
}
// Ожидаем: ADC постепенно падает (на батарее) или растёт (на зарядке)
```

### Почему это должно сработать:

1. **Чистый PIC** — не в test mode, не испорчен SM0 auto mode
2. **Init через bit-bang** — PIC ACKает {0x2D},{0x2E},{0x2F} (доказано!)
3. **bat_read {0x2F,0x00,0x01}** запускает мониторинг (мелодия = proof!)
4. **NEW manual mode read** — не ломает PIC (доказано 20+ итераций стабильно)
5. **Нет SM0 auto mode** вообще — ни read ни write

### Ключевое отличие от предыдущих попыток:

| Попытка | Write | Read | Результат |
|---------|-------|------|-----------|
| v0.24-v0.27 | bit-bang | SM0 auto mode | ff или test pattern |
| v0.28 | SM0 auto mode | SM0 auto mode | ADC=591 (20 мин), потом ff |
| v0.30 | нет write | RSTCTRL + SM0 auto mode | ADC=1439 static |
| **НОВЫЙ** | **bit-bang** | **NEW manual mode** | **? (не тестировали!)** |

Мы НИКОГДА не комбинировали bit-bang write + NEW manual mode read на чистом PIC!
Это единственная нетестированная комбинация.

## 5. Формат ответа PIC — что ожидать

Из IDA DATA_TRACE:
- Byte 0: SM0 echo (0xFF для auto mode, может быть другим для manual mode)
- Bytes 1+: реальные данные PIC
- struct+8: RAW ADC value (сравнивается с 401/542)

Из D0/D1 при boot (kernel i2c probe через manual mode):
```
D0 = 0xED254797 → little-endian: 97 47 25 ED
D1 = 0xE6013E39 → little-endian: 39 3E 01 E6
```
Полный ответ: `97 47 25 ED 39 3E 01 E6` (8 bytes через manual mode)

Это ДРУГОЙ формат чем SM0 auto mode (`55 00 00 00 39 3e 01 e6`)!
Manual mode не добавляет echo byte — данные начинаются с byte 0.

### Интерпретация (гипотеза):
```
Byte 0: 0x97 = ???  (не 0x55 — разный формат!)
Byte 1: 0x47 = ???
Byte 2: 0x25 = ???
Byte 3: 0xED = ???
Byte 4: 0x39 = ADC high
Byte 5: 0x3E = ADC low
Byte 6: 0x01 = status (charger?)
Byte 7: 0xE6 = checksum?
```

Или bytes 0-1 = ADC: `0x9747` = ??? или `(0x97&0x03)<<8 | 0x47` = `0x0347` = 839 → NORMAL range!

## 6. Бипер — как управлять

```
{0x34, 0x00, 0x03} = buzzer ON (или запуск мелодии)
{0x34, 0x00, 0x00} = buzzer OFF (стоп мелодии)
```

PIC firmware имеет встроенные мелодии:
- Startup melody (при инициализации {0x2D}+{0x2E}+{0x2F})
- LOW battery alarm (при ADC < порога)
- Может быть: tamper alarm, power off notification

Для управления: отправить {0x34, 0x00, 0x00} через bit-bang после init.

## 7. Резюме: что делать когда роутер будет доступен

1. Вытащить батарею 10 сек → включить (чистый PIC)
2. НЕ загружать lcd_drv (он в /lib/modules, переименовать в .bak)
3. Unbind i2c-mt7621
4. Запустить pic_battery_final:
   - bit-bang: {0x2D} + {0x2E} + {0x2F,0x00,0x01}
   - bit-bang: {0x34,0x00,0x00} (стоп мелодии)
   - NEW manual mode read × 60 (каждые 5 сек = 5 мин)
5. Отключить зарядку через 2 мин → смотреть падает ли ADC
6. Если ADC live → ПОБЕДА → переносить в lcd_drv.ko
