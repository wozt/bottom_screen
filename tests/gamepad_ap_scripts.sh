#!/bin/sh
# Exercise both radio modes without touching a real interface or requiring root.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=$(mktemp -d)
cleanup()
{
    if [ -f "$TMP/pids" ]; then
        while read -r pid; do kill "$pid" 2>/dev/null || true; done < "$TMP/pids"
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

mkdir -p "$TMP/bin" "$TMP/hostapd/hostapd" "$TMP/hostapd/conf" "$TMP/run"

for command in ip iw nmcli; do
    printf '%s\n' '#!/bin/sh' 'exit 0' > "$TMP/bin/$command"
    chmod +x "$TMP/bin/$command"
done
printf '%s\n' '#!/bin/sh' 'exec /bin/sleep 0.01' > "$TMP/bin/sleep"
chmod +x "$TMP/bin/sleep"

cat > "$TMP/hostapd/hostapd/hostapd" <<'EOF'
#!/bin/sh
echo $$ >> "$AP_TEST_ROOT/pids"
trap 'exit 0' TERM INT
echo AP-ENABLED
case "$*" in (*pair.conf*) echo 'Building Message M8';; esac
while :; do sleep 1; done
EOF
cat > "$TMP/hostapd/hostapd/hostapd_cli" <<'EOF'
#!/bin/sh
case " $* " in
    *' wps_pin '*) echo "$*" >> "$AP_TEST_ROOT/wps";;
    *' all_sta '*) printf '%s\n' '34:af:2c:00:00:01' 'flags=[AUTH][ASSOC][AUTHORIZED]';;
esac
exit 0
EOF
chmod +x "$TMP/hostapd/hostapd/hostapd" "$TMP/hostapd/hostapd/hostapd_cli"
: > "$TMP/hostapd/conf/pair.conf"
: > "$TMP/hostapd/conf/normal.conf"

export PATH="$TMP/bin:$PATH"
export AP_TEST_ROOT="$TMP"
export DRC_IF=test-wiiu0
export DRC_HOSTAP="$TMP/hostapd"
export DRC_RUN="$TMP/run"
export DRC_PAIR_CONF="$TMP/hostapd/conf/pair.conf"
export DRC_NORMAL_CONF="$TMP/hostapd/conf/normal.conf"

normal=$(/bin/bash "$ROOT/gamepad/tools/ap-normal.sh")
printf '%s' "$normal" | grep -q 'AP normal actif sur test-wiiu0'

pair=$(/bin/bash "$ROOT/gamepad/tools/ap-pair.sh" 01235678)
printf '%s' "$pair" | grep -q 'PIN arme'
printf '%s' "$pair" | grep -q 'M8 detecte'
printf '%s' "$pair" | grep -q 'GamePad connectee'
grep -q '01235678' "$TMP/wps"

stopped=$(/bin/bash "$ROOT/gamepad/tools/ap-stop.sh")
printf '%s' "$stopped" | grep -q 'AP arrêté'
[ ! -e "$TMP/run/hostapd.pid" ]

if /bin/bash "$ROOT/gamepad/tools/ap-pair.sh" bad-pin > /dev/null 2>&1; then
    echo 'invalid pairing PIN was accepted' >&2
    exit 1
fi

echo 'PASS: fresh pairing, normal reconnect and AP stop use distinct paths'
