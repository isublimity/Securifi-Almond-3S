# Battery Remaining Time Estimation — Алгоритм

**Цель**: оценка оставшегося времени работы роутера Almond 3S от батареи.
**Реализация**: C в `data_collector.c`, результат в JSON → `lcd_ui.uc` отображает.
**Платформа**: MIPS 1004Kc (soft-float), static binary via zig cc.

---

## 1. Исходные данные

### 1.0 Батарея

**Модель**: 18650-251P, 2S (2 последовательно)
- Ёмкость: 3200 mAh
- Энергия: 23.68 Wh

### 1.1 Источник ADC

PIC16LF1509 через I2C (cmd 0x36) возвращает 10-bit ADC (0–1023).
`data_collector` уже читает батарею каждые 2 секунды через `get_battery()` → `ioctl(fd, 2, raw)`.

Текущий JSON output:
```json
"battery": {"adc": 628, "percent": 50, "charging": false, "valid": true}
```

**Нужно добавить поля:**
```json
"battery": {
    "adc": 628, "percent": 50, "charging": false, "valid": true,
    "remain_min": 87, "drain_rate": 2.15
}
```
- `remain_min` — оставшееся время в минутах (-1 = неизвестно)
- `drain_rate` — текущая скорость разряда в ADC/мин (0 = не разряжается)

### 1.2 Характеристики батареи (из тестового разряда)

Полный разряд записан в `Battery_Drain_logs1.txt`:
- **1213 строк** в логе, **1203 прошли валидацию** (buf[3]==0x02 && buf[4]==0x04)
- **11 corrupted reads** (~0.9%) — отбрасываются валидатором
- Длительность: **4.13 часа**

```
Start ADC: 745 (полная зарядка, без зарядного)
End ADC:   68  (последняя запись перед отключением)
```

**ВАЖНО**: конец кривой (ADC < 200) — НЕ монотонный, скачет. В зоне ADC < 100
батарея нестабильна, оценка бессмысленна.

Интервал семплирования ядром: **~12 секунд** (mean=12.4s, median=12.2s, max=36.5s).
data_collector опрашивает каждые 2 секунды, но ядро обновляет ADC реже → одинаковые значения подряд.

Шум ADC: **±2–3 единицы** между соседними чтениями при стабильном разряде.
При ~12с интервале и скорости ~2 ADC/мин, реальное изменение за 1 сэмпл — ~0.4 ADC.
**Signal-to-noise при коротком окне** — главная проблема оценки.

### 1.3 Формат buf[5] (charging + battery detect)

| Сценарий | buf[5] | bin | Описание |
|----------|--------|-----|----------|
| Разряд (батарея есть) | 0x00 | 00000000 | Нет зарядки |
| Зарядка (батарея, начало) | 0x01 | 00000001 | bit0 = charging |
| Зарядка (батарея, стабильно) | 0x09 | 00001001 | bit0=1, bit3=1 (норма!) |
| Нет батареи + зарядник | 0x69 | 01101001 | bit5+bit6 = no battery |
| Нет батареи (переходное) | 0x49 | 01001001 | bit6=1 |
| Вставка батареи (переход) | 0x41 | 01000001 | bit6=1, быстро → 0x01 |

**Биты buf[5]:**
- bit0 = зарядка (0=нет, 1=да)
- bit3 = неизвестно (появляется и с батареей и без)
- bit5+bit6 = **нет батареи** (только когда батарея физически отключена)

**Детекция:**
```c
bi->charging = (raw[5] & 0x01) ? 1 : 0;      // bit0 = зарядка
bi->no_battery = (raw[5] & 0x60) ? 1 : 0;     // bit5+bit6 = нет батареи
```

При вставке батареи: buf[5] переходит 0x69 → 0x41 → 0x01 за ~1-2 цикла (~12-24 сек).
При отключении батареи: buf[5] переходит 0x01 → 0x69 мгновенно, ADC растёт (зарядник напрямую).

### 1.4 Форма кривой разряда

Кривая разряда **НЕ линейная** — типичная Li-ion:

![Discharge Curve](discharge_curve.png)

