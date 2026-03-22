#!/bin/sh
#
# first_setup.sh — Настройка Almond 3S после прошивки
#
# Запуск на роутере:
#   sh first_setup.sh --all           — полная настройка
#   sh first_setup.sh --setup_wifi    — только WiFi
#   sh first_setup.sh --setup_lte     — только LTE модем
#   sh first_setup.sh --setup_ui      — только LCD UI
#   sh first_setup.sh --setup_vpn     — только WireGuard
#   sh first_setup.sh --setup_system  — система (hostname, timezone, irq)
#
# Скачать и запустить одной командой:
#   wget -O- https://raw.githubusercontent.com/isublimity/Securifi-Almond-3S/main/first_setup.sh | sh -s -- --all
#

REPO="https://raw.githubusercontent.com/isublimity/Securifi-Almond-3S/main"
SCRIPTS_DIR="/etc/lcd/scripts"
MODEM_GPIO="/sys/devices/platform/1e000000.palmbus/1e000600.gpio/gpiochip1/gpio/modem_reset/value"

log() { echo "[$(date +%H:%M:%S)] $1"; }

# =============================================
#  SYSTEM
# =============================================
setup_system() {
    log "=== System setup ==="

    # Hostname + timezone
    uci set system.@system[0].hostname='Almond3S'
    uci set system.@system[0].zonename='Europe/Moscow'
    uci set system.@system[0].timezone='MSK-3'
    uci commit system

    # LAN IP
    uci set network.lan.ipaddr='192.168.11.1'
    uci commit network

    # IRQ affinity
    grep -q "smp_affinity" /etc/rc.local || {
        sed -i '/^exit 0/i\
# IRQ optimization\
echo 2 > "/proc/irq/29/smp_affinity"   # USB -> Core1t2\
echo 4 > "/proc/irq/31/smp_affinity"   # WiFi 2.4GHz -> Core2t1\
echo 8 > "/proc/irq/32/smp_affinity"   # WiFi 5GHz -> Core2t2' /etc/rc.local
    }

    log "  hostname=Almond3S, LAN=192.168.11.1, TZ=MSK-3"
}

# =============================================
#  WIFI
# =============================================
setup_wifi() {
    log "=== WiFi setup ==="

    # WiFi driver bind (MT7662E)
    grep -q "14c3 7662" /etc/rc.local || {
        sed -i '/^exit 0/i\
# WiFi MT7662E bind\
echo "14c3 7662" > /sys/bus/pci/drivers/mt7615e/new_id 2>/dev/null' /etc/rc.local
    }

    # Detect which radio is 5GHz and which is 2.4GHz
    # phy0 may be 5G or 2.4G depending on PCI enumeration
    PHY0_BAND=$(iw phy phy0 info 2>/dev/null | grep -c "Band 2")
    if [ "$PHY0_BAND" -gt 0 ]; then
        # radio0=5GHz, radio1=2.4GHz
        R5="radio0"; R2="radio1"
    else
        R5="radio1"; R2="radio0"
    fi

    # 5 GHz (ch165 VHT20, country CN — furthest from Zeekr ch149)
    uci set wireless.$R5.disabled='0'
    uci set wireless.$R5.band='5g'
    uci set wireless.$R5.channel='165'
    uci set wireless.$R5.htmode='VHT20'
    uci set wireless.$R5.country='CN'
    uci set wireless.default_$R5.ssid='Almond-5G'
    uci set wireless.default_$R5.encryption='psk2'
    uci set wireless.default_$R5.key='12345678'
    uci set wireless.default_$R5.disabled='0'

    # 2.4 GHz (ch6 HT40, country CN)
    uci set wireless.$R2.disabled='0'
    uci set wireless.$R2.band='2g'
    uci set wireless.$R2.channel='6'
    uci set wireless.$R2.htmode='HT40'
    uci set wireless.$R2.country='CN'
    uci set wireless.default_$R2.ssid='Almond'
    uci set wireless.default_$R2.encryption='psk2'
    uci set wireless.default_$R2.key='12345678'
    uci set wireless.default_$R2.disabled='0'

    uci commit wireless
    wifi reload 2>/dev/null || wifi up 2>/dev/null

    log "  5G($R5): Almond-5G ch165 VHT20, 2.4G($R2): Almond ch6 HT40"
}

