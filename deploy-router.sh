#!/bin/sh
#
# deploy-router.sh — Полная установка всех компонентов на роутер Almond 3S
#
# Этот скрипт заливает ВСЕ наработки проекта на роутер:
#   - Kernel module (lcd_drv.ko)
#   - Userspace утилиты (lcd_render, lcd_touch_read и др.)
#   - Lua UI (lcd_ui.lua, lcdlib.so)
#   - LuCI модуль VPN Switch
#   - Скрипты LTE (watchdog, band test, SMS)
#
# Предполагается что роутер доступен по SSH и работает OpenWrt.
# Предварительные бинарники лежат в out/ (собраны через build.sh).
#
# Использование:
#   На роутере:
#     wget https://raw.githubusercontent.com/isublimity/Securifi-Almond-3S/main/deploy-router.sh
#     sh deploy-router.sh
#
#   Или с компьютера:
#     scp deploy-router.sh root@192.168.11.1:/tmp/
#     ssh root@192.168.11.1 'sh /tmp/deploy-router.sh'
#

set -e

REPO="https://raw.githubusercontent.com/isublimity/Securifi-Almond-3S/main"

echo "========================================="
echo " Securifi Almond 3S — Полная установка"
echo "========================================="
echo ""

# === 1. Kernel module ===
echo "[1/6] Kernel module (lcd_drv.ko)..."
wget -qO /tmp/lcd_drv.ko "$REPO/out/lcd_drv.ko"
rmmod i2c_mt7621 2>/dev/null || true
rmmod lcd_drv 2>/dev/null || true
sleep 1
insmod /tmp/lcd_drv.ko
echo "  OK: lcd_drv.ko загружен, /dev/lcd доступен"

# === 2. Userspace утилиты ===
echo "[2/6] Userspace утилиты..."
wget -qO /tmp/lcd_render "$REPO/out/lcd_render"
wget -qO /tmp/lcd_touch_read "$REPO/out/lcd_touch_read"
wget -qO /tmp/lcd_touch_poll "$REPO/out/lcd_touch_poll"
wget -qO /tmp/pic_test "$REPO/out/pic_test"
chmod +x /tmp/lcd_render /tmp/lcd_touch_read /tmp/lcd_touch_poll /tmp/pic_test
echo "  OK: lcd_render, lcd_touch_read, lcd_touch_poll, pic_test"

# === 3. Lua модули ===
echo "[3/6] Lua UI + lcdlib.so..."
wget -qO /tmp/lcd_ui.lua "$REPO/modules/lcd_ui.lua"
wget -qO /usr/lib/lua/lcdlib.so "$REPO/out/lcdlib.so"
echo "  OK: lcd_ui.lua, lcdlib.so"

# === 4. LuCI VPN Switch ===
echo "[4/6] LuCI VPN Switch..."
mkdir -p /usr/lib/lua/luci/controller /usr/lib/lua/luci/view
wget -qO /usr/lib/lua/luci/controller/vpnswitch.lua "$REPO/luci-vpnswitch/vpnswitch.lua"
wget -qO /usr/lib/lua/luci/view/vpnswitch.htm "$REPO/luci-vpnswitch/vpnswitch.htm"
rm -rf /tmp/luci-*
echo "  OK: http://<router>/cgi-bin/luci/admin/services/vpnswitch"

# === 5. LTE скрипты ===
echo "[5/7] LTE скрипты..."
wget -qO /etc/init.d/lte-watchdog "$REPO/scripts/lte-watchdog.sh"
wget -qO /tmp/lte-band-test.sh "$REPO/scripts/lte-band-test.sh"
wget -qO /tmp/sms.sh "$REPO/scripts/sms.sh"
chmod +x /etc/init.d/lte-watchdog /tmp/lte-band-test.sh /tmp/sms.sh
/etc/init.d/lte-watchdog enable 2>/dev/null || true
echo "  OK: lte-watchdog, lte-band-test, sms"

# === 6. LCD init service ===
echo "[6/7] LCD init service..."
wget -qO /etc/init.d/lcd-init "$REPO/scripts/lcd-init.sh"
chmod +x /etc/init.d/lcd-init
/etc/init.d/lcd-init enable 2>/dev/null || true
echo "  OK: lcd-init (autostart at boot)"

# === 7. Запуск LCD стека ===
echo "[7/7] Запуск LCD..."
killall lcd_render lua 2>/dev/null || true
sleep 1
rmmod i2c_mt7621 2>/dev/null || true
/tmp/lcd_render &
sleep 1
lua /tmp/lcd_ui.lua > /tmp/lcd_ui.log 2>&1 &
sleep 3

echo ""
echo "========================================="
echo " Установка завершена!"
echo "========================================="
echo ""
echo "LCD:       $(pgrep lcd_render >/dev/null && echo 'работает' || echo 'ОШИБКА')"
echo "UI:        $(pgrep -f lcd_ui.lua >/dev/null && echo 'работает' || echo 'ОШИБКА')"
echo "Watchdog:  $(/etc/init.d/lte-watchdog status 2>/dev/null | head -1)"
echo "LuCI VPN:  http://$(uci get network.lan.ipaddr 2>/dev/null || echo '192.168.11.1')/cgi-bin/luci/admin/services/vpnswitch"
echo ""
echo "Управление:"
echo "  /tmp/lte-band-test.sh        — тест LTE бендов"
echo "  /tmp/sms.sh list             — SMS"
echo "  /etc/init.d/lte-watchdog status  — статус watchdog"
