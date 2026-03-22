#!/bin/bash
#
# build.sh — Сборка модулей и утилит для Securifi Almond 3S
#
# Компоненты:
#   lcd_drv.ko      — kernel module (LCD + touch, NO PIC)
#   lcd_render      — JSON socket renderer
#   touch_poll      — touch polling daemon
#   data_collector  — LTE/WiFi/VPN stats collector
#   lcd_ui.uc       — UI скрипт (ucode)
#
# Использование:
#   ./build.sh              — собрать всё (kernel + userspace)
#   ./build.sh kernel       — только kernel module
#   ./build.sh userspace    — только userspace утилиты
#   ./build.sh deploy       — залить на роутер
#   ./build.sh firmware     — полная сборка прошивки
#
# Настройка: cp build_config.sh.example build_config.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MODULES_DIR="$SCRIPT_DIR/modules"
OUT_DIR="$SCRIPT_DIR/out"

if [ -f "$SCRIPT_DIR/build_config.sh" ]; then
    source "$SCRIPT_DIR/build_config.sh"
else
    echo "ОШИБКА: cp build_config.sh.example build_config.sh"
    exit 1
fi

# fildunsky_openwrt: kernel 6.12.74, GCC 14.3
FILD_DIR="${FILD_DIR:-/mnt/sata/var/openwrt/fildunsky_openwrt}"
KDIR="$FILD_DIR/build_dir/target-mipsel_24kc_musl/linux-ramips_mt7621/linux-6.12.74"
CROSS="$FILD_DIR/staging_dir/toolchain-mipsel_24kc_gcc-14.3.0_musl/bin/mipsel-openwrt-linux-musl-"

mkdir -p "$OUT_DIR"

# === Kernel module ===
build_kernel() {
    echo "=== Сборка lcd_drv.ko (GCC 14.3, kernel 6.12.74) ==="
    local SRC="$FILD_DIR/package/lcd-gpio/src"

    scp -O "$MODULES_DIR/lcd_drv.c" "$MODULES_DIR/splash_4pda.h" \
           "$MODULES_DIR/pic_calib.h" "$MODULES_DIR/Makefile" \
        "$BUILD_SERVER:$SRC/"

    ssh "$BUILD_SERVER" "cd $FILD_DIR && \
        make -C $KDIR M=$SRC ARCH=mips CROSS_COMPILE=${CROSS} modules 2>&1 | tail -5"

    scp -O "$BUILD_SERVER:$SRC/lcd_drv.ko" "$OUT_DIR/"
    echo ">>> $OUT_DIR/lcd_drv.ko ($(wc -c < "$OUT_DIR/lcd_drv.ko") bytes)"
}

# === Userspace ===
build_userspace() {
    echo "=== Сборка userspace (zig cc, mipsel-musleabi) ==="
    local ZIG="zig cc -target mipsel-linux-musleabi -Os -static"

    $ZIG -o "$OUT_DIR/lcd_render" "$MODULES_DIR/lcd_render.c"
    echo ">>> lcd_render"

    $ZIG -o "$OUT_DIR/touch_poll" "$MODULES_DIR/touch_poll.c"
    echo ">>> touch_poll"

    $ZIG -o "$OUT_DIR/data_collector" "$MODULES_DIR/data_collector.c"
    echo ">>> data_collector"

    ls -la "$OUT_DIR/lcd_render" "$OUT_DIR/touch_poll" "$OUT_DIR/data_collector"
}

# === Deploy ===
deploy() {
    echo "=== Деплой на $ROUTER ==="

    ssh "$ROUTER" '/etc/init.d/lcd_ui stop 2>/dev/null; killall -9 ucode data_collector touch_poll 2>/dev/null; sleep 2' || true

    scp -O "$OUT_DIR/lcd_render" "$OUT_DIR/touch_poll" "$OUT_DIR/data_collector" "$ROUTER:/usr/bin/"
    scp -O "$MODULES_DIR/lcd_ui.uc" "$ROUTER:/usr/bin/"
    scp -O "$MODULES_DIR/settings.lua" "$ROUTER:/etc/lcd/settings.lua" 2>/dev/null || {
        ssh "$ROUTER" "mkdir -p /etc/lcd"
        scp -O "$MODULES_DIR/settings.lua" "$ROUTER:/etc/lcd/settings.lua"
    }
    scp -O "$SCRIPT_DIR/scripts/lcd/"*.sh "$SCRIPT_DIR/scripts/lcd/"*.init "$ROUTER:/etc/lcd/scripts/" 2>/dev/null || true

    if [ -f "$OUT_DIR/lcd_drv.ko" ]; then
        scp -O "$OUT_DIR/lcd_drv.ko" "$ROUTER:/lib/modules/\$(ssh $ROUTER 'uname -r')/lcd_drv.ko"
    fi

    ssh "$ROUTER" "chmod +x /usr/bin/lcd_render /usr/bin/touch_poll /usr/bin/data_collector /usr/bin/lcd_ui.uc"
    ssh "$ROUTER" "/etc/init.d/lcd_ui start 2>/dev/null"

    echo ">>> Deploy OK"
}

# === Full firmware ===
build_firmware() {
    echo "=== Полная сборка прошивки ==="
    ssh "$BUILD_SERVER" "cd $FILD_DIR && make -j\$(nproc) 2>&1 | tail -5"
    scp -O "$BUILD_SERVER:$FILD_DIR/bin/targets/ramips/mt7621/openwrt-*-sysupgrade.bin" "$OUT_DIR/"
    echo ">>> Firmware: $(ls -lh "$OUT_DIR/openwrt-"*sysupgrade.bin 2>/dev/null)"
}

# === Main ===
case "${1:-all}" in
    kernel)    build_kernel ;;
    userspace) build_userspace ;;
    deploy)    deploy ;;
    firmware)  build_firmware ;;
    all)
        build_kernel
        build_userspace
        echo ""
        echo "=== Готово ==="
        ls -la "$OUT_DIR/"
        ;;
    *)
        echo "Использование: $0 {all|kernel|userspace|deploy|firmware}"
        exit 1
        ;;
esac