```
ADC 745→700:  22 мин  (быстрый начальный спад)
ADC 700→600:  43 мин  (плавный средний участок, ~2 ADC/мин)
ADC 600→500:  51 мин  (плавный, ~2 ADC/мин)
ADC 500→400:  38 мин  (ускоряется, ~3 ADC/мин)
ADC 400→300:  48 мин  (замедляется, ~2 ADC/мин)  ← ниже порога CRITICAL
ADC 300→200:  37 мин  (ускоряется, ~3 ADC/мин)
ADC 200→100:   9 мин  (обвал, ~11 ADC/мин) ← батарея мёртвая
```

**Ключевые пороги (из stock firmware):**
| ADC | Значение |
|-----|----------|
| ≥542 | NORMAL (20–100%) |
| 401–541 | LOW (1–20%) |
| <401 | CRITICAL (0%) |

### 1.5 Таблица ADC → оставшееся время (эмпирическая, сглаженная)

Из тестового разряда, smoothing window=11.
Таблица до ADC=400 (CRITICAL), ниже — доп. столбец до ADC=100 (реальный конец жизни).

**ОГРАНИЧЕНИЕ**: таблица основана на ОДНОМ тесте разряда при нагрузке WiFi+LTE+LCD+touch.
При другой нагрузке время может отличаться. Linreg-коррекция компенсирует это.

| ADC | До ADC=400 (мин) | До ADC=100 (мин) | Локальная скорость (ADC/мин) |
|-----|-------------------|-------------------|------------------------------|
| 750 | 152 | 245 | 1.8 |
| 725 | 145 | 238 | 2.1 |
| 700 | 130 | 224 | 1.5 |
| 675 | 116 | 209 | 2.7 |
| 650 | 109 | 202 | 3.1 |
| 625 | 100 | 193 | 2.9 |
| 600 | 88 | 182 | 1.5 |
| 575 | 68 | 162 | 1.7 |
| 550 | 56 | 149 | 2.2 |
| 525 | 43 | 137 | 3.0 |
| 500 | 37 | 130 | 3.5 |
| 475 | 29 | 122 | 3.5 |
| 450 | 22 | 115 | 3.2 |
| 425 | 12 | 106 | 2.2 |
| 400 | 0 | 93 | 2.7 |

---

## 2. Алгоритм

### 2.1 Обзор

Два механизма, комбинированные:

1. **Lookup table** — эмпирическая таблица ADC → minutes_remaining (работает сразу, без истории)
2. **Linear regression** — по последним N ADC точкам, корректирует таблицу на основе реальной скорости

Формула:
```
remain = table_remain(current_adc) * correction_factor * user_cal_factor
```

Где:
- `table_remain(adc)` — интерполяция по таблице
- `correction_factor = table_rate(adc) / measured_rate` — если батарея разряжается быстрее/медленнее чем в тесте
- `user_cal_factor` — ручной множитель из конфига

### 2.2 Кольцевой буфер (ring buffer)

```c
#define BAT_HIST_MAX 30  // максимальный размер (аллоцируется статически)

struct bat_sample {
    time_t t;   // epoch seconds
    int adc;    // 10-bit ADC
};

struct bat_estimator {
    struct bat_sample hist[BAT_HIST_MAX];
    int count;       // сколько заполнено (0..hist_size)
    int head;        // следующая позиция для записи
    int remain_min;  // результат: оставшееся время (-1 = неизвестно)
    int drain_rate;  // ADC/мин * 100 (fixed-point, положительное = разряд)
    int was_charging;  // для детекции перехода charge→discharge
};
```

**Рекомендуемые defaults:**
- `hist_size = 20` — 20 точек
- `min_interval = 30` секунд между точками
- Итого окно: 20 * 30 = 600 секунд = **10 минут**
- За 10 мин ADC падает на ~20 единиц → SNR ~7:1 (vs шум ±3)

**Правила заполнения:**
1. Добавлять точку ТОЛЬКО если `battery.valid == true`
2. Добавлять ТОЛЬКО если `battery.charging == false` (bit0 buf[5] = 0)
3. Добавлять ТОЛЬКО если прошло `>= min_interval` секунд от последней точки
4. При переходе charging→discharging — очистить буфер (начинать заново)

