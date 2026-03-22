#!/bin/sh
# WG ON — поднять WireGuard (route через hotplug 90-wg-route)
ifup wg0 2>/dev/null