# =============================================
#  LTE
# =============================================
setup_lte() {
    log "=== LTE setup ==="

    # GPIO reset modem (Fibocom needs hard reset after cold boot)
    log "  GPIO reset modem..."
    ifdown lte 2>/dev/null
    sleep 2
    if [ -f "$MODEM_GPIO" ]; then
        echo 1 > "$MODEM_GPIO"; sleep 3; echo 0 > "$MODEM_GPIO"
        log "  Waiting 15s for modem..."
        sleep 15
    fi

    # Verify MBIM responds (not just USB enumeration)
    if [ -c /dev/cdc-wdm0 ]; then
        umbim -n -d /dev/cdc-wdm0 caps >/dev/null 2>&1
        if [ $? -ne 0 ]; then
            log "  MBIM timeout, second reset..."
            echo 1 > "$MODEM_GPIO"; sleep 3; echo 0 > "$MODEM_GPIO"
            sleep 15
        fi
    fi

    if [ ! -c /dev/cdc-wdm0 ]; then
        log "  ERROR: modem not found after reset"
        return 1
    fi
    log "  Modem detected"

    # AT: очистка APN
    AT_PORT=""
    for p in /dev/ttyACM2 /dev/ttyACM1 /dev/ttyACM0; do
        [ -c "$p" ] && AT_PORT="$p" && break
    done

    if [ -n "$AT_PORT" ]; then
        log "  AT port: $AT_PORT"
        cat "$AT_PORT" > /tmp/_at_setup &
        ATPID=$!
        sleep 1
        # Clear Verizon APN profiles
        for cid in 1 2 3 4 5 6 7 8; do
            printf "AT+CGDCONT=%d\r" "$cid" > "$AT_PORT"
            sleep 0.3 2>/dev/null || sleep 1
        done
        sleep 1
        # Set correct APN
        printf 'AT+CGDCONT=1,"IP","internet"\r' > "$AT_PORT"
        sleep 1
        kill $ATPID 2>/dev/null
    fi

    # UCI network interface
    uci set network.lte=interface
    uci set network.lte.proto='mbim'
    uci set network.lte.device='/dev/cdc-wdm0'
    uci set network.lte.apn='internet'
    uci set network.lte.pdptype='ipv4'
    uci set network.lte.metric='100'
    uci commit network

    # Firewall: add lte to wan zone
    local WAN_ZONE=$(uci show firewall | grep "name='wan'" | head -1 | cut -d. -f1-2)
    if [ -n "$WAN_ZONE" ]; then
        local NETS=$(uci get "$WAN_ZONE.network" 2>/dev/null)
        echo "$NETS" | grep -q "lte" || {
            uci set "$WAN_ZONE.network=$NETS lte"
            uci commit firewall
        }
    fi

    # Bring up
    ifup lte 2>/dev/null
    sleep 8
    local IP=$(ip -4 addr show wwan0 2>/dev/null | grep -o 'inet [0-9.]*' | awk '{print $2}')
    log "  LTE IP: ${IP:-not yet}"

    # LTE watchdog cron
    mkdir -p /etc/lcd/scripts
    cat > /etc/lcd/scripts/lte_reset.sh << 'LREOF'
#!/bin/sh
MODEM_GPIO="/sys/devices/platform/1e000000.palmbus/1e000600.gpio/gpiochip1/gpio/modem_reset/value"
ifdown lte 2>/dev/null
sleep 2
[ -f "$MODEM_GPIO" ] && { echo 1 > "$MODEM_GPIO"; sleep 2; echo 0 > "$MODEM_GPIO"; sleep 8; }
ifup lte 2>/dev/null
LREOF
    chmod +x /etc/lcd/scripts/lte_reset.sh

    log "  LTE configured (APN=internet, interface=lte)"
}

# =============================================
#  VPN (WireGuard)
# =============================================
setup_vpn() {
    log "=== VPN setup ==="
    log "  WireGuard interface not configured (needs keys)"
    log "  After configuring wg0, set network.wg0.defaultroute=0"

    # WG route hotplug — add route only after handshake
    cat > /etc/hotplug.d/iface/90-wg-route << 'WGEOF'
#!/bin/sh
[ "$INTERFACE" = "wg0" ] || exit 0
case "$ACTION" in
    ifup)
        OK=0
        for i in $(seq 1 10); do
            HS=$(wg show wg0 latest-handshakes 2>/dev/null | awk '{print $2}')
            NOW=$(date +%s)
            if [ -n "$HS" ] && [ "$HS" != "0" ] && [ $((NOW - HS)) -lt 30 ]; then
                OK=1; break
            fi
            sleep 1
        done
        [ "$OK" = "1" ] && ip route replace default dev wg0 metric 10
        ;;
    ifdown)
        ip route del default dev wg0 2>/dev/null
        ;;