### 2.3 Linear Regression (целочисленная)

Вся арифметика на **int64_t** — soft-float double на MIPS ~50x медленнее.
drain_rate хранится как fixed-point: `rate_x100 = ADC/мин * 100`.

Когда в буфере >= 3 точки:

```c
void bat_calc_slope(struct bat_estimator *est, int *slope_x1000) {
    // slope_x1000 = ADC/сек * 1000 (отрицательное при разряде)
    int n = est->count;
    int oldest = (est->head - n + BAT_HIST_MAX) % BAT_HIST_MAX;
    int64_t t0 = (int64_t)est->hist[oldest].t;

    int64_t sum_t = 0, sum_a = 0, sum_tt = 0, sum_ta = 0;
    for (int i = 0; i < n; i++) {
        int idx = (oldest + i) % BAT_HIST_MAX;
        int64_t t = (int64_t)(est->hist[idx].t) - t0;
        int64_t a = (int64_t)est->hist[idx].adc;
        sum_t += t;
        sum_a += a;
        sum_tt += t * t;
        sum_ta += t * a;
    }

    int64_t denom = (int64_t)n * sum_tt - sum_t * sum_t;
    if (denom == 0) { *slope_x1000 = 0; return; }

    // slope * 1000 = (n * sum_ta - sum_t * sum_a) * 1000 / denom
    int64_t numer = ((int64_t)n * sum_ta - sum_t * sum_a);
    *slope_x1000 = (int)(numer * 1000 / denom);
}
```

Скорость разряда (ADC/мин * 100):
```c
// slope_x1000 отрицательный → drain_rate_x100 положительный
est->drain_rate = (int)(-(int64_t)slope_x1000 * 60 / 10);  // *60/1000*100 = *60/10
```

### 2.4 Lookup table (встроенная)

Таблица с шагом 25 ADC, сглаженная (window=11). Интерполяция линейная, целочисленная.

```c
struct bat_table_entry { int adc; int min_to_400; };

static const struct bat_table_entry bat_table[] = {
    {750, 152}, {725, 145}, {700, 130}, {675, 116}, {650, 109},
    {625, 100}, {600,  88}, {575,  68}, {550,  56}, {525,  43},
    {500,  37}, {475,  29}, {450,  22}, {425,  12}, {400,   0},
};
#define BAT_TABLE_SIZE 15

int bat_table_lookup(int adc) {
    if (adc >= 750) return 152;
    if (adc <= 400) return 0;
    for (int i = 0; i < BAT_TABLE_SIZE - 1; i++) {
        if (adc >= bat_table[i + 1].adc) {
            int da = bat_table[i].adc - bat_table[i + 1].adc;  // всегда 25
            int dm = bat_table[i].min_to_400 - bat_table[i + 1].min_to_400;
            return bat_table[i + 1].min_to_400 + (adc - bat_table[i + 1].adc) * dm / da;
        }
    }
    return 0;
}
```

**Табличная скорость** (ADC/мин * 100, целочисленная):
```c
int bat_table_rate_x100(int adc) {
    // Берём таблицу в окне ±15 ADC
    int t1 = bat_table_lookup(adc + 15 > 750 ? 750 : adc + 15);
    int t2 = bat_table_lookup(adc - 15 < 400 ? 400 : adc - 15);
    int dt = t1 - t2;  // минут на 30 ADC
    if (dt <= 0) return 250;  // fallback 2.5 ADC/мин
    return 3000 / dt;  // 30 * 100 / dt
}
```

### 2.5 Комбинированная оценка

