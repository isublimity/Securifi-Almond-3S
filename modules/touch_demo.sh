#!/bin/sh
# Touch demo: show tap coordinates on LCD
# Uses lcd_render + lcdlib touch ioctl via simple C helper

SOCK="/tmp/lcd.sock"

send() {
    echo "$1" | socat - UNIX-CONNECT:$SOCK 2>/dev/null
}

# Start lcd_render if not running
pidof lcd_render >/dev/null || /tmp/lcd_render &
sleep 1

# Draw initial screen
send '{"cmd":"rect","x":0,"y":0,"w":320,"h":240,"color":"#0000"}'
send '{"cmd":"text","x":40,"y":20,"text":"TOUCH DEMO","color":"#FFE0","size":2}'
send '{"cmd":"text","x":30,"y":50,"text":"Tap anywhere!","color":"#FFFF","size":1}'
send '{"cmd":"flush"}'

# Poll touch via /dev/lcd ioctl using helper
/tmp/touch_poll
