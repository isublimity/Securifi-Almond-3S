#!/bin/bash
# mdcli.sh — memdebug client helper
# Usage:
#   ./mdcli.sh cmd "sm0"                  — send command
#   ./mdcli.sh upload local.bin /tmp/bin  — upload file to router
#   ./mdcli.sh shell                      — interactive nc session

HOST=${MDHOST:-10.10.10.254}
PORT=${MDPORT:-5555}

case "$1" in
    cmd)
        echo "$2" | nc -w3 "$HOST" "$PORT"
        ;;
    upload)
        if [ -z "$2" ] || [ -z "$3" ]; then
            echo "Usage: $0 upload <local_file> <remote_path>"
            exit 1
        fi
        SIZE=$(stat -f%z "$2" 2>/dev/null || stat -c%s "$2" 2>/dev/null)
        echo "Uploading $2 ($SIZE bytes) -> $3"
        {
            printf "upload %s %d\n" "$3" "$SIZE"
            sleep 0.5
            cat "$2"
            sleep 0.5
            printf "\nq\n"
        } | nc -w10 "$HOST" "$PORT"
        ;;
    shell)
        echo "Connecting to memdebug at $HOST:$PORT (Ctrl-C to exit)"
        nc "$HOST" "$PORT"
        ;;
    build-upload)
        # Build C file and upload: ./mdcli.sh build-upload source.c /tmp/prog
        if [ -z "$2" ] || [ -z "$3" ]; then
            echo "Usage: $0 build-upload <source.c> <remote_path>"
            exit 1
        fi
        TMPBIN=$(mktemp /tmp/mips_XXXXXX)
        echo "Building $2..."
        zig cc -target mipsel-linux-musleabi -Os -static -o "$TMPBIN" "$2" || exit 1
        SIZE=$(stat -f%z "$TMPBIN" 2>/dev/null || stat -c%s "$TMPBIN" 2>/dev/null)
        echo "Built $TMPBIN ($SIZE bytes), uploading to $3..."
        {
            printf "upload %s %d\n" "$3" "$SIZE"
            sleep 0.5
            cat "$TMPBIN"
            sleep 0.5
            printf "\nq\n"
        } | nc -w10 "$HOST" "$PORT"
        rm -f "$TMPBIN"
        ;;
    *)
        echo "memdebug client"
        echo "  $0 cmd <command>              — send command (sm0/poll/r/w/d)"
        echo "  $0 upload <file> <rpath>      — upload file to router"
        echo "  $0 build-upload <src.c> <rpath> — cross-compile + upload"
        echo "  $0 shell                      — interactive session"
        echo ""
        echo "Set MDHOST/MDPORT for custom address (default 10.10.10.254:5555)"
        ;;
esac