```c
void bat_estimate(struct bat_estimator *est, int cur_adc) {
    int tab_min = bat_table_lookup(cur_adc);

    if (est->count < 3) {
        // Мало данных — только таблица
        est->remain_min = tab_min;
        est->drain_rate = 0;
        return;
    }

    int slope_x1000;
    bat_calc_slope(est, &slope_x1000);

    if (slope_x1000 >= 0) {
        // Не разряжается (шум или рост)
        est->remain_min = -1;
        est->drain_rate = 0;
        return;
    }

    // drain_rate_x100 = ADC/мин * 100
    int drain_rate_x100 = (int)(-(int64_t)slope_x1000 * 60 / 10);
    est->drain_rate = drain_rate_x100;

    int tab_rate_x100 = bat_table_rate_x100(cur_adc);

    // correction_x100 = tab_rate / measured_rate * 100
    int correction_x100 = (int)((int64_t)tab_rate_x100 * 100 / drain_rate_x100);

    // Clamp: 30..300 (0.3x .. 3.0x)
    if (correction_x100 < 30) correction_x100 = 30;
    if (correction_x100 > 300) correction_x100 = 300;

    // remain = tab_min * correction * user_factor
    // user_factor_x100 тоже fixed-point (100 = 1.0)
    est->remain_min = (int)((int64_t)tab_min * correction_x100 * user_factor_x100 / 10000);
    if (est->remain_min < 0) est->remain_min = 0;
}
```

### 2.6 Главная функция (bat_update)

Вызывается каждые 2 секунды из main loop, после `get_battery()`:

```c
void bat_update(struct bat_estimator *est, struct battery_info *bi) {
    if (!bi->valid) {
        est->remain_min = -1;
        return;
    }

    // Детекция перехода charging → discharging
    if (bi->charging) {
        est->was_charging = 1;
        bat_hist_clear(est);
        est->remain_min = -1;
        est->drain_rate = 0;
        return;
    }
    if (est->was_charging) {
        bat_hist_clear(est);
        est->was_charging = 0;
    }

    // ADC < 100 — батарея мёртвая
    if (bi->adc < 100) {
        est->remain_min = 0;
        return;
    }

    // Throttle: добавлять точку не чаще min_interval
    time_t now = time(NULL);
    if (est->count > 0) {
        int last = (est->head - 1 + BAT_HIST_MAX) % BAT_HIST_MAX;
        if ((now - est->hist[last].t) < bat_cal_interval)
            goto calc;  // Не добавляем точку, но пересчитываем
    }

    // Push точку
    bat_hist_push(est, now, bi->adc);

calc:
    bat_estimate(est, bi->adc);
}
```

### 2.7 Когда НЕ показывать время

- `charging == true` → показывать "зарядка", remain_min = -1
- `valid == false` → показывать "?", remain_min = -1
- `adc < 100` → батарея мёртвая, remain_min = 0
- `count < 3` — показывать оценку по таблице (грубую, но лучше чем ничего)

---

## 3. Калибровка

### 3.1 Файл конфигурации

`/etc/lcd/bat_cal` — простой `key=value` формат, одна пара на строку.
Комментарии `#`, пустые строки игнорируются.

**Файл НЕ обязателен.** Если `/etc/lcd/bat_cal` отсутствует — работаем на встроенных
defaults (см. таблицу ниже). Файл нужен ТОЛЬКО если базовые параметры не подходят
(другая батарея, хочется подкрутить точность после калибровки). Можно указать только
те ключи, которые нужно изменить — остальные останутся default.

```
# Battery calibration
cutoff_adc=400
time_factor=100
hist_size=20
min_interval=30
```

| Ключ | Тип | Default | Описание |
|------|-----|---------|----------|
| `cutoff_adc` | int | 400 | ADC значение "батарея мёртвая". Если хочется запас — поставить 420 |
| `time_factor` | int | 100 | Множитель * 100 (fixed-point). 120 = "+20%", 80 = "-20%" |
| `hist_size` | int | 20 | Размер кольцевого буфера. 10–30. Больше = стабильнее |
| `min_interval` | int | 30 | Секунд между точками. 10–60. С hist_size определяет окно |

### 3.2 Как калибровать

1. Зарядить батарею полностью (ADC ~740–750)
2. Отключить зарядку, засечь время
3. Дождаться ADC ~400 (CRITICAL)
4. Сравнить реальное время с оценкой
5. Скорректировать `time_factor`:
   ```
   time_factor = реальное_время / оценённое_время * старый_factor
   ```
   Пример: реально 180 мин, оценка 150 мин → `time_factor = 180/150 * 100 = 120`

### 3.3 Замена таблицы

