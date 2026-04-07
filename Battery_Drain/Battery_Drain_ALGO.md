# Battery Remaining Time Estimation — Алгоритм

**Цель**: оценка оставшегося времени работы роутера Almond 3S от батареи.
**Реализация**: C в `data_collector.c`, результат в JSON → `lcd_ui.uc` отображает.

---

## 1. Исходные данные

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

Полный разряд записан в `Battery_Drain_logs1.txt` (1203 точки, 4.13 часа).

```
Start ADC: 745 (полная зарядка)
End ADC:   68  (полный разряд, роутер выключился вскоре)
```

Интервал семплирования ядром: **~12 секунд** (mean=12.4s, median=12.2s).
data_collector опрашивает каждые 2 секунды, но ядро обновляет реже → одинаковые значения подряд.

Шум ADC: **±2–3 единицы** между соседними чтениями при стабильном разряде.

### 1.3 Форма кривой разряда

Кривая разряда **НЕ линейная** — типичная Li-ion:

![Discharge Curve](discharge_curve.png)

```
ADC 745→700:  22 мин  (быстрый начальный спад)
ADC 700→600:  43 мин  (плавный средний участок, ~2 ADC/мин)
ADC 600→500:  51 мин  (плавный, ~2 ADC/мин)
ADC 500→400:  38 мин  (ускоряется, ~2.6 ADC/мин)
ADC 400→300:  48 мин  (замедляется, ~2 ADC/мин)  ← ниже порога CRITICAL
ADC 300→200:  37 мин  (ускоряется, ~2.7 ADC/мин)
ADC 200→100:   9 мин  (обвал, ~11 ADC/мин) ← батарея мёртвая
```

**Ключевые пороги (из stock firmware):**
| ADC | Значение |
|-----|----------|
| ≥542 | NORMAL (20–100%) |
| 401–541 | LOW (1–20%) |
| <401 | CRITICAL (0%) |

### 1.4 Таблица ADC → оставшееся время (эмпирическая)

Из тестового разряда. Время — от момента достижения данного ADC до ADC=400 (CRITICAL):

| ADC | До ADC=400 (мин) | Локальная скорость (ADC/мин) |
|-----|-------------------|------------------------------|
| 725 | 145 | 3.0 |
| 700 | 131 | 1.4 |
| 675 | 116 | 2.9 |
| 650 | 109 | 2.4 |
| 625 | 99 | 2.0 |
| 600 | 88 | 0.5 |
| 575 | 68 | 1.7 |
| 550 | 55 | 2.0 |
| 525 | 43 | 2.4 |
| 500 | 38 | 3.5 |
| 475 | 29 | 3.5 |
| 450 | 22 | 2.9 |
| 425 | 12 | 2.0 |
| 400 | 0 | — |

---

## 2. Алгоритм

### 2.1 Обзор

Два механизма, комбинированные:

1. **Lookup table** — эмпирическая таблица ADC → minutes_remaining (грубая оценка, работает сразу)
2. **Linear regression** — по последним N ADC точкам, уточняет таблицу на основе реальной скорости разряда

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
#define BAT_HIST_SIZE 10  // количество точек (по умолчанию, настраивается)
#define BAT_MIN_INTERVAL 10  // минимум секунд между точками

struct bat_sample {
    time_t t;   // epoch seconds
    int adc;    // 10-bit ADC
};

struct bat_estimator {
    struct bat_sample hist[BAT_HIST_SIZE];
    int count;       // сколько заполнено (0..BAT_HIST_SIZE)
    int head;        // следующая позиция для записи
    int remain_min;  // результат: оставшееся время (-1 = неизвестно)
    double drain_rate; // ADC/мин (положительное = разряд)
};
```

**Правила заполнения:**
1. Добавлять точку ТОЛЬКО если `battery.valid == true`
2. Добавлять ТОЛЬКО если `battery.charging == false` (при зарядке — очистить буфер, remain=-1)
3. Добавлять ТОЛЬКО если прошло `>= BAT_MIN_INTERVAL` секунд от последней точки
4. При переходе charging→discharging — очистить буфер (начинать заново)

### 2.3 Linear Regression

Когда в буфере >= 3 точки, считаем линейную регрессию `adc = slope * t + intercept`:

```c
// t_i отсчитывается от t[0] (чтобы числа были маленькие)
// slope = (N * Σ(t*adc) - Σt * Σadc) / (N * Σ(t²) - (Σt)²)

double sum_t = 0, sum_a = 0, sum_tt = 0, sum_ta = 0;
int n = est->count;
time_t t0 = est->hist[oldest].t;  // oldest точка в буфере

for (int i = 0; i < n; i++) {
    int idx = (est->head - n + i + BAT_HIST_SIZE) % BAT_HIST_SIZE;
    double t = (double)(est->hist[idx].t - t0);
    double a = (double)est->hist[idx].adc;
    sum_t += t;
    sum_a += a;
    sum_tt += t * t;
    sum_ta += t * a;
}

