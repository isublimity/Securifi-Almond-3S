#!/bin/sh
#
# lte-band-test.sh — Тест LTE бендов для выбора лучшего
#
# Перебирает бенды B1/B3/B7/B8/B20, на каждом ждёт подключения,
# замеряет RSRP/SINR/RSRQ, выдаёт рекомендацию.
#
# Использование:
#   ./lte-band-test.sh          — тест всех бендов
#   ./lte-band-test.sh lock 3   — заблокировать на Band 3
#   ./lte-band-test.sh unlock   — разблокировать все бенды
#

AT_PORT="/dev/ttyUSB2"
QMI_DEV="/dev/cdc-wdm0"
IFACE="lte"
WAIT_SEC=15

at_cmd() {
    (echo -e "$1\r"; sleep 2) | socat -T4 -t4 STDIO "$AT_PORT",crnl,nonblock 2>/dev/null
}

get_signal() {
    uqmi -d "$QMI_DEV" --get-signal-info 2>/dev/null
}

get_band() {
    at_cmd 'AT+QNWINFO' | grep QNWINFO | head -1
}

set_band_mask() {
    local mask="$1"
    at_cmd "AT+QCFG=\"band\",0,$mask,0" >/dev/null 2>&1
}

# Бенды EC21-E для Билайн
BANDS="1 3 7 8 20"
BAND_MASKS_1="0x1"
BAND_MASKS_3="0x4"
BAND_MASKS_7="0x40"
BAND_MASKS_8="0x80"
BAND_MASKS_20="0x80000"
BAND_ALL="0x800d5"

case "$1" in
    lock)
        band="$2"
        if [ -z "$band" ]; then echo "Использование: $0 lock <band>"; exit 1; fi
        eval mask="\$BAND_MASKS_$band"
        if [ -z "$mask" ]; then echo "Неизвестный бенд: $band"; exit 1; fi
        echo "Блокировка на Band $band (маска $mask)..."
        set_band_mask "$mask"
        sleep 2
        ifdown "$IFACE"; sleep 3; ifup "$IFACE"
        sleep 10
        echo "Сигнал:"
        get_signal
        echo "Бенд:"
        get_band
        exit 0
        ;;
    unlock)
        echo "Разблокировка всех бендов ($BAND_ALL)..."
        set_band_mask "$BAND_ALL"
        sleep 2
        ifdown "$IFACE"; sleep 3; ifup "$IFACE"
        sleep 10
        echo "Сигнал:"
        get_signal
        exit 0
        ;;
esac

# === Тест всех бендов ===

echo "========================================="
echo " LTE Band Test — Quectel EC21-E"
echo "========================================="
echo ""

RESULTS=""

for band in $BANDS; do
    eval mask="\$BAND_MASKS_$band"
    echo "--- Band $band (маска $mask) ---"

    set_band_mask "$mask"
    sleep 2
    ifdown "$IFACE" 2>/dev/null
    sleep 3
    ifup "$IFACE"

    echo -n "Ожидание подключения..."
    ok=0
    for i in $(seq 1 $WAIT_SEC); do
        sleep 1
        echo -n "."
        sig=$(uqmi -d "$QMI_DEV" --get-signal-info 2>/dev/null)
        rsrp=$(echo "$sig" | grep rsrp | grep -o '\-[0-9]*')
        if [ -n "$rsrp" ]; then
            ok=1
            break
        fi
    done
    echo ""

    if [ "$ok" = "1" ]; then
        rsrq=$(echo "$sig" | grep rsrq | grep -o '\-[0-9]*')
        sinr=$(echo "$sig" | grep snr | grep -o '\-\?[0-9.]*')
        rssi=$(echo "$sig" | grep rssi | grep -o '\-[0-9]*')
        band_info=$(get_band)

        echo "  RSRP: ${rsrp} dBm"
        echo "  RSRQ: ${rsrq} dB"
        echo "  SINR: ${sinr} dB"
        echo "  RSSI: ${rssi} dBm"
        echo "  Info: $band_info"
        RESULTS="${RESULTS}B${band}\t${rsrp}\t${rsrq}\t${sinr}\n"
    else
        echo "  НЕ ПОДКЛЮЧИЛСЯ (нет покрытия Band $band)"
        RESULTS="${RESULTS}B${band}\t---\t---\t---\n"
    fi
    echo ""
done

# Вернуть все бенды
echo "--- Восстановление всех бендов ---"
set_band_mask "$BAND_ALL"
sleep 2
ifdown "$IFACE"; sleep 3; ifup "$IFACE"

echo ""
echo "========================================="
echo " Результаты"
echo "========================================="
echo -e "Band\tRSRP\tRSRQ\tSINR"
echo -e "$RESULTS"
echo ""
echo "Рекомендация: выберите бенд с лучшим RSRP (ближе к 0)"
echo "и положительным SINR."
echo ""
echo "Заблокировать: $0 lock <номер>"
echo "Разблокировать: $0 unlock"