esac
WGEOF
    chmod +x /etc/hotplug.d/iface/90-wg-route

    # UI shell scripts
    mkdir -p /etc/lcd/scripts
    cat > /etc/lcd/scripts/wg_on.sh << 'EOF'
#!/bin/sh
ifup wg0 2>/dev/null
EOF
    cat > /etc/lcd/scripts/wg_off.sh << 'EOF'
#!/bin/sh
ifdown wg0 2>/dev/null
EOF
    cat > /etc/lcd/scripts/reboot.sh << 'EOF'
#!/bin/sh
reboot
EOF
    cat > /etc/lcd/scripts/backlight.sh << 'EOF'
#!/bin/sh
touch_poll bl "${1:-1}" 2>/dev/null
EOF
    chmod +x /etc/lcd/scripts/*.sh

    log "  WG hotplug + UI scripts installed"
}

# =============================================
#  LCD UI
# =============================================
setup_ui() {
    log "=== LCD UI setup ==="

    # Скачиваем бинарники и скрипты
    log "  Downloading binaries..."
    wget -qO /usr/bin/lcd_render    "$REPO/out/lcd_render"    2>/dev/null
    wget -qO /usr/bin/touch_poll    "$REPO/out/touch_poll"    2>/dev/null
    wget -qO /usr/bin/data_collector "$REPO/out/data_collector" 2>/dev/null
    chmod +x /usr/bin/lcd_render /usr/bin/touch_poll /usr/bin/data_collector

    log "  Downloading UI script..."
    wget -qO /usr/bin/lcd_ui.uc     "$REPO/modules/lcd_ui.uc" 2>/dev/null
    chmod +x /usr/bin/lcd_ui.uc

    log "  Downloading settings..."
    mkdir -p /etc/lcd/scripts
    wget -qO /etc/lcd/settings.lua  "$REPO/modules/settings.lua" 2>/dev/null

    # Init script (S99 — after network)
    cat > /etc/init.d/lcd_ui << 'INITEOF'
#!/bin/sh /etc/rc.common
START=99
STOP=10
USE_PROCD=1
start_service() {
    [ -c /dev/lcd ] && echo -n "touch_start" > /dev/lcd 2>/dev/null && sleep 1
    procd_open_instance lcd_render
    procd_set_param command /usr/bin/lcd_render
    procd_set_param respawn
    procd_close_instance
    procd_open_instance data_collector
    procd_set_param command /usr/bin/data_collector
    procd_set_param respawn
    procd_close_instance
    /usr/bin/touch_poll daemon
    sleep 2
    procd_open_instance lcd_ui
    procd_set_param command /usr/bin/ucode /usr/bin/lcd_ui.uc
    procd_set_param respawn
    procd_set_param stderr 1
    procd_close_instance
}
stop_service() { killall touch_poll 2>/dev/null; }
INITEOF
    chmod +x /etc/init.d/lcd_ui
    /etc/init.d/lcd_ui enable

    # Start now
    /etc/init.d/lcd_ui start 2>/dev/null

    log "  LCD UI installed and started"
}

# =============================================
#  MAIN
# =============================================

if [ $# -eq 0 ]; then
    echo "Usage: $0 [--all|--setup_system|--setup_wifi|--setup_lte|--setup_vpn|--setup_ui]"
    echo ""
    echo "  --all           Full setup (system + wifi + lte + vpn + ui)"
    echo "  --setup_system  Hostname, timezone, LAN IP, IRQ"
    echo "  --setup_wifi    WiFi 2.4G + 5G (Almond / Almond-5G)"
    echo "  --setup_lte     LTE modem (Fibocom L860-GL, APN=internet)"
    echo "  --setup_vpn     WireGuard hotplug + UI scripts"
    echo "  --setup_ui      LCD UI (lcd_render + touch_poll + data_collector + lcd_ui.uc)"
    exit 0
fi

for arg in "$@"; do
    case "$arg" in
        --all)
            setup_system
            setup_wifi
            setup_lte
            setup_vpn
            setup_ui
            ;;
        --setup_system) setup_system ;;
        --setup_wifi)   setup_wifi ;;
        --setup_lte)    setup_lte ;;
        --setup_vpn)    setup_vpn ;;
        --setup_ui)     setup_ui ;;
        *)
            echo "Unknown option: $arg"
            exit 1
            ;;
    esac
done

log "=== Setup complete ==="
