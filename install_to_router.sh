#!/bin/sh
#
# install_to_router.sh — Установка на роутер после прошивки
#
# Запуск с Mac:
#   ./install_to_router.sh              — полная настройка + приватные ключи
#   ./install_to_router.sh --public     — без приватных ключей
#

ROUTER="root@192.168.11.1"
DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== Uploading to $ROUTER ==="
scp -O "$DIR/first_setup.sh" "$ROUTER:/tmp/"

if [ "$1" = "--public" ]; then
    echo "=== Running setup (public only) ==="
    ssh "$ROUTER" 'sh /tmp/first_setup.sh --all'
else
    if [ -f "$DIR/private/secrets.sh" ]; then
        scp -O "$DIR/private/secrets.sh" "$ROUTER:/tmp/"
        echo "=== Running setup (full + private) ==="
        ssh "$ROUTER" 'sh /tmp/first_setup.sh --all --private'
    else
        echo "WARNING: private/secrets.sh not found, running public only"
        ssh "$ROUTER" 'sh /tmp/first_setup.sh --all'
    fi
fi
