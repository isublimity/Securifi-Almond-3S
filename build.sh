#!/bin/bash
#
# build.sh — Сборка модулей и утилит для Securifi Almond 3S
#
# Компоненты LCD UI:
#   lcd_drv.ko      — kernel module (дисплей + тач + PIC battery)
#   lcd_render      — userspace renderer (JSON через unix socket)
#   touch_poll      — touch polling daemon (latch mode)
#   data_collector  — фоновый сбор данных (LTE/WiFi/VPN/system)
#   lcd_ui.uc       — UI скрипт (ucode: uloop + ubus + uci)
#   settings.lua    — конфигурация
#
# Требования:
#   - SSH доступ к серверу сборки (BUILD_SERVER)
#   - На сервере: клон openwrt_almond с собранным тулчейном
#   - Локально: zig (https://ziglang.org) для кросс-компиляции userspace
#   - Роутер доступен по SSH (ROUTER)
#
# Использование:
#   ./build.sh              — собрать всё (kernel + userspace)
#   ./build.sh kernel       — только kernel module (lcd_drv.ko) на сервере
#   ./build.sh userspace    — только userspace утилиты
#   ./build.sh deploy       — залить на роутер
#   ./build.sh deploy-run   — залить и запустить
#
# Настройка:
#   cp build_config.sh.example build_config.sh && nano build_config.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MODULES_DIR="$SCRIPT_DIR/modules"
OUT_DIR="$SCRIPT_DIR/out"

# === Конфигурация ===
if [ -f "$SCRIPT_DIR/build_config.sh" ]; then
    source "$SCRIPT_DIR/build_config.sh"
else
    echo "ОШИБКА: Не найден build_config.sh"
    echo "  cp build_config.sh.example build_config.sh"
    exit 1
fi

KDIR="build_dir/target-mipsel_24kc_musl/linux-ramips_mt7621/linux-6.6.127"
CROSS="staging_dir/toolchain-mipsel_24kc_gcc-13.3.0_musl/bin/mipsel-openwrt-linux-musl-"

mkdir -p "$OUT_DIR"

# === Kernel module ===
build_kernel() {
    echo "=== Сборка lcd_drv.ko на сервере ==="

    scp -O "$MODULES_DIR/lcd_drv.c" "$MODULES_DIR/splash_4pda.h" \
           "$MODULES_DIR/pic_calib.h" "$MODULES_DIR/Makefile" \
        "$BUILD_SERVER:$BUILD_DIR/package/lcd-gpio/src/"

    ssh "$BUILD_SERVER" "cd $BUILD_DIR && \
        make -C $KDIR \
            M=\"\$(pwd)/package/lcd-gpio/src\" \
            ARCH=mips \
            CROSS_COMPILE=\"\$(pwd)/$CROSS\" \
            modules 2>&1 | tail -5"

    scp -O "$BUILD_SERVER:$BUILD_DIR/package/lcd-gpio/src/lcd_drv.ko" "$OUT_DIR/"
    echo ">>> $OUT_DIR/lcd_drv.ko"
}

# === Userspace ===
build_userspace() {
    echo "=== Сборка userspace (zig cc) ==="
    local ZIG="zig cc -target mipsel-linux-musleabi -Os -static"

    $ZIG -o "$OUT_DIR/lcd_render" "$MODULES_DIR/lcd_render.c"
    echo ">>> lcd_render"

    $ZIG -o "$OUT_DIR/touch_poll" "$MODULES_DIR/touch_poll.c"
    echo ">>> touch_poll"

    $ZIG -o "$OUT_DIR/data_collector" "$MODULES_DIR/data_collector.c"
    echo ">>> data_collector"

    ls -la "$OUT_DIR/lcd_render" "$OUT_DIR/touch_poll" "$OUT_DIR/data_collector"
}

# === Деплой на роутер ===
deploy() {
    echo "=== Деплой на $ROUTER ==="

    # Остановить процессы
    ssh "$ROUTER" 'killall touch_poll 2>/dev/null; kill -9 $(pidof ucode) 2>/dev/null; killall data_collector lcd_render 2>/dev/null; sleep 1' || true

    # Бинарники
    scp -O "$OUT_DIR/lcd_render" "$OUT_DIR/touch_poll" "$OUT_DIR/data_collector" \
        "$ROUTER:/usr/bin/"
    echo ">>> Бинарники загружены"

    # Скрипты
    scp -O "$MODULES_DIR/lcd_ui.uc" "$ROUTER:/usr/bin/"
    scp -O "$MODULES_DIR/settings.lua" "$ROUTER:/etc/lcd/settings.lua" 2>/dev/null || \
        ssh "$ROUTER" "mkdir -p /etc/lcd" && \
        scp -O "$MODULES_DIR/settings.lua" "$ROUTER:/etc/lcd/settings.lua"
    echo ">>> Скрипты загружены"

    # Kernel module (если есть)
    if [ -f "$OUT_DIR/lcd_drv.ko" ]; then
        scp -O "$OUT_DIR/lcd_drv.ko" "$ROUTER:/tmp/"
        echo ">>> lcd_drv.ko загружен в /tmp/"
    fi

    ssh "$ROUTER" "chmod +x /usr/bin/lcd_render /usr/bin/touch_poll /usr/bin/data_collector /usr/bin/lcd_ui.uc"
    echo ">>> Деплой завершён"
}

# === Деплой + запуск ===
deploy_run() {
    deploy
    echo "=== Запуск LCD UI ==="
    ssh "$ROUTER" '
        # Kernel module
        if [ -f /tmp/lcd_drv.ko ]; then
            rmmod lcd_drv 2>/dev/null
            insmod /tmp/lcd_drv.ko
            cp /tmp/lcd_drv.ko /lib/modules/$(uname -r)/lcd_drv.ko
        fi
        sleep 1

        # Запуск демонов
        lcd_render &
        sleep 1
        data_collector &
        touch_poll daemon
        sleep 1

        # UI
        ucode /usr/bin/lcd_ui.uc > /tmp/lcd_ui.log 2>&1 &
        sleep 3

        echo "=== Статус ==="
        ps | grep -E "lcd_render|data_collector|touch_poll|ucode" | grep -v grep
        cat /tmp/lcd_ui.log
    '
}

# === Точка входа ===
case "${1:-all}" in
    kernel)     build_kernel ;;
    userspace)  build_userspace ;;
    deploy)     deploy ;;
    deploy-run) deploy_run ;;
    all)
        build_kernel
        build_userspace
        echo ""
        echo "=== Сборка завершена ==="
        ls -la "$OUT_DIR/"
        ;;
    *)
        echo "Использование: $0 {all|kernel|userspace|deploy|deploy-run}"
        exit 1
        ;;
esac