Таблица `bat_table[]` вшита в C-код. При смене батареи/условий:
1. Записать новый тест разряда (logread или dmesg > drain_log.txt)
2. Пересчитать: `python3 build_table.py` (в директории `Battery_Drain/`)
3. Обновить массив в data_collector.c
4. Пересобрать: `./build.sh userspace && ./build.sh deploy`

---

## 4. Интеграция в data_collector.c

### 4.1 Что добавить

**Структуры** (после `struct battery_info`, строка ~44):
```c
/* ======== Battery Time Estimation ======== */
#define BAT_HIST_MAX 30
#define BAT_CAL_PATH "/etc/lcd/bat_cal"

struct bat_sample { time_t t; int adc; };

struct bat_estimator {
    struct bat_sample hist[BAT_HIST_MAX];
    int count, head;
    int remain_min;     // -1 = unknown, 0 = dead, >0 = minutes
    int drain_rate;     // ADC/min * 100 (fixed-point)
    int was_charging;
};

static struct bat_estimator bat_est = {0};

/* Calibration (read from /etc/lcd/bat_cal at startup) */
static int bat_cal_cutoff = 400;
static int bat_cal_factor = 100;   // *100 fixed-point (100 = 1.0x)
static int bat_cal_hist_size = 20;
static int bat_cal_interval = 30;  // seconds
```

**Функции** (полный список):
1. `bat_cal_load()` — чтение `/etc/lcd/bat_cal` (key=value формат, см. секцию 3.1 и 6.2)
2. `bat_hist_clear(struct bat_estimator *e)` — `e->count = 0; e->head = 0;`
3. `bat_hist_push(struct bat_estimator *e, time_t t, int adc)` — push в ring buffer, `head = (head+1) % hist_size`
4. `bat_table_lookup(int adc)` — таблица → минуты (см. 2.4)
5. `bat_table_rate_x100(int adc)` — табличная скорость * 100 (см. 2.4)
6. `bat_calc_slope(struct bat_estimator *e, int *slope_x1000)` — linreg на int64_t (см. 2.3)
7. `bat_estimate(struct bat_estimator *e, int cur_adc)` — комбинированная оценка (см. 2.5)
8. `bat_update(struct bat_estimator *e, struct battery_info *bi)` — главный entry point (см. 2.6)

### 4.2 Вызов в main loop

Строка ~303 data_collector.c:
```c
get_battery(&bat);
bat_update(&bat_est, &bat);   // ← ДОБАВИТЬ
```

### 4.3 JSON output

Строка ~316, расширить формат battery:
```c
"\"battery\":{\"adc\":%d,\"percent\":%d,\"charging\":%s,\"valid\":%s,"
"\"remain_min\":%d,\"drain_rate\":%d.%d},"
```

Аргументы:
```c
bat.adc, bat.percent,
bat.charging ? "true" : "false",
bat.valid ? "true" : "false",
bat_est.remain_min,
bat_est.drain_rate / 100, abs(bat_est.drain_rate) % 100
```

(`drain_rate` в JSON — ADC/мин с одним десятичным: `"drain_rate":2.1`)

### 4.4 Вызов bat_cal_load() при старте

В main(), перед while(running) loop:
```c
bat_cal_load();
```

### 4.5 Отображение в lcd_ui.uc

В блоке отрисовки батареи (строка ~460 lcd_ui.uc), заменить строку с `bat_txt`:
```javascript
let bat_txt = bvalid ? bpct + "%" : "?";
let remain = bat?.remain_min;
if (remain != null && remain >= 0 && !bchg) {
    if (remain >= 60)
        bat_txt += " ~" + int(remain / 60) + "h" + sprintf("%02d", remain % 60);
    else
        bat_txt += " ~" + remain + "m";
}
lcd_text(bat_x + 18, 2, bat_txt, bat_color, bat_bg, 2);
```

---

## 5. Точность оценки

### 5.1 Чистый linreg (N=20, interval=30s, cutoff=400)

| Диапазон ADC | Медианная ошибка | P25–P75 |
|-------------|-----------------|---------|
| 600–750 | +6% | -11% .. +42% |
| 450–600 | +55% | +10% .. +87% |
| 400–450 | +2% | -15% .. +21% |
| **Общая** | **+34%** | **-10% .. +73%** |

