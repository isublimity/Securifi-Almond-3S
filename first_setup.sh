#!/bin/sh
#
# first_setup.sh — Настройка Almond 3S после прошивки OpenWrt
#
# Бинарники (lcd_render, data_collector, touch_poll, lcd_drv.ko) уже в прошивке
# как пакеты OpenWrt. Этот скрипт настраивает систему и ставит UI скрипты.
#
# Запуск:
#   sh first_setup.sh --all             — полная настройка (без приватных ключей)
#   sh first_setup.sh --all --private   — полная + WG/OpenVPN ключи
#   sh first_setup.sh --setup_wifi      — только WiFi
#   sh first_setup.sh --setup_lte       — только LTE модем
#   sh first_setup.sh --setup_ui        — только LCD UI
#   sh first_setup.sh --setup_vpn       — VPN скрипты + hotplug
#   sh first_setup.sh --setup_private   — только приватные ключи
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

    uci set system.@system[0].hostname='Almond3S'
    uci set system.@system[0].zonename='Europe/Moscow'
    uci set system.@system[0].timezone='MSK-3'
    uci commit system

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

    # MT7662E bind
    grep -q "14c3 7662" /etc/rc.local || {
        sed -i '/^exit 0/i\
# WiFi MT7662E bind\
echo "14c3 7662" > /sys/bus/pci/drivers/mt7615e/new_id 2>/dev/null' /etc/rc.local
    }

    # Detect 5GHz radio
    PHY0_BAND=$(iw phy phy0 info 2>/dev/null | grep -c "Band 2")
    if [ "$PHY0_BAND" -gt 0 ]; then
        R5="radio0"; R2="radio1"
    else
        R5="radio1"; R2="radio0"
    fi

    # 5 GHz
    uci set wireless.$R5.disabled='0'
    uci set wireless.$R5.band='5g'
    uci set wireless.$R5.channel='165'
    uci set wireless.$R5.htmode='VHT20'
    uci set wireless.$R5.country='CN'
    uci set wireless.default_$R5.ssid='Almond-5G'
    uci set wireless.default_$R5.encryption='psk2'
    uci set wireless.default_$R5.key='12345678'
    uci set wireless.default_$R5.disabled='0'

    # 2.4 GHz
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

    # Remove conflicting interfaces
    for iface in $(uci show network 2>/dev/null | grep "device='/dev/cdc-wdm0'" | cut -d. -f2 | sort -u); do
        [ "$iface" = "lte" ] && continue
        log "  Removing conflicting interface '$iface'"
        uci delete "network.$iface"
    done
    uci commit network

    # GPIO reset modem
    log "  GPIO reset modem..."
    ifdown lte 2>/dev/null
    sleep 2
    if [ -f "$MODEM_GPIO" ]; then
        echo 1 > "$MODEM_GPIO"; sleep 3; echo 0 > "$MODEM_GPIO"
        log "  Waiting 15s for modem..."
        sleep 15
    fi

    # Verify MBIM
    if [ -c /dev/cdc-wdm0 ]; then
        umbim -n -d /dev/cdc-wdm0 caps >/dev/null 2>&1
        if [ $? -ne 0 ]; then
            log "  MBIM timeout, second reset..."
            echo 1 > "$MODEM_GPIO"; sleep 3; echo 0 > "$MODEM_GPIO"
            sleep 15
        fi
    fi

    if [ ! -c /dev/cdc-wdm0 ]; then
        log "  ERROR: modem not found"
        return 1
    fi

    # AT: clear APN profiles
    AT_PORT=""
    for p in /dev/ttyACM2 /dev/ttyACM1 /dev/ttyACM0; do
        [ -c "$p" ] && AT_PORT="$p" && break
    done
    if [ -n "$AT_PORT" ]; then
        log "  AT port: $AT_PORT"
        cat "$AT_PORT" > /tmp/_at_setup &
        ATPID=$!
        sleep 1
        for cid in 1 2 3 4 5 6 7 8; do
            printf "AT+CGDCONT=%d\r" "$cid" > "$AT_PORT"
            sleep 0.3 2>/dev/null || sleep 1
        done
        sleep 1
        printf 'AT+CGDCONT=1,"IP","internet"\r' > "$AT_PORT"
        sleep 1
        kill $ATPID 2>/dev/null
    fi

    # UCI
    uci set network.lte=interface
    uci set network.lte.proto='mbim'
    uci set network.lte.device='/dev/cdc-wdm0'
    uci set network.lte.apn='internet'
    uci set network.lte.pdptype='ipv4'
    uci set network.lte.metric='100'
    uci commit network

    # Firewall: wan zone
    WAN_ZONE=$(uci show firewall | grep "name='wan'" | head -1 | cut -d. -f1-2)
    if [ -n "$WAN_ZONE" ]; then
        EXTRA=""
        for net in $(uci -q get "$WAN_ZONE.network"); do
            case "$net" in wan|wan6|lte) continue ;; esac
            uci -q get "network.$net" >/dev/null 2>&1 && EXTRA="$EXTRA $net"
        done
        uci -q delete "$WAN_ZONE.network"
        uci add_list "$WAN_ZONE.network=wan"
        uci add_list "$WAN_ZONE.network=wan6"
        uci add_list "$WAN_ZONE.network=lte"
        for net in $EXTRA; do
            uci add_list "$WAN_ZONE.network=$net"
        done
        uci commit firewall
    fi

    ifup lte 2>/dev/null
    sleep 8
    IP=$(ip -4 addr show wwan0 2>/dev/null | grep -o 'inet [0-9.]*' | awk '{print $2}')
    log "  LTE IP: ${IP:-not yet}"
}

