#!/bin/bash
# Stop only the access point managed by the Bottom Screen GamePad tools.
set -u

: "${DRC_IF:=wlxe0ad474070d8}"
: "${DRC_HOSTAP:=/home/wozt/rtw88_TSF/drc-hostap}"
: "${DRC_RUN:=${HOME:-/tmp}/.cache/drc-lab}"

HC="$DRC_HOSTAP/hostapd/hostapd_cli"

stop_saved_process()
{
    pid_file=$1
    expected=$2
    [ -s "$pid_file" ] || return 0
    read -r saved_pid < "$pid_file"
    case "$saved_pid" in (*[!0-9]*|'') return 0;; esac
    process=$(basename "$(readlink "/proc/$saved_pid/exe" 2>/dev/null)")
    [ "$process" = "$expected" ] && kill -TERM "$saved_pid" 2>/dev/null
}

echo "[ap] arrêt de l'AP GamePad"
if [ -x "$HC" ] && [ -S "/var/run/hostapd/$DRC_IF" ]; then
    "$HC" -p /var/run/hostapd -i "$DRC_IF" terminate >/dev/null 2>&1
fi
stop_saved_process "$DRC_RUN/hostapd.pid" hostapd
stop_saved_process "$DRC_RUN/dnsmasq.pid" dnsmasq

for _ in $(seq 1 30); do
    [ ! -S "/var/run/hostapd/$DRC_IF" ] && break
    sleep 0.1
done
if [ -S "/var/run/hostapd/$DRC_IF" ]; then
    echo "[ap] erreur: hostapd n'a pas arrêté l'AP sur $DRC_IF" >&2
    exit 1
fi
rm -f "$DRC_RUN/hostapd.pid" "$DRC_RUN/dnsmasq.pid"

if ip link show "$DRC_IF" >/dev/null 2>&1; then
    ip addr flush dev "$DRC_IF" >/dev/null 2>&1
    ip link set "$DRC_IF" down >/dev/null 2>&1
    if command -v nmcli >/dev/null 2>&1; then
        nmcli device set "$DRC_IF" managed yes >/dev/null 2>&1
    fi
fi

echo "[ap] AP arrêté"
