#!/bin/bash
# ap-pair.sh <PIN 8 chiffres> — monte l'AP d'appairage, arme le WPS, puis
# bascule vers l'AP normal des que M8 part.
#
# La fenetre est etroite : la GamePad attend environ 4 s apres M8 avant de
# chercher l'AP normal, et un kill -9 laisse l'interface 5 s en transitoire.
# D'ou l'arret par SIGTERM avec attente de la sortie reelle.
set -u
PIN=${1:-00005678}
: "${DRC_IF:=wlxe0ad474070d8}"
: "${DRC_AP_MAC:=34:af:2c:be:ef:01}"
: "${DRC_HOSTAP:=/home/wozt/rtw88_TSF/drc-hostap}"
: "${DRC_RUN:=${HOME:-/tmp}/.cache/drc-lab}"
: "${DRC_UUID:=22210203-0405-0607-0809-1a1b1c1d1e1f}"
mkdir -p "$DRC_RUN"
HH=$DRC_HOSTAP/hostapd/hostapd
HC=$DRC_HOSTAP/hostapd/hostapd_cli
PAIR_CONF=${DRC_PAIR_CONF:-$DRC_HOSTAP/conf/local_pair2.conf}
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

fail()
{
  echo "[pair] erreur: $*" >&2
  exit 1
}

stop_saved_hostapd()
{
  [ -s "$DRC_RUN/hostapd.pid" ] || return 0
  read -r saved_pid < "$DRC_RUN/hostapd.pid"
  case "$saved_pid" in (*[!0-9]*|'') return 0;; esac
  process=$(basename "$(readlink "/proc/$saved_pid/exe" 2>/dev/null)")
  [ "$process" = hostapd ] && kill -TERM "$saved_pid" 2>/dev/null
}

HPID=
stop_pair_ap()
{
  [ -n "$HPID" ] && kill -TERM "$HPID" 2>/dev/null
}
trap stop_pair_ap EXIT

case "$PIN" in (*[!0-9]*|'') fail "le PIN doit contenir 8 chiffres";; esac
[ "${#PIN}" -eq 8 ] || fail "le PIN doit contenir 8 chiffres"
[ -x "$HH" ] || fail "hostapd Wii U introuvable: $HH"
[ -x "$HC" ] || fail "hostapd_cli introuvable: $HC"
[ -f "$PAIR_CONF" ] || fail "configuration d'appairage introuvable: $PAIR_CONF"
ip link show "$DRC_IF" >/dev/null 2>&1 || fail "interface Wi-Fi introuvable: $DRC_IF"

# A normal AP may already be running from an earlier session.  Stop only the
# configured GamePad interface before replacing it with the temporary _STA1 AP.
if [ -S "/var/run/hostapd/$DRC_IF" ]; then
  "$HC" -p /var/run/hostapd -i "$DRC_IF" terminate >/dev/null 2>&1
fi
stop_saved_hostapd
for _ in $(seq 1 30); do
  [ ! -S "/var/run/hostapd/$DRC_IF" ] && break
  sleep 0.1
done

command -v nmcli >/dev/null 2>&1 && nmcli device set "$DRC_IF" managed no >/dev/null 2>&1
ip link set "$DRC_IF" down || fail "impossible de désactiver $DRC_IF"
sleep 1
iw dev "$DRC_IF" set type managed 2>/dev/null
ip link set "$DRC_IF" address "$DRC_AP_MAC" || fail "impossible d'appliquer l'adresse AP"
ip link set "$DRC_IF" up || fail "impossible d'activer $DRC_IF"
sleep 2

echo "[pair] AP d'appairage (_STA1), PIN $PIN"
"$HH" -dd "$PAIR_CONF" > "$DRC_RUN/pair.log" 2>&1 &
HPID=$!
echo "$HPID" > "$DRC_RUN/hostapd.pid"
ready=0
for _ in $(seq 1 100); do
  if grep -qa "AP-ENABLED" "$DRC_RUN/pair.log"; then ready=1; break; fi
  kill -0 "$HPID" 2>/dev/null || break
  sleep 0.05
done
[ "$ready" -eq 1 ] || fail "hostapd n'a pas activé l'AP d'appairage"
"$HC" -p /var/run/hostapd -i "$DRC_IF" wps_pin "$DRC_UUID" "$PIN" 300 >/dev/null || \
  fail "impossible d'armer le PIN WPS"
echo "[pair] PIN arme — lance la synchro sur la GamePad"

paired=0
for i in $(seq 1 2000); do
  if grep -qa "Building Message M8\|WPS-SUCCESS" "$DRC_RUN/pair.log"; then paired=1; break; fi
  kill -0 "$HPID" 2>/dev/null || break
  sleep 0.2
done
[ "$paired" -eq 1 ] || fail "appairage expiré avant le message WPS M8"
echo "[pair] M8 detecte, bascule vers l'AP normal"
kill -TERM "$HPID" 2>/dev/null
for _ in $(seq 1 30); do kill -0 "$HPID" 2>/dev/null || break; sleep 0.1; done
HPID=
trap - EXIT

"$SCRIPT_DIR/ap-normal.sh" || exit $?
echo "[pair] AP normal actif, attente du GamePad..."
for _ in $(seq 1 90); do
  if "$HC" -p /var/run/hostapd -i "$DRC_IF" all_sta 2>/dev/null | grep -q '\[AUTHORIZED\]'; then
    echo "[pair] GamePad connectee"
    exit 0
  fi
  sleep 1
done
fail "le GamePad ne s'est pas reconnecté à l'AP normal"
