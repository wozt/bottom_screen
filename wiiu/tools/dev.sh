#!/bin/bash
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WHAT="${1:-client}"

find_console() {
    seq 2 254 | sed 's|^|192.168.2.|' | xargs -P 64 -I{} sh -c '
        timeout 1 python3 -c "
import socket,sys
try:
    s=socket.create_connection((\"{}\",21),0.8)
    d=s.recv(64)
    s.close()
except Exception:
    sys.exit(1)
sys.exit(0 if b\"Hello\" in d else 1)" 2>/dev/null && echo {}' | head -1
}

CONSOLE="${WIIU_IP:-$(find_console)}"

if [ -z "$CONSOLE" ]; then
    echo "no console found (set WIIU_IP to skip discovery)" >&2
    exit 1
fi

echo "console: $CONSOLE"

if [ "$WHAT" = "logs" ]; then
    TARGET=""
else
    make -C "$HERE/.." || exit 1
    TARGET="$HERE/../bottomscreen_wiiu.rpx"
fi

udplogserver &
LOGGER=$!
trap 'kill $LOGGER 2>/dev/null' EXIT
sleep 1

if [ -n "$TARGET" ]; then
    echo "sending $(basename "$TARGET")"
    WIILOAD="tcp:$CONSOLE" wiiload "$TARGET" || exit 1
fi

echo "--- logs (ctrl-c to stop) ---"
wait "$LOGGER"
