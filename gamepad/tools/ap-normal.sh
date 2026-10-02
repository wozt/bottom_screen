#!/bin/bash
# Start the normal Wii U GamePad AP used after the pad has been paired.
#
# The launcher runs this script through pkexec.  Every setting can still be
# overridden for another adapter/install, while the defaults match the local
# drc-hostap setup used by the rest of the GamePad tools.
set -u

: "${DRC_IF:=wlxe0ad474070d8}"
: "${DRC_AP_MAC:=34:af:2c:be:ef:01}"
: "${DRC_HOSTAP:=/home/wozt/rtw88_TSF/drc-hostap}"
: "${DRC_RUN:=${HOME:-/tmp}/.cache/drc-lab}"
: "${DRC_MTU:=1800}"
: "${DRC_DNSMASQ_CONF:=}"

HH="$DRC_HOSTAP/hostapd/hostapd"
HC="$DRC_HOSTAP/hostapd/hostapd_cli"
NORMAL_CONF=${DRC_NORMAL_CONF:-$DRC_HOSTAP/conf/local_normal2.conf}
mkdir -p "$DRC_RUN"
hostapd_pid=

fail()
{
    [ -n "$hostapd_pid" ] && kill -TERM "$hostapd_pid" 2>/dev/null
    echo "[ap] erreur: $*" >&2
    exit 1
}

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

[ -x "$HH" ] || fail "hostapd Wii U introuvable: $HH"
[ -x "$HC" ] || fail "hostapd_cli introuvable: $HC"
[ -f "$NORMAL_CONF" ] || fail "configuration AP normale introuvable: $NORMAL_CONF"
ip link show "$DRC_IF" >/dev/null 2>&1 || fail "interface Wi-Fi introuvable: $DRC_IF"

# Stop only the AP on the GamePad interface.  The pid file also covers a
# half-started instance whose control socket was never created.
if [ -S "/var/run/hostapd/$DRC_IF" ]; then
    "$HC" -p /var/run/hostapd -i "$DRC_IF" terminate >/dev/null 2>&1
fi
stop_saved_process "$DRC_RUN/hostapd.pid" hostapd
stop_saved_process "$DRC_RUN/dnsmasq.pid" dnsmasq
for _ in $(seq 1 30); do
    [ ! -S "/var/run/hostapd/$DRC_IF" ] && break
    sleep 0.1
done

command -v nmcli >/dev/null 2>&1 && nmcli device set "$DRC_IF" managed no >/dev/null 2>&1
ip link set "$DRC_IF" down || fail "impossible de désactiver $DRC_IF"
sleep 1
iw dev "$DRC_IF" set type managed >/dev/null 2>&1
ip link set "$DRC_IF" address "$DRC_AP_MAC" || fail "impossible d'appliquer l'adresse AP"
ip link set "$DRC_IF" up || fail "impossible d'activer $DRC_IF"
sleep 2

echo "[ap] démarrage de l'AP normal"
"$HH" -dd "$NORMAL_CONF" > "$DRC_RUN/ap.log" 2>&1 &
hostapd_pid=$!
echo "$hostapd_pid" > "$DRC_RUN/hostapd.pid"

ready=0
for _ in $(seq 1 100); do
    if grep -qa "AP-ENABLED" "$DRC_RUN/ap.log"; then ready=1; break; fi
    kill -0 "$hostapd_pid" 2>/dev/null || break
    sleep 0.05
done
if [ "$ready" -ne 1 ]; then
    tail -20 "$DRC_RUN/ap.log" >&2
    kill -TERM "$hostapd_pid" 2>/dev/null
    fail "hostapd n'a pas activé l'AP"
fi

ip addr flush dev "$DRC_IF" || fail "impossible de réinitialiser l'adresse IP"
ip addr add 192.168.1.10/24 dev "$DRC_IF" || fail "impossible d'appliquer l'adresse IP"
ip link set mtu "$DRC_MTU" dev "$DRC_IF" || fail "impossible d'appliquer le MTU"

if [ -n "$DRC_DNSMASQ_CONF" ]; then
    [ -f "$DRC_DNSMASQ_CONF" ] || fail "configuration dnsmasq introuvable: $DRC_DNSMASQ_CONF"
    /usr/sbin/dnsmasq -d -C "$DRC_DNSMASQ_CONF" > "$DRC_RUN/dns.log" 2>&1 &
    echo $! > "$DRC_RUN/dnsmasq.pid"
fi

echo "[ap] AP normal actif sur $DRC_IF"
