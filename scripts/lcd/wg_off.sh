#!/bin/sh
# WG OFF — остановить WireGuard (route убирает hotplug 90-wg-route)
ifdown wg0 2>/dev/null