double denom = n * sum_tt - sum_t * sum_t;
if (denom == 0) { /* недостаточно разброса по времени */ return; }

double slope = (n * sum_ta - sum_t * sum_a) / denom;  // ADC/сек
```

Если `slope >= 0` → батарея не разряжается (или шум) → `remain_min = -1`.

Скорость разряда: `drain_rate = -slope * 60.0` (ADC/мин, положительное).

### 2.4 Lookup table (встроенная)

Таблица с шагом 25 ADC. Интерполяция линейная.

```c
// {adc, minutes_to_400}
static const struct { int adc; int min; } bat_table[] = {
    {750, 153},
    {725, 145},
    {700, 131},
    {675, 116},
    {650, 109},
    {625,  99},
    {600,  88},
    {575,  68},
    {550,  55},
    {525,  43},
    {500,  38},
    {475,  29},
    {450,  22},
    {425,  12},
    {400,   0},
};
```

**Интерполяция:**
```c
int table_lookup(int adc) {
    if (adc >= 750) return 153;
    if (adc <= 400) return 0;
    // Найти два соседних элемента, линейно интерполировать
    for (int i = 0; i < TABLE_SIZE - 1; i++) {
        if (adc >= bat_table[i+1].adc) {
            int da = bat_table[i].adc - bat_table[i+1].adc;
            int dm = bat_table[i].min - bat_table[i+1].min;
            return bat_table[i+1].min + (adc - bat_table[i+1].adc) * dm / da;
        }
    }
    return 0;
}
```

**Для correction_factor** нужна также табличная скорость при данном ADC:
```c
double table_rate(int adc) {
    // Разница минут между соседними точками / разница ADC → ADC/мин
    // Или проще: для интервала [adc-12, adc+12] посчитать delta_min из таблицы
    int t1 = table_lookup(adc + 12);
    int t2 = table_lookup(adc - 12);
    double dt = (double)(t1 - t2);  // минут на 24 ADC
    if (dt <= 0) return 2.5;  // fallback
    return 24.0 / dt;  // ADC/мин
}
```

### 2.5 Комбинированная оценка

```c
void bat_estimate(struct bat_estimator *est) {
    if (est->count < 3) {
        // Недостаточно данных — только по таблице
        int cur_adc = /* последняя точка */ ;
        est->remain_min = table_lookup(cur_adc);
        est->drain_rate = 0;
        return;
    }

    // Linear regression → slope (ADC/сек)
    double slope = calc_slope(est);  // см. 2.3

    if (slope >= 0) {
        est->remain_min = -1;  // не разряжается
        est->drain_rate = 0;
        return;
    }

    double measured_rate = -slope * 60.0;  // ADC/мин (положительное)
    est->drain_rate = measured_rate;

    int cur_adc = /* newest point */ ;
    int tab_min = table_lookup(cur_adc);
    double tab_rate = table_rate(cur_adc);

    // Коррекция: если разряд быстрее чем в таблице — время меньше
    double correction = tab_rate / measured_rate;

    // Clamp коррекцию (защита от выбросов)
    if (correction < 0.3) correction = 0.3;
    if (correction > 3.0) correction = 3.0;

    est->remain_min = (int)(tab_min * correction * user_cal_factor);
    if (est->remain_min < 0) est->remain_min = 0;
}
```

### 2.6 Когда НЕ показывать время

- `charging == true` → показывать "зарядка", не время
- `valid == false` → показывать "?", не время
- `count < 3` И `adc > 700` → только что отключили зарядку, данных мало, таблица ненадёжна при высоком ADC
- `adc < 100` → батарея мёртвая, "0 мин"

---

## 3. Калибровка

### 3.1 Файл конфигурации

`/etc/lcd/bat_cal.json`:
```json
{
    "cutoff_adc": 400,
    "time_factor": 1.0,
    "hist_size": 10,
    "min_interval": 10
}
```

| Поле | Тип | Default | Описание |
|------|-----|---------|----------|
| `cutoff_adc` | int | 400 | ADC значение "батарея мёртвая". Таблица ведёт к этому. Если хочется запас — поставить 420 |
| `time_factor` | float | 1.0 | Глобальный множитель результата. 1.2 = "добавь 20%", 0.8 = "отними 20%" |
| `hist_size` | int | 10 | Размер кольцевого буфера. Меньше = быстрее реакция, больше шум. Больше = стабильнее, медленнее адаптация |
| `min_interval` | int | 10 | Минимум секунд между точками. Ядро обновляет ADC ~каждые 12с, ставить меньше бессмысленно |

### 3.2 Как калибровать

1. Зарядить батарею полностью (ADC ~740–750)
2. Отключить зарядку, засечь время
3. Дождаться выключения (или ADC ~400)
4. Сравнить реальное время с оценкой
5. Скорректировать `time_factor`:
   ```
   time_factor = реальное_время / оценённое_время * старый_factor
   ```

### 3.3 Замена таблицы

Таблица `bat_table[]` вшита в C-код. При смене батареи/условий — перезаписать новый тест разряда и пересчитать. Скрипт: `almond-batr-drain/build_table.py`.

---

## 4. Интеграция в data_collector.c

### 4.1 Что добавить

**Структуры** (после `struct battery_info`):
```c
#define BAT_HIST_MAX 20

