#!/bin/sh
#
# lte-watchdog.sh — Автопереподключение LTE модема
#
# Проверяет каждые CHECK_INTERVAL секунд:
#   1. Есть ли интерфейс wwan0
#   2. Есть ли IP адрес
#   3. Проходит ли ping
# При сбое — переподключает LTE
#
# Установка:
#   cp lte-watchdog.sh /etc/init.d/lte-watchdog
#   chmod +x /etc/init.d/lte-watchdog
#   /etc/init.d/lte-watchdog enable
#   /etc/init.d/lte-watchdog start
#

START=99
STOP=10

CHECK_INTERVAL=60     # секунд между проверками
PING_HOST="8.8.8.8"
PING_TIMEOUT=5
FAIL_COUNT=0
MAX_FAILS=3           # сколько проверок подряд до перезапуска
LOG="/tmp/lte-watchdog.log"
IFACE="lte"

log() {
    local ts=$(date "+%Y-%m-%d %H:%M:%S")
    echo "$ts $1" >> "$LOG"
    logger -t lte-watchdog "$1"
}

check_lte() {
    # 1. Интерфейс wwan0 поднят?
    if ! ip link show wwan0 >/dev/null 2>&1; then
        return 1
    fi

    # 2. IP адрес есть?
    local ip=$(ip -4 addr show wwan0 2>/dev/null | grep -o 'inet [0-9.]*' | awk '{print $2}')
    if [ -z "$ip" ]; then
        return 2
    fi

    # 3. Ping проходит?
    if ! ping -c 1 -W "$PING_TIMEOUT" -I wwan0 "$PING_HOST" >/dev/null 2>&1; then
        return 3
    fi

    return 0
}

restart_lte() {
    log "RESTART: ifdown/ifup $IFACE"
    ifdown "$IFACE" 2>/dev/null
    sleep 5
    ifup "$IFACE"
    sleep 10

    if check_lte; then
        log "RESTART: OK — LTE восстановлен"
    else
        log "RESTART: FAIL — LTE не поднялся, попробуем сброс модема"
        # Аппаратный сброс модема через AT
        (echo -e "AT+CFUN=1,1\r"; sleep 1) | socat -T3 -t3 STDIO /dev/ttyUSB2,crnl,nonblock 2>/dev/null
        sleep 15
        ifup "$IFACE"
        sleep 10
        if check_lte; then
            log "RESTART: OK после сброса модема"
        else
            log "RESTART: FAIL после сброса. Нужна ручная проверка"
        fi
    fi
}

watchdog_loop() {
    log "START: watchdog запущен (интервал ${CHECK_INTERVAL}с, макс ${MAX_FAILS} ошибок)"
    FAIL_COUNT=0

    while true; do
        if check_lte; then
            if [ "$FAIL_COUNT" -gt 0 ]; then
                log "OK: LTE восстановился (было $FAIL_COUNT ошибок)"
            fi
            FAIL_COUNT=0
        else
            FAIL_COUNT=$((FAIL_COUNT + 1))
            local reason="unknown"
            case $? in
                1) reason="wwan0 не найден" ;;
                2) reason="нет IP адреса" ;;
                3) reason="ping не проходит" ;;
            esac
            log "FAIL ($FAIL_COUNT/$MAX_FAILS): $reason"

            if [ "$FAIL_COUNT" -ge "$MAX_FAILS" ]; then
                restart_lte
                FAIL_COUNT=0
            fi
        fi

        sleep "$CHECK_INTERVAL"
    done
}

# OpenWrt init.d совместимость
start() {
    watchdog_loop &
    echo $! > /tmp/lte-watchdog.pid
}

stop() {
    [ -f /tmp/lte-watchdog.pid ] && kill $(cat /tmp/lte-watchdog.pid) 2>/dev/null
    rm -f /tmp/lte-watchdog.pid
}

case "$1" in
    start)  start ;;
    stop)   stop ;;
    restart) stop; start ;;
    status)
        if [ -f /tmp/lte-watchdog.pid ] && kill -0 $(cat /tmp/lte-watchdog.pid) 2>/dev/null; then
            echo "running (PID $(cat /tmp/lte-watchdog.pid))"
            tail -5 "$LOG" 2>/dev/null
        else
            echo "stopped"
        fi
        ;;
    *)
        # Если запущен без аргументов — запустить в foreground (для отладки)
        watchdog_loop
        ;;
esac