# =============================================
#  VPN scripts + hotplug (публичная часть)
# =============================================
setup_vpn() {
    log "=== VPN scripts ==="
    mkdir -p "$SCRIPTS_DIR"

    cat > "$SCRIPTS_DIR/vpn_wg_on.sh" << 'EOF'
#!/bin/sh
ifup wg0 2>/dev/null
EOF

    cat > "$SCRIPTS_DIR/vpn_wg_off.sh" << 'EOF'
#!/bin/sh
ifdown wg0 2>/dev/null
EOF

    cat > "$SCRIPTS_DIR/vpn_ovpn_on.sh" << 'EOF'
#!/bin/sh
ifdown wg0 2>/dev/null
ifdown l2tp_tina 2>/dev/null
killall openvpn 2>/dev/null
sleep 1
openvpn --config /etc/openvpn/sirius.ovpn --daemon --log /tmp/openvpn.log
sleep 3
/etc/init.d/firewall reload 2>/dev/null
EOF

    cat > "$SCRIPTS_DIR/vpn_ovpn_off.sh" << 'EOF'
#!/bin/sh
killall openvpn 2>/dev/null
EOF

    cat > "$SCRIPTS_DIR/vpn_l2tp_on.sh" << 'EOF'
#!/bin/sh
ifdown wg0 2>/dev/null
killall openvpn 2>/dev/null
ifup l2tp_tina
EOF

    cat > "$SCRIPTS_DIR/vpn_l2tp_off.sh" << 'EOF'
#!/bin/sh
ifdown l2tp_tina 2>/dev/null
EOF

    cat > "$SCRIPTS_DIR/vpn_off.sh" << 'EOF'
#!/bin/sh
ifdown wg0 2>/dev/null
killall openvpn 2>/dev/null
ifdown l2tp_tina 2>/dev/null
ip route del default dev wg0 2>/dev/null
EOF

    cat > "$SCRIPTS_DIR/lte_reset.sh" << 'EOF'
#!/bin/sh
MODEM_GPIO="/sys/devices/platform/1e000000.palmbus/1e000600.gpio/gpiochip1/gpio/modem_reset/value"
ifdown lte 2>/dev/null
sleep 2
if [ -f "$MODEM_GPIO" ]; then
    echo 1 > "$MODEM_GPIO"; sleep 3; echo 0 > "$MODEM_GPIO"; sleep 15
fi
if ! umbim -n -d /dev/cdc-wdm0 caps >/dev/null 2>&1; then
    echo 1 > "$MODEM_GPIO" 2>/dev/null; sleep 3
    echo 0 > "$MODEM_GPIO" 2>/dev/null; sleep 15
fi
ifup lte 2>/dev/null
EOF

    cat > "$SCRIPTS_DIR/reboot.sh" << 'EOF'
#!/bin/sh
killall touch_poll 2>/dev/null
kill $(pidof ucode) 2>/dev/null
killall lcd_render data_collector 2>/dev/null
sleep 1
reboot
EOF

    cat > "$SCRIPTS_DIR/backlight.sh" << 'EOF'
#!/bin/sh
touch_poll bl "${1:-1}" 2>/dev/null
EOF

    chmod +x "$SCRIPTS_DIR"/*.sh

    # WG route hotplug
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

    log "  VPN scripts + WG hotplug installed"
}

# =============================================
#  LCD UI (только скрипты, бинарники из пакетов)
# =============================================
setup_ui() {
    log "=== LCD UI setup ==="

    # lcd_drv autoload
    if [ ! -f /etc/modules.d/90-lcd-drv ]; then
        echo "lcd_drv" > /etc/modules.d/90-lcd-drv
        log "  Created /etc/modules.d/90-lcd-drv"
    fi

    # Fix /dev/lcd if stale
    if [ -e /dev/lcd ] && [ ! -c /dev/lcd ]; then
        rm -f /dev/lcd
    fi

    # Load lcd_drv if not loaded
    if ! lsmod | grep -q lcd_drv; then
        insmod /lib/modules/$(uname -r)/lcd_drv.ko 2>/dev/null && log "  Loaded lcd_drv.ko"
    fi

    # UI script (ucode) — единственное что качаем
    log "  Downloading lcd_ui.uc..."
    wget -qO /usr/bin/lcd_ui.uc "$REPO/modules/lcd_ui.uc" 2>/dev/null
    chmod +x /usr/bin/lcd_ui.uc

    # Init script
    cat > /etc/init.d/lcd_ui << 'INITEOF'
#!/bin/sh /etc/rc.common
START=99
STOP=10
USE_PROCD=1
start_service() {
    [ -c /dev/lcd ] || return
    echo -n "touch_start" > /dev/lcd 2>/dev/null
    sleep 1
    procd_open_instance lcd_render
    procd_set_param command /usr/bin/lcd_render
    procd_set_param respawn
    procd_close_instance
    procd_open_instance touch_poll
    procd_set_param command /usr/bin/touch_poll daemon_fg
    procd_set_param respawn
    procd_close_instance
    procd_open_instance data_collector
    procd_set_param command /usr/bin/data_collector
    procd_set_param respawn
    procd_close_instance
    sleep 2
    procd_open_instance lcd_ui
    procd_set_param command /usr/bin/ucode /usr/bin/lcd_ui.uc
    procd_set_param respawn
    procd_set_param stderr 1
    procd_close_instance
}
stop_service() {
    killall touch_poll 2>/dev/null
}
INITEOF
    chmod +x /etc/init.d/lcd_ui
    /etc/init.d/lcd_ui enable
    /etc/init.d/lcd_ui start 2>/dev/null

    log "  LCD UI installed and started"
}

# =============================================
#  PRIVATE: загрузка из отдельного файла
# =============================================
setup_private() {
    SECRETS="/tmp/secrets.sh"
    if [ -f "$SECRETS" ]; then
        log "=== Private setup (secrets.sh) ==="
        . "$SECRETS"
        log "  Private config applied"
    else
        log "=== Private setup SKIPPED ==="
        log "  File $SECRETS not found"
        log "  Copy: scp private/secrets.sh root@192.168.11.1:/tmp/"
    fi
}

# =============================================
#  MAIN
# =============================================
if [ $# -eq 0 ]; then
    echo "Usage: $0 [options]"
    echo ""
    echo "  --all             Full public setup (system + wifi + lte + vpn + ui)"
    echo "  --all --private   Full setup + private VPN keys"
    echo "  --setup_system    Hostname, timezone, LAN IP, IRQ"
    echo "  --setup_wifi      WiFi 2.4G + 5G"
    echo "  --setup_lte       LTE modem (Fibocom L860-GL)"
    echo "  --setup_vpn       VPN scripts + WG hotplug"
    echo "  --setup_ui        LCD UI (lcd_ui.uc + init.d)"
    echo "  --setup_private   WG/OpenVPN keys only"
    exit 0
fi

WANT_PRIVATE=0
for arg in "$@"; do
    [ "$arg" = "--private" ] && WANT_PRIVATE=1
done

for arg in "$@"; do
    case "$arg" in
        --all)
            setup_system
            setup_wifi
            setup_lte
            setup_vpn
            setup_ui
            [ "$WANT_PRIVATE" = "1" ] && setup_private
            ;;
        --setup_system)  setup_system ;;
        --setup_wifi)    setup_wifi ;;
        --setup_lte)     setup_lte ;;
        --setup_vpn)     setup_vpn ;;
        --setup_ui)      setup_ui ;;
        --setup_private) setup_private ;;
        --private) ;; # handled above
        *)
            echo "Unknown option: $arg"
            exit 1
            ;;
    esac
done

log "=== Setup complete ==="