struct bat_sample { time_t t; int adc; };

struct bat_estimator {
    struct bat_sample hist[BAT_HIST_MAX];
    int count, head;
    int remain_min;
    double drain_rate;
    int was_charging;  // для детекции перехода charge→discharge
};

static struct bat_estimator bat_est = {0};
```

**Конфигурация** (глобалы, читаются из файла при старте):
```c
static int bat_cal_cutoff = 400;
static double bat_cal_factor = 1.0;
static int bat_cal_hist_size = 10;
static int bat_cal_interval = 10;
```

**Функции:**
1. `bat_cal_load()` — прочитать `/etc/lcd/bat_cal.json` при старте
2. `bat_hist_push(struct bat_estimator *e, time_t t, int adc)` — добавить точку в ring buffer
3. `bat_hist_clear(struct bat_estimator *e)` — очистить буфер
4. `bat_table_lookup(int adc)` — интерполяция по таблице → минуты
5. `bat_table_rate(int adc)` — табличная скорость разряда ADC/мин
6. `bat_calc_slope(struct bat_estimator *e)` — линейная регрессия → ADC/сек
7. `bat_update(struct bat_estimator *e, struct battery_info *bi)` — главная функция, вызывается после `get_battery()`

### 4.2 Вызов в main loop

```c
// В main while(running) loop, после get_battery(&bat):
get_battery(&bat);
bat_update(&bat_est, &bat);   // ← ДОБАВИТЬ
```

### 4.3 JSON output

Расширить формат:
```c
"\"battery\":{\"adc\":%d,\"percent\":%d,\"charging\":%s,\"valid\":%s,"
"\"remain_min\":%d,\"drain_rate\":%.1f},"
...
bat.adc, bat.percent,
bat.charging?"true":"false", bat.valid?"true":"false",
bat_est.remain_min, bat_est.drain_rate
```

### 4.4 Отображение в lcd_ui.uc

В блоке отрисовки батареи (строка ~460 lcd_ui.uc), после процента:
```javascript
let bat_txt = bvalid ? bpct + "%" : "?";
let remain = bat?.remain_min;
if (remain != null && remain >= 0 && !bchg) {
    if (remain >= 60)
        bat_txt += " ~" + int(remain / 60) + "h" + sprintf("%02d", remain % 60);
    else
        bat_txt += " ~" + remain + "m";
}
```

---

## 5. Точность оценки

Из анализа тестовых данных (N=10, cutoff=400):

| Диапазон ADC | Медианная ошибка | P25–P75 |
|-------------|-----------------|---------|
| 600–750 | -1% | -42% .. +85% |
| 500–600 | +32% | -4% .. +92% |
| 400–500 | -6% | -30% .. +60% |
| **Общая** | **+11%** | **-28% .. +86%** |

Высокий разброс (P25–P75) объясняется шумом ADC ±2–3 единицы при коротком окне.
Комбинированный метод (таблица + коррекция) значительно стабильнее чистого linreg,
потому что таблица даёт базовую оценку, а linreg только корректирует.

**Ожидаемая точность комбинированного метода: ±15–25%** в диапазоне ADC 450–700.

---

## 6. Справка: файлы

| Файл | Описание |
|------|----------|
| `modules/data_collector.c` | **СЮДА ВНОСИТЬ ИЗМЕНЕНИЯ** — C daemon |
| `modules/lcd_ui.uc` | UI — добавить отображение remain_min |
| `Battery_Drain_logs1.txt` | Сырой лог разряда (1213 строк) |
| `almond-batr-drain/discharge_curve.png` | График кривой разряда |
| `almond-batr-drain/drain_data.json` | Парсенные данные (JSON: [[ts, adc, charger], ...]) |
| `almond-batr-drain/parse_drain.py` | Парсер лога + график |
| `almond-batr-drain/build_table.py` | Генератор таблицы + анализ точности |
| `/etc/lcd/bat_cal.json` | Конфиг калибровки (на роутере) |

---

## 7. Сводка: что делать

1. В `data_collector.c` добавить `struct bat_estimator` + ring buffer + linreg + lookup table
2. В main loop вызывать `bat_update()` после `get_battery()`
3. Расширить JSON: поля `remain_min`, `drain_rate`
4. В `lcd_ui.uc` показать оставшееся время рядом с процентом батареи
5. Создать `/etc/lcd/bat_cal.json` с defaults
6. Собрать: `./build.sh userspace && ./build.sh deploy`
