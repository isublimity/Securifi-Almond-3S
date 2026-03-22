#!/bin/sh
# Backlight control: backlight.sh 0|1|2 (off/on/splash)
touch_poll bl "${1:-1}" 2>/dev/null
