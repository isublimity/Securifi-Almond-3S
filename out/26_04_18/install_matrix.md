# Build 2026-04-18 — Matrix boot splash

Drop-in replacement для `lcd_drv.ko`: заставка с падающими зелёными
символами формирует силуэт кролика, по низу — последняя строка из
kmsg. Фразы «Wake up, Neo…» → «The Matrix has you…» → «Follow the
white rabbit.» прокручиваются пока userspace не возьмёт управление.

## Файл

| Файл | Размер | Назначение |
|------|--------|-----------|
| `lcd_drv.ko` | 167K | kernel module для 24.10.6 / 6.6.127 |

## Совместимость

- **OpenWrt**: 24.10.6 stable (r29141-81be8a8869)
- **Kernel**: 6.6.127, vermagic `f31f6f85a36836e510d64a18a9a5f1bf`
- **Target**: ramips/mt7621, securifi_almond-3s
- **GCC**: 13.3.0

Модуль собран против upstream-образа из `out/26_04_16/` — vermagic
совпадает с официальным 24.10.6. Для самосборных прошивок с другой
конфигурацией vermagic может отличаться.

## Установка

```bash
# Скопировать на роутер
scp lcd_drv.ko root@192.168.11.1:/lib/modules/6.6.127/lcd_drv.ko

# Перезагрузить модуль
ssh root@192.168.11.1 '
  /etc/init.d/lcd_ui stop
  killall -9 ucode lcd_render data_collector touch_poll 2>/dev/null
  sleep 1
  rmmod lcd_drv
  sleep 1
  insmod /lib/modules/6.6.127/lcd_drv.ko
  sleep 15      # посмотреть заставку
  /etc/init.d/lcd_ui start
'
```

Автозагрузка уже настроена через `/etc/modules.d/90-lcd-drv` (создаётся
`first_setup.sh`), так что после `reboot` модуль поднимется сам, UI
запустится через procd — и заставка успеет сыграть.

## Проверка

```bash
ssh root@192.168.11.1 '
  lsmod | grep lcd_drv    # loaded
  ls -la /dev/lcd         # crw------- 10,257
  dmesg | grep lcd_drv | tail -5
'
```

Должна появиться строка `lcd_drv V260401 by Sublimity — START`.

## Отладка

- **Белый экран после insmod** → vermagic mismatch, проверь `uname -r`.
- **Мелкие артефакты / нет дождя** → SM0/palmbus конфликт, в dmesg
  будет `i2c_mt7621: transfer failed`. Обычно лечится перезагрузкой.
- **Заставка не переходит в UI** → не запущен `lcd_ui` (`ps | grep
  ucode`), или userspace не пишет в `/dev/lcd`.
- **Matrix-текст не видно** → overlap с UI: проверь `/etc/init.d/lcd_ui
  status`, должен быть остановлен на момент insmod.

## Возврат назад

Предыдущая версия — `out/26_04_16/kmod-lcd-gpio_*.ipk`. Установка:

```bash
scp out/26_04_16/kmod-lcd-gpio_*.ipk root@192.168.11.1:/tmp/
ssh root@192.168.11.1 'opkg install --force-reinstall /tmp/kmod-lcd-gpio_*.ipk && reboot'
```

## Что внутри изменилось (vs 26_04_16)

- scene 6 `scene_matrix` — падающие символы + sticky-слой для кролика
- scene dispatch: `NUM_SCENES=7`, matrix по умолчанию на boot
- `render_dmesg` полностью удалён, phase 1 в `render_fn` выпилен
- последняя строка kmsg рисуется зелёным внизу экрана (`LCD_H - 9`)
- splash держится неограниченно до первой записи в `/dev/lcd`
