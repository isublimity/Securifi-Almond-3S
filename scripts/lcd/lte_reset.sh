#!/bin/sh
# LTE Reset — полный перезапуск модема с GPIO HW reset
MODEM_GPIO="/sys/devices/platform/1e000000.palmbus/1e000600.gpio/gpiochip1/gpio/modem_reset/value"

ifdown lte 2>/dev/null
sleep 2

# HW reset (PERST active low)
if [ -f "$MODEM_GPIO" ]; then
    echo 1 > "$MODEM_GPIO"
    sleep 3
    echo 0 > "$MODEM_GPIO"
    sleep 15
fi

# Verify MBIM responds, retry if not
if ! umbim -n -d /dev/cdc-wdm0 caps >/dev/null 2>&1; then
    echo 1 > "$MODEM_GPIO" 2>/dev/null; sleep 3
    echo 0 > "$MODEM_GPIO" 2>/dev/null; sleep 15
fi

ifup lte 2>/dev/null
sleep 10
