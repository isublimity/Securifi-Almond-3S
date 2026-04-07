# Securifi Almond 3S — Архитектура LCD/UI системы

## Версионирование
Формат: **V{YY}{MM}{NN}** — YY=год, MM=месяц, NN=номер сборки
Пример: `V260401` = 2026, апрель, сборка 01

При старте каждый модуль пишет в dmesg:
```
<module_name> V260401 by Sublimity — START
```

## Процессы

```
┌──────────────────────────────────────────┐
│  lcd_drv.ko V260401 (kernel)              │
│  - Framebuffer /dev/lcd (mmap 320x240)    │
│  - Touch thread (SX8650 poll 50ms)        │
│  - PIC battery (cmd 0x39+0x36, 10s)       │
│  - ioctl: 0=flush, 1=touch, 2=battery     │
└──────────────────────────────────────────┘
        ↕ mmap + ioctl (/dev/lcd)
┌──────────────────────────────────────────┐
│  lcd_server V260401 (C, static binary)    │
│  ONE process, multiple threads:           │
│                                           │
│  main thread: socket server               │
│    - /tmp/lcd.sock (accept UI clients)    │
│    - dispatch draw cmds → render          │
│    - dispatch events → UI clients         │
│                                           │
│  render thread:                           │
│    - mmap /dev/lcd framebuffer            │
│    - draw: rect, text, image, clear       │
│    - flush on command                     │
│                                           │
│  touch thread:                            │
│    - ioctl(fd, 1) poll every 50ms         │
│    - push touch events to all clients     │
│                                           │
│  data thread:                             │
│    - connect to /tmp/lcd_data.sock        │
│    - receive JSON from data_collector     │
│    - push data updates to all clients     │
└──────────────────────────────────────────┘
        ↕ /tmp/lcd.sock (bidirectional JSON)
┌──────────────────────────────────────────┐
│  lcd_ui.uc V260401 (ucode script)         │
│  - Connect to /tmp/lcd.sock               │
│  - Receive: touch + data events           │
│  - Send: draw commands                    │
│  - UI pages, menus, transitions           │
└──────────────────────────────────────────┘

┌──────────────────────────────────────────┐
│  data_collector V260401 (C, static)       │
│  - Socket server: /tmp/lcd_data.sock      │
│  - Push JSON every 2s to all clients      │
│  - LTE: AT cmds via /dev/ttyACM*          │
│  - WiFi: iwinfo/ubus                      │
│  - VPN: wg show / interface status        │
│  - Battery: ioctl(/dev/lcd, 2)            │
│  - System: uptime, memory, CPU            │
└──────────────────────────────────────────┘
```

## Socket протоколы

### /tmp/lcd.sock (lcd_server ↔ lcd_ui)

**UI → server (draw commands):**
```json
{"cmd":"clear","color":"#000000"}
{"cmd":"rect","x":0,"y":0,"w":320,"h":30,"color":"#333333"}
{"cmd":"text","x":10,"y":5,"text":"Battery: 64%","size":16,"color":"#FFFFFF"}
{"cmd":"flush"}
```

**server → UI (events):**
```json
{"event":"touch","x":150,"y":100,"state":"down"}
{"event":"touch","x":150,"y":100,"state":"up"}
{"event":"data","battery":{"adc":810,"percent":64,"charging":true},"lte":{"rsrp":-95}}
```

### /tmp/lcd_data.sock (data_collector → lcd_server)

**data_collector → lcd_server (push every 2s):**
```json
{
  "ts": 1712505600,
  "lte": {"csq":15,"rsrp":-85,"rsrq":-8,"band":"B7","operator":"MTS","ip":"10.0.0.1"},
  "wifi": {"clients":[{"mac":"AA:BB:CC:DD:EE:FF","signal":-45}]},
  "vpn": {"active":true,"type":"wireguard","ping_ms":12},
  "battery": {"adc":810,"percent":64,"charging":true,"valid":true},
  "ping": {"google_ms":15},
  "uptime": 86400,
  "mem_free_mb": 170,
  "cpu_load": 0.25
}
```

## Запуск

### Порядок:
1. `lcd_drv.ko` — autoload при boot (modules.d/90-lcd-drv)
2. `data_collector` — сразу после boot
3. `lcd_server` — после lcd_drv загружен
4. `lcd_ui.uc` — после lcd_server создал /tmp/lcd.sock

### Init script (/etc/init.d/lcd_ui):
```sh
#!/bin/sh /etc/rc.common
START=99
USE_PROCD=1

start_service() {
    procd_open_instance data_collector
    procd_set_param command /usr/bin/data_collector
    procd_set_param respawn
    procd_close_instance

    procd_open_instance lcd_server
    procd_set_param command /usr/bin/lcd_server
    procd_set_param respawn
    procd_close_instance

    # lcd_ui starts after lcd_server socket ready
    procd_open_instance lcd_ui
    procd_set_param command /usr/bin/ucode /usr/bin/lcd_ui.uc
    procd_set_param respawn
    procd_close_instance
}
```

## Сборка

```bash
cd Securifi-Almond-3S/
./build.sh kernel      # lcd_drv.ko (build server)
./build.sh userspace   # lcd_server, data_collector (zig cc)
./build.sh deploy      # scp + restart
./build.sh firmware    # full OpenWrt image
```

## Файлы на роутере

```
/lib/modules/6.12.74/lcd_drv.ko   — kernel module
/usr/bin/lcd_server                — render + touch + socket
/usr/bin/data_collector            — stats daemon
/usr/bin/lcd_ui.uc                 — UI script
/etc/lcd/settings.lua              — UI config
/dev/lcd                           — framebuffer + ioctl
/tmp/lcd.sock                      — UI ↔ server socket
/tmp/lcd_data.sock                 — data → server socket
```
