#!/bin/bash
# ap-pair.sh <PIN 8 chiffres> — monte l'AP d'appairage, arme le WPS, puis
# bascule vers l'AP normal des que M8 part.
#
# La fenetre est etroite : la GamePad attend environ 4 s apres M8 avant de
# chercher l'AP normal, et un kill -9 laisse l'interface 5 s en transitoire.
# D'ou l'arret par SIGTERM avec attente de la sortie reelle.
set +e
PIN=${1:-00005678}
: "${DRC_IF:=wlxe0ad474070d8}"
: "${DRC_AP_MAC:=34:af:2c:be:ef:01}"
: "${DRC_HOSTAP:=/home/wozt/rtw88_TSF/drc-hostap}"
: "${DRC_RUN:=$HOME/.cache/drc-lab}"
: "${DRC_UUID:=22210203-0405-0607-0809-1a1b1c1d1e1f}"
mkdir -p "$DRC_RUN"
HH=$DRC_HOSTAP/hostapd/hostapd
HC=$DRC_HOSTAP/hostapd/hostapd_cli
CONF=$DRC_HOSTAP/conf

ip link set "$DRC_IF" down; sleep 1
iw dev "$DRC_IF" set type managed 2>/dev/null
ip link set "$DRC_IF" address "$DRC_AP_MAC"
ip link set "$DRC_IF" up; sleep 2

echo "[pair] AP d'appairage (_STA1), PIN $PIN"
$HH -dd "$CONF/local_pair2.conf" > "$DRC_RUN/pair.log" 2>&1 &
HPID=$!
sleep 5
$HC -p /var/run/hostapd -i "$DRC_IF" wps_pin "$DRC_UUID" "$PIN" 300 >/dev/null
echo "[pair] PIN arme — lance la synchro sur la GamePad"

for i in $(seq 1 2000); do
  grep -qa "Building Message M8\|WPS-SUCCESS" "$DRC_RUN/pair.log" && break
  sleep 0.2
done
echo "[pair] M8 detecte, bascule vers l'AP normal"
kill -TERM $HPID 2>/dev/null
for i in $(seq 1 30); do kill -0 $HPID 2>/dev/null || break; sleep 0.1; done

ip link set "$DRC_IF" up 2>/dev/null
$HH -dd "$CONF/local_normal2.conf" > "$DRC_RUN/ap.log" 2>&1 &
for i in $(seq 1 100); do grep -qa "AP-ENABLED" "$DRC_RUN/ap.log" && break; sleep 0.05; done
ip addr flush dev "$DRC_IF"
ip addr add 192.168.1.10/24 dev "$DRC_IF"
ip link set mtu "${DRC_MTU:-1800}" dev "$DRC_IF"
[ -n "$DRC_DNSMASQ_CONF" ] && /usr/sbin/dnsmasq -d -C "$DRC_DNSMASQ_CONF" > "$DRC_RUN/dns.log" 2>&1 &
echo "[pair] AP normal actif, attente DHCP..."
for i in $(seq 1 90); do
  grep -qa DHCPACK "$DRC_RUN/dns.log" 2>/dev/null && { echo "[pair] GamePad connectee"; exit 0; }
  sleep 1
done
echo "[pair] pas de DHCP — la GamePad n'a pas repris"
