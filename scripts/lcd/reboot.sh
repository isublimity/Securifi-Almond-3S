#!/bin/sh
# Reboot — switch LCD to boot console before rebooting
# Kill UI processes, show splash/console, then reboot
killall touch_poll 2>/dev/null
kill $(pidof ucode) 2>/dev/null
killall lcd_render data_collector 2>/dev/null
# ioctl(5, 99) = random splash scene while shutting down
# ioctl(4, 1) = backlight on
sleep 1
reboot
