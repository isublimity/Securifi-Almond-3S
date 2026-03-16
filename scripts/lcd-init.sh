#!/bin/sh /etc/rc.common
#
# lcd-init — Запуск LCD стека при загрузке Almond 3S
#
# Порядок:
#   1. Загрузка lcd_drv.ko (splash 4PDA на 1.5 сек)
#   2. rmmod i2c_mt7621 (конфликт с тачем)
#   3. Запуск lcd_render (unix socket сервер)
#   4. Ожидание lcd_ui.lua (если нет — показать "No lcd_ui")
#

START=99
STOP=10

LCD_MOD="/lib/modules/$(uname -r)/lcd_drv.ko"
LCD_MOD_TMP="/tmp/lcd_drv.ko"
LCD_RENDER="/tmp/lcd_render"
LCD_UI="/tmp/lcd_ui.lua"
LCDLIB="/usr/lib/lua/lcdlib.so"

start() {
    # 1. Kernel module (shows splash)
    if [ -f "$LCD_MOD" ]; then
        rmmod i2c_mt7621 2>/dev/null
        rmmod lcd_drv 2>/dev/null
        insmod "$LCD_MOD"
    elif [ -f "$LCD_MOD_TMP" ]; then
        rmmod i2c_mt7621 2>/dev/null
        rmmod lcd_drv 2>/dev/null
        insmod "$LCD_MOD_TMP"
    else
        logger -t lcd-init "lcd_drv.ko not found"
        return 1
    fi

    sleep 2

    # 2. lcd_render (socket server)
    if [ -x "$LCD_RENDER" ]; then
        killall lcd_render 2>/dev/null
        $LCD_RENDER &
        sleep 1
    else
        logger -t lcd-init "lcd_render not found"
    fi

    # 3. LCD UI
    if [ -f "$LCD_UI" ] && [ -f "$LCDLIB" ]; then
        killall -9 lua 2>/dev/null
        lua "$LCD_UI" > /tmp/lcd_ui.log 2>&1 &
        logger -t lcd-init "lcd_ui.lua started"
    else
        # No UI — show message on display
        logger -t lcd-init "lcd_ui.lua or lcdlib.so not found"
        if [ -x "$LCD_RENDER" ]; then
            $LCD_RENDER '{"cmd":"clear","color":"#000080"}'
            $LCD_RENDER '{"cmd":"text","x":30,"y":60,"text":"No lcd_ui.lua","color":"red","size":3}'
            $LCD_RENDER '{"cmd":"text","x":20,"y":110,"text":"Run deploy-router.sh","color":"white","size":2}'
            $LCD_RENDER '{"cmd":"text","x":20,"y":150,"text":"to install LCD UI","color":"#4208","size":2}'
        fi
    fi
}

stop() {
    killall lua 2>/dev/null
    killall lcd_render 2>/dev/null
    rmmod lcd_drv 2>/dev/null
}

restart() {
    stop
    sleep 1
    start
}

status() {
    echo "lcd_drv: $(lsmod | grep lcd_drv | awk '{print "loaded"}' || echo 'not loaded')"
    echo "lcd_render: $(pgrep lcd_render >/dev/null && echo 'running' || echo 'stopped')"
    echo "lcd_ui: $(pgrep -f lcd_ui.lua >/dev/null && echo 'running' || echo 'stopped')"
    [ -f /tmp/lcd_ui.log ] && tail -3 /tmp/lcd_ui.log
}