Большая ошибка в 450–600: кривая разряда нелинейна, линейная экстраполяция переоценивает.

### 5.2 Таблица + коррекция (комбинированный метод)

| Диапазон ADC | Медианная ошибка | P25–P75 |
|-------------|-----------------|---------|
| 600–750 | +0% | -21% .. +30% |
| 450–600 | +18% | +6% .. +34% |
| 400–450 | +9% | -7% .. +29% |
| **Общая** | **+11%** | **-2% .. +34%** |

Комбинированный метод значительно стабильнее. P25–P75 сужается с [-10,+73] до [-2,+34].

**ВАЖНО**: это оценка на СВОИХ данных (один и тот же тест). На другом цикле разряда
(другая нагрузка, температура, износ батареи) ошибка будет больше. Linreg-коррекция
именно для этого и нужна — подстраивает таблицу под реальные условия.

### 5.3 Источники ошибки

1. **Шум ADC ±2–3** — при коротком окне SNR плохой, увеличиваем окно до 10 мин
2. **Нелинейность кривой** — таблица компенсирует, linreg только корректирует масштаб
3. **Нагрузка** — WiFi/LTE/LCD активность влияет на скорость разряда
4. **Температура** — Li-ion имеет другую ёмкость при холоде
5. **Износ** — со временем ёмкость уменьшается, нужна перекалибровка

---

## 6. Заметки для реализации

### 6.1 Арифметика

- **НЕ использовать double/float** — MIPS soft-float, ~50x penalty
- Все вычисления на **int / int64_t**
- Fixed-point: `*100` для drain_rate и factors, `*1000` для slope
- `int64_t` обязателен для sum_tt, sum_ta и промежуточных произведений в linreg

### 6.2 Парсинг конфига /etc/lcd/bat_cal

Формат `key=value`, одна пара на строку. `#` — комментарий. Файл опционален.

```c
static void bat_cal_load(void) {
    FILE *fp = fopen(BAT_CAL_PATH, "r");
    if (!fp) return;  // defaults остаются
    char line[64];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        int val = atoi(eq + 1);
        if (strcmp(line, "cutoff_adc") == 0)   bat_cal_cutoff = val;
        else if (strcmp(line, "time_factor") == 0)  bat_cal_factor = val;
        else if (strcmp(line, "hist_size") == 0)    bat_cal_hist_size = val > BAT_HIST_MAX ? BAT_HIST_MAX : val;
        else if (strcmp(line, "min_interval") == 0) bat_cal_interval = val;
    }
    fclose(fp);
}
```

### 6.3 Размер кода

Весь estimation блок — ~150-200 строк C. Никаких аллокаций (static struct).
BAT_HIST_MAX=30 * 8 bytes = 240 bytes RAM — пренебрежимо.

---

## 7. Справка: файлы

| Файл | Описание |
|------|----------|
| `modules/data_collector.c` | **СЮДА ВНОСИТЬ ИЗМЕНЕНИЯ** — C daemon, строка ~44 (структуры), ~303 (вызов) |
| `modules/lcd_ui.uc` | UI — строка ~460, добавить отображение remain_min |
| `Battery_Drain/Battery_Drain_logs1.txt` | Сырой лог разряда (1213 строк) |
| `Battery_Drain/Battery_Charge_logs1.txt` | Лог зарядки (34 строки) |
| `Battery_Drain/discharge_curve.png` | График кривой разряда |
| `Battery_Drain/Battery_Drain_ALGO.md` | Этот документ |
| `/etc/lcd/bat_cal` | Конфиг калибровки, key=value (на роутере) |

---

## 8. Сводка: что делать

1. В `data_collector.c` добавить `struct bat_estimator` + ring buffer + linreg (int64_t) + lookup table
2. Добавить `bat_cal_load()` при старте, `bat_update()` после `get_battery()` в main loop
3. Расширить JSON battery: поля `remain_min`, `drain_rate`
4. В `lcd_ui.uc` показать оставшееся время рядом с процентом батареи
5. Создать `/etc/lcd/bat_cal` с defaults на роутере
6. Собрать: `./build.sh userspace && ./build.sh deploy`
