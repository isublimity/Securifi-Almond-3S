#!/bin/sh
# modem_setup.sh — Настройка Fibocom L860-GL на Almond 3S
# Запускать на роутере: sh modem_setup.sh
# Или через SSH: ssh root@192.168.11.1 'sh /tmp/modem_setup.sh'

set -e

echo "=== Fibocom L860-GL Modem Setup ==="

# 1. Проверка модема
if [ ! -c /dev/cdc-wdm0 ]; then
    echo "ERROR: /dev/cdc-wdm0 not found — modem not detected"
    exit 1
fi
echo "✓ Modem detected: /dev/cdc-wdm0"

# 2. Загрузка CDC-ACM для AT-команд (если нет)
if [ ! -c /dev/ttyACM0 ]; then
    echo "Loading cdc-acm..."
    modprobe cdc-acm 2>/dev/null || insmod /lib/modules/*/cdc-acm.ko 2>/dev/null || true
    sleep 2
fi
ls /dev/ttyACM* 2>/dev/null && echo "✓ AT ports available" || echo "⚠ No ttyACM ports"

# 3. AT-команды: очистка Verizon профилей, установка бендов
echo "--- AT Configuration ---"
PORT=""
for p in /dev/ttyACM0 /dev/ttyACM1 /dev/ttyACM2; do
    [ -c "$p" ] && PORT="$p" && break
done

if [ -n "$PORT" ]; then
    at_cmd() {
        rm -f /tmp/at_r
        cat "$PORT" > /tmp/at_r 2>/dev/null &
        ATPID=$!
        sleep 1
        printf "%s\r" "$1" > "$PORT"
        sleep 3
        kill $ATPID 2>/dev/null; wait $ATPID 2>/dev/null
        RESP=$(cat /tmp/at_r 2>/dev/null | tr "\r" "\n" | grep -E "^\+|^OK$|^ERROR$" | head -3)
        echo "  $1 => $RESP"
    }

    # Flush buffer
    at_cmd "AT"

    # Check current bands
    at_cmd "AT+XACT?"

    # If bands are wrong (Verizon B2/B4/B5/B13), set Russian bands
    BANDS=$(cat /tmp/at_r 2>/dev/null | tr "\r" "\n" | grep "+XACT:" | head -1)
    case "$BANDS" in
        *102,104,105,113*)
            echo "⚠ Verizon bands detected! Setting Russian bands..."
            at_cmd "AT+XACT=2,,,101,103,107,108,120,138"
            ;;
        *101,103,107*)
            echo "✓ Russian bands already set"
            ;;
    esac

    # Check/set APN
    at_cmd "AT+CGDCONT?"
    APN=$(cat /tmp/at_r 2>/dev/null | tr "\r" "\n" | grep "+CGDCONT:" | head -1)
    case "$APN" in
        *internet.beeline.ru*)
            echo "✓ APN = internet.beeline.ru"
            ;;
        *vzwinternet*|*VZWIMS*|*3gnet*)
            echo "⚠ Wrong APN! Fixing..."
            at_cmd "AT+CGDCONT=0"
            at_cmd "AT+CGDCONT=1"
            at_cmd "AT+CGDCONT=2"
            at_cmd "AT+CGDCONT=1,\"IP\",\"internet.beeline.ru\""
            echo "APN set. Rebooting modem..."
            at_cmd "AT+CFUN=15"
            echo "Waiting 30s for modem reboot..."
            sleep 30
            ;;
        *)
            echo "Setting APN..."
            at_cmd "AT+CGDCONT=1,\"IP\",\"internet.beeline.ru\""
            ;;
    esac
else
    echo "⚠ No AT port — skipping AT configuration"
fi

# 4. Настройка сетевого интерфейса
echo "--- Network Configuration ---"
uci set network.lte=interface
uci set network.lte.proto='mbim'
uci set network.lte.device='/dev/cdc-wdm0'
uci set network.lte.apn='internet.beeline.ru'
uci set network.lte.pdptype='ipv4'
uci set network.lte.metric='10'
uci commit network
echo "✓ LTE interface configured"

# 5. Firewall
if ! uci get firewall.@zone[1].network 2>/dev/null | grep -q lte; then
    uci add_list firewall.@zone[1].network='lte'
    uci commit firewall
    echo "✓ LTE added to WAN firewall zone"
else
    echo "✓ LTE already in WAN zone"
fi

# 6. Поднять интерфейс
echo "--- Bringing up LTE ---"
ifdown lte 2>/dev/null
sleep 2
ifup lte
sleep 15

# 7. Проверка
echo "=== Status ==="
umbim -d /dev/cdc-wdm0 registration 2>&1 | grep -E "registerstate|provider_name"
echo "--- IP ---"
ip addr show wwan0 2>&1 | grep "inet " || echo "⚠ No IP yet (may need more time)"
echo "--- Route ---"
ip route | grep wwan0 | head -2 || echo "⚠ No route via wwan0"
echo "--- Ping ---"
ping -c1 -W5 8.8.8.8 2>&1 | grep -E "bytes from|100%" || echo "⚠ No internet"

echo ""
echo "=== Done ==="
echo "If no internet: wait 1-2 min and run 'ifup lte' again"
echo "Check logs: logread | grep mbim"
echo "Manual AT: picocom /dev/ttyACM0 (Ctrl-A Ctrl-X to exit)"
