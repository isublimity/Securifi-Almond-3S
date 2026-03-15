#!/bin/sh
#
# sms.sh — Чтение и отправка SMS через Quectel EC21-E
#
# Использование:
#   ./sms.sh list              — список всех SMS
#   ./sms.sh read              — прочитать непрочитанные
#   ./sms.sh send +79001234567 "Текст"  — отправить SMS
#   ./sms.sh delete all        — удалить все SMS
#   ./sms.sh poll              — цикл проверки новых SMS (для демона)
#

AT_PORT="/dev/ttyUSB2"
SMS_LOG="/tmp/sms.log"
POLL_INTERVAL=30

at_cmd() {
    (echo -e "$1\r"; sleep 2) | socat -T4 -t4 STDIO "$AT_PORT",crnl,nonblock 2>/dev/null
}

init_sms() {
    # Текстовый режим SMS
    at_cmd "AT+CMGF=1" >/dev/null
    # Кодировка GSM (латиница + цифры)
    at_cmd 'AT+CSCS="GSM"' >/dev/null
}

list_sms() {
    init_sms
    echo "=== Все SMS ==="
    at_cmd 'AT+CMGL="ALL"'
}

read_unread() {
    init_sms
    echo "=== Непрочитанные SMS ==="
    at_cmd 'AT+CMGL="REC UNREAD"'
}

send_sms() {
    local number="$1"
    local text="$2"

    if [ -z "$number" ] || [ -z "$text" ]; then
        echo "Использование: $0 send +79001234567 \"Текст сообщения\""
        return 1
    fi

    init_sms
    echo "Отправка SMS на $number..."

    # Отправка: AT+CMGS="number"\r текст\x1a
    (echo -e "AT+CMGS=\"$number\"\r"; sleep 1; echo -e "${text}\x1a"; sleep 5) \
        | socat -T8 -t8 STDIO "$AT_PORT",crnl,nonblock 2>/dev/null

    if [ $? -eq 0 ]; then
        echo "OK: SMS отправлено"
        echo "$(date) SENT to=$number text=$text" >> "$SMS_LOG"
    else
        echo "ОШИБКА: не удалось отправить"
    fi
}

delete_sms() {
    init_sms
    if [ "$1" = "all" ]; then
        echo "Удаление всех SMS..."
        at_cmd "AT+CMGDA=\"DEL ALL\""
        echo "OK"
    else
        echo "Использование: $0 delete all"
    fi
}

# === SMS-демон: проверяет новые SMS и выполняет команды ===
#
# Поддерживаемые SMS-команды (отправить на номер SIM):
#   VPN ON    — включить WireGuard
#   VPN OFF   — выключить VPN
#   STATUS    — ответить SMS со статусом
#   REBOOT    — перезагрузка (через AT+CFUN)
#
poll_sms() {
    echo "SMS poll запущен (интервал ${POLL_INTERVAL}с)"
    echo "$(date) SMS poll started" >> "$SMS_LOG"

    while true; do
        init_sms
        local raw=$(at_cmd 'AT+CMGL="REC UNREAD"')
        local has_sms=$(echo "$raw" | grep "+CMGL:")

        if [ -n "$has_sms" ]; then
            echo "$raw" | while IFS= read -r line; do
                case "$line" in
                    *"+CMGL:"*)
                        # Извлечь индекс и номер
                        idx=$(echo "$line" | sed 's/+CMGL: \([0-9]*\).*/\1/')
                        from=$(echo "$line" | sed 's/.*"\([+0-9]*\)".*/\1/')
                        ;;
                    [A-Za-z]*)
                        # Текст SMS (следующая строка после +CMGL)
                        cmd=$(echo "$line" | tr -d '\r\n' | tr 'a-z' 'A-Z' | sed 's/^ *//')
                        echo "$(date) SMS от $from: $cmd" >> "$SMS_LOG"

                        case "$cmd" in
                            "VPN ON"|"WG ON")
                                uci set network.wgvpn.disabled=0; uci commit network; ifup wgvpn
                                echo "$(date) CMD: VPN ON" >> "$SMS_LOG"
                                ;;
                            "VPN OFF"|"WG OFF")
                                ifdown wgvpn; uci set network.wgvpn.disabled=1; uci commit network
                                echo "$(date) CMD: VPN OFF" >> "$SMS_LOG"
                                ;;
                            "STATUS")
                                local ip=$(wget -qO- -T 5 http://ifconfig.me/ip 2>/dev/null)
                                local sig=$(uqmi -d /dev/cdc-wdm0 --get-signal-info 2>/dev/null | grep rsrp | grep -o '\-[0-9]*')
                                send_sms "$from" "IP:${ip:-?} RSRP:${sig:-?}dBm Up:$(uptime | awk -F'up ' '{print $2}' | awk -F, '{print $1}')"
                                ;;
                            "REBOOT")
                                echo "$(date) CMD: REBOOT" >> "$SMS_LOG"
                                (sleep 3; echo -e "AT+CFUN=1,1\r" > "$AT_PORT") &
                                ;;
                        esac

                        # Удалить обработанное SMS
                        at_cmd "AT+CMGD=$idx" >/dev/null
                        ;;
                esac
            done
        fi

        sleep "$POLL_INTERVAL"
    done
}

# === Main ===
case "$1" in
    list)    list_sms ;;
    read)    read_unread ;;
    send)    send_sms "$2" "$3" ;;
    delete)  delete_sms "$2" ;;
    poll)    poll_sms ;;
    *)
        echo "Использование: $0 {list|read|send|delete|poll}"
        echo ""
        echo "  list              — все SMS"
        echo "  read              — непрочитанные"
        echo "  send +7... \"text\" — отправить"
        echo "  delete all        — удалить все"
        echo "  poll              — демон (проверка + выполнение команд)"
        ;;
esac
