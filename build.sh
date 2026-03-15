#!/bin/bash
#
# build.sh — Сборка модулей и утилит для Securifi Almond 3S
#
# Описание:
#   Скрипт собирает kernel module (lcd_drv.ko) на удалённом Linux-сервере
#   с установленным OpenWrt SDK, а userspace-утилиты компилирует локально
#   через zig cc (кросс-компиляция для MIPS).
#
# Требования:
#   - SSH доступ к серверу сборки (BUILD_SERVER)
#   - На сервере: клон openwrt_almond с собранным тулчейном (make toolchain/install)
#   - Локально: zig (https://ziglang.org) для кросс-компиляции userspace
#   - Роутер доступен по SSH (ROUTER)
#
# Использование:
#   ./build.sh              — собрать всё (kernel + userspace)
#   ./build.sh kernel       — только kernel module (lcd_drv.ko) на сервере
#   ./build.sh userspace    — только userspace утилиты (lcd_render, pic_test и др.)
#   ./build.sh deploy       — залить бинарники + скрипты на роутер
#   ./build.sh deploy-run   — залить и перезапустить kernel module
#
# Настройка:
#   Скопируйте build_config.sh.example в build_config.sh и укажите свои данные.
#   build_config.sh не коммитится в git (в .gitignore).
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MODULES_DIR="$SCRIPT_DIR/modules"
OUT_DIR="$SCRIPT_DIR/out"

# === Конфигурация ===
# Загружаем приватные настройки из build_config.sh
# Этот файл НЕ коммитится в git — содержит адреса серверов
if [ -f "$SCRIPT_DIR/build_config.sh" ]; then
    source "$SCRIPT_DIR/build_config.sh"
else
    echo "ОШИБКА: Не найден build_config.sh"
    echo ""
    echo "Создайте файл build_config.sh по образцу build_config.sh.example:"
    echo "  cp build_config.sh.example build_config.sh"
    echo "  nano build_config.sh"
    echo ""
    echo "Укажите в нём:"
    echo "  BUILD_SERVER — SSH адрес сервера сборки (user@host)"
    echo "  BUILD_DIR    — путь к openwrt_almond на сервере"
    echo "  ROUTER       — SSH адрес роутера (root@192.168.11.1)"
    exit 1
fi

# Пути в OpenWrt SDK (относительно BUILD_DIR)
# KDIR — путь к собранному ядру (нужен для компиляции .ko модулей)
KDIR="build_dir/target-mipsel_24kc_musl/linux-ramips_mt7621/linux-6.6.127"
# CROSS — префикс кросс-компилятора (gcc для mipsel)
CROSS="staging_dir/toolchain-mipsel_24kc_gcc-13.3.0_musl/bin/mipsel-openwrt-linux-musl-"

mkdir -p "$OUT_DIR"

# === Сборка kernel module ===
build_kernel() {
    echo "=== Сборка lcd_drv.ko на сервере ==="

    scp -O "$MODULES_DIR/lcd_drv.c" "$MODULES_DIR/splash_4pda.h" "$MODULES_DIR/Makefile" \
        "$BUILD_SERVER:$BUILD_DIR/package/lcd-gpio/src/"

    ssh "$BUILD_SERVER" "cd $BUILD_DIR && \
        make -C $KDIR \
            M=\"\$(pwd)/package/lcd-gpio/src\" \
            ARCH=mips \
            CROSS_COMPILE=\"\$(pwd)/$CROSS\" \
            modules 2>&1 | tail -5"

    scp -O "$BUILD_SERVER:$BUILD_DIR/package/lcd-gpio/src/lcd_drv.ko" "$OUT_DIR/"

    echo ">>> $OUT_DIR/lcd_drv.ko"
    ls -la "$OUT_DIR/lcd_drv.ko"
}

# === Сборка userspace утилит ===
build_userspace() {
    echo "=== Сборка userspace (zig cc) ==="

    zig cc -target mipsel-linux-musleabi -O2 -static \
        -o "$OUT_DIR/lcd_render" "$MODULES_DIR/lcd_render.c"
    echo ">>> lcd_render"

    zig cc -target mipsel-linux-musleabi -O2 -static \
        -o "$OUT_DIR/pic_test" "$MODULES_DIR/pic_test.c"
    echo ">>> pic_test"

    zig cc -target mipsel-linux-musleabi -O2 -static \
        -o "$OUT_DIR/lcd_touch_read" "$MODULES_DIR/lcd_touch_read.c"
    echo ">>> lcd_touch_read"

    zig cc -target mipsel-linux-musleabi -O2 -static \
        -o "$OUT_DIR/lcd_touch_poll" "$MODULES_DIR/lcd_touch_poll.c"
    echo ">>> lcd_touch_poll"

    ls -la "$OUT_DIR/lcd_render" "$OUT_DIR/pic_test" "$OUT_DIR/lcd_touch_read" "$OUT_DIR/lcd_touch_poll"
}

# === Деплой на роутер ===
deploy() {
    echo "=== Деплой на $ROUTER ==="

    scp -O "$OUT_DIR/lcd_drv.ko" "$OUT_DIR/lcd_render" "$OUT_DIR/pic_test" \
        "$OUT_DIR/lcd_touch_read" "$OUT_DIR/lcd_touch_poll" "$ROUTER:/tmp/"
    echo ">>> Бинарники загружены"

    ssh "$ROUTER" "mkdir -p /etc/lcd_scripts"
    scp -O "$SCRIPT_DIR/lcd_scripts/"*.lua "$ROUTER:/etc/lcd_scripts/"
    scp -O "$MODULES_DIR/lcd_ui.lua" "$ROUTER:/tmp/"
    echo ">>> Скрипты загружены"

    scp -O "$SCRIPT_DIR/luci-vpnswitch/vpnswitch.lua" "$ROUTER:/usr/lib/lua/luci/controller/"
    scp -O "$SCRIPT_DIR/luci-vpnswitch/vpnswitch.htm" "$ROUTER:/usr/lib/lua/luci/view/"
    ssh "$ROUTER" "rm -rf /tmp/luci-*"
    echo ">>> LuCI vpnswitch загружен"

    ssh "$ROUTER" "ls -la /tmp/lcd_drv.ko /tmp/lcd_render /tmp/lcd_touch_read /tmp/lcd_ui.lua"
}

# === Деплой + перезагрузка модуля ===
deploy_run() {
    deploy
    echo "=== Перезагрузка lcd_drv.ko ==="
    ssh "$ROUTER" "rmmod lcd_drv 2>/dev/null; insmod /tmp/lcd_drv.ko && echo 'OK: модуль загружен' && dmesg | grep lcd_drv | tail -10"
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
