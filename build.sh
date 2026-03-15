#!/bin/bash
#
# build.sh — Сборка модулей и утилит для Securifi Almond 3S
#
# Использование:
#   ./build.sh              — собрать всё (kernel module на сервере + userspace локально)
#   ./build.sh kernel       — только kernel module (lcd_drv.ko)
#   ./build.sh userspace    — только userspace (lcd_render, pic_test)
#   ./build.sh deploy       — залить на роутер
#   ./build.sh deploy-run   — залить и перезапустить модуль
#

set -e

# === Конфигурация ===
ROUTER="root@192.168.11.1"
BUILD_DIR="/mnt/sata/var/openwrt/fork/openwrt_almond"
KDIR="build_dir/target-mipsel_24kc_musl/linux-ramips_mt7621/linux-6.6.127"
CROSS="staging_dir/toolchain-mipsel_24kc_gcc-13.3.0_musl/bin/mipsel-openwrt-linux-musl-"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MODULES_DIR="$SCRIPT_DIR/modules"
OUT_DIR="$SCRIPT_DIR/out"

mkdir -p "$OUT_DIR"

# === Функции ===

build_kernel() {
    echo "=== Building lcd_drv.ko on server ==="

    # Загрузить исходники на сервер
    scp -O "$MODULES_DIR/lcd_drv.c" "$MODULES_DIR/Makefile" \
        "$BUILD_SERVER:$BUILD_DIR/package/lcd-gpio/src/"

    # Собрать
    ssh "$BUILD_SERVER" "cd $BUILD_DIR && \
        make -C $KDIR \
            M=\"\$(pwd)/package/lcd-gpio/src\" \
            ARCH=mips \
            CROSS_COMPILE=\"\$(pwd)/$CROSS\" \
            modules 2>&1 | tail -5"

    # Скачать результат
    scp -O "$BUILD_SERVER:$BUILD_DIR/package/lcd-gpio/src/lcd_drv.ko" "$OUT_DIR/"

    echo ">>> $OUT_DIR/lcd_drv.ko"
    ls -la "$OUT_DIR/lcd_drv.ko"
}

build_userspace() {
    echo "=== Building userspace (zig cc) ==="

    zig cc -target mipsel-linux-musleabi -O2 -static \
        -o "$OUT_DIR/lcd_render" "$MODULES_DIR/lcd_render.c"
    echo ">>> $OUT_DIR/lcd_render"

    zig cc -target mipsel-linux-musleabi -O2 -static \
        -o "$OUT_DIR/pic_test" "$MODULES_DIR/pic_test.c"
    echo ">>> $OUT_DIR/pic_test"

    zig cc -target mipsel-linux-musleabi -O2 -static \
        -o "$OUT_DIR/lcd_touch_read" "$MODULES_DIR/lcd_touch_read.c"
    echo ">>> $OUT_DIR/lcd_touch_read"

    ls -la "$OUT_DIR/lcd_render" "$OUT_DIR/pic_test" "$OUT_DIR/lcd_touch_read"
}

deploy() {
    echo "=== Deploying to $ROUTER ==="
    scp -O "$OUT_DIR/lcd_drv.ko" "$OUT_DIR/lcd_render" "$OUT_DIR/pic_test" \
        "$OUT_DIR/lcd_touch_read" "$ROUTER:/tmp/"
    echo ">>> Binaries uploaded"

    # Deploy LCD scripts and UI
    ssh "$ROUTER" "mkdir -p /etc/lcd_scripts"
    scp -O "$SCRIPT_DIR/lcd_scripts/"*.lua "$ROUTER:/etc/lcd_scripts/"
    scp -O "$MODULES_DIR/lcd_ui.lua" "$ROUTER:/tmp/"
    echo ">>> Scripts uploaded"

    # Deploy LuCI vpnswitch
    scp -O "$SCRIPT_DIR/luci-vpnswitch/vpnswitch.lua" "$ROUTER:/usr/lib/lua/luci/controller/"
    scp -O "$SCRIPT_DIR/luci-vpnswitch/vpnswitch.htm" "$ROUTER:/usr/lib/lua/luci/view/"
    ssh "$ROUTER" "rm -rf /tmp/luci-*"
    echo ">>> LuCI vpnswitch deployed"

    ssh "$ROUTER" "ls -la /tmp/lcd_drv.ko /tmp/lcd_render /tmp/lcd_touch_read /tmp/lcd_ui.lua"
}

deploy_run() {
    deploy
    echo "=== Reloading lcd_drv.ko ==="
    ssh "$ROUTER" "rmmod lcd_drv 2>/dev/null; insmod /tmp/lcd_drv.ko && echo 'OK: module loaded' && dmesg | grep lcd_drv | tail -10"
}

# === Main ===

case "${1:-all}" in
    kernel)
        build_kernel
        ;;
    userspace)
        build_userspace
        ;;
    deploy)
        deploy
        ;;
    deploy-run)
        deploy_run
        ;;
    all)
        build_kernel
        build_userspace
        echo ""
        echo "=== Build complete ==="
        ls -la "$OUT_DIR/"
        ;;
    *)
        echo "Usage: $0 {all|kernel|userspace|deploy|deploy-run}"
        exit 1
        ;;
esac
