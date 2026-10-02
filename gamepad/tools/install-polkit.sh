#!/bin/bash
# Install a tightly scoped, passwordless Polkit action for GamePad AP control.
# The one installation itself must be run as root.
set -eu

SOURCE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
INSTALL_DIR=/usr/local/libexec/bottom-screen-gamepad
CONFIG_DIR=/etc/bottom-screen-gamepad
RULE_FILE=/etc/polkit-1/rules.d/49-bottom-screen-gamepad.rules
CONTROL="$INSTALL_DIR/gamepad-ap-control"

if [ "${1:-}" = "--uninstall" ]; then
    [ "$(id -u)" -eq 0 ] || { echo "Run this uninstall with sudo." >&2; exit 1; }
    rm -f "$RULE_FILE"
    rm -rf "$INSTALL_DIR" "$CONFIG_DIR"
    echo "Bottom Screen GamePad Polkit helper removed."
    exit 0
fi

[ "$#" -eq 0 ] || { echo "usage: sudo $0 [--uninstall]" >&2; exit 2; }
[ "$(id -u)" -eq 0 ] || {
    echo "This is a one-time system installation. Run:" >&2
    echo "  sudo $0" >&2
    exit 1
}

TARGET_USER=${SUDO_USER:-}
if [ -z "$TARGET_USER" ] || [ "$TARGET_USER" = root ]; then
    TARGET_USER=$(logname 2>/dev/null || true)
fi
case "$TARGET_USER" in (*[!A-Za-z0-9_.-]*|'')
    echo "Cannot determine the desktop user. Run with sudo from that account." >&2
    exit 1
    ;;
esac
TARGET_HOME=$(getent passwd "$TARGET_USER" | cut -d: -f6)
[ -n "$TARGET_HOME" ] || { echo "Unknown user: $TARGET_USER" >&2; exit 1; }

HOSTAP_SOURCE=${DRC_HOSTAP:-$TARGET_HOME/rtw88_TSF/drc-hostap}
PAIR_SOURCE=${DRC_PAIR_CONF:-$HOSTAP_SOURCE/conf/local_pair2.conf}
NORMAL_SOURCE=${DRC_NORMAL_CONF:-$HOSTAP_SOURCE/conf/local_normal2.conf}
DNS_SOURCE=${DRC_DNSMASQ_CONF:-}
IFACE=${DRC_IF:-wlxe0ad474070d8}
AP_MAC=${DRC_AP_MAC:-34:af:2c:be:ef:01}
MTU=${DRC_MTU:-1800}
UUID=${DRC_UUID:-22210203-0405-0607-0809-1a1b1c1d1e1f}

case "$IFACE" in (*[!A-Za-z0-9_.:-]*|'') echo "Invalid DRC_IF: $IFACE" >&2; exit 1;; esac
case "$MTU" in (*[!0-9]*|'') echo "Invalid DRC_MTU: $MTU" >&2; exit 1;; esac
[ -x "$HOSTAP_SOURCE/hostapd/hostapd" ] || {
    echo "Wii U hostapd not found: $HOSTAP_SOURCE/hostapd/hostapd" >&2; exit 1;
}
[ -x "$HOSTAP_SOURCE/hostapd/hostapd_cli" ] || {
    echo "hostapd_cli not found: $HOSTAP_SOURCE/hostapd/hostapd_cli" >&2; exit 1;
}
[ -f "$PAIR_SOURCE" ] || { echo "Pairing config not found: $PAIR_SOURCE" >&2; exit 1; }
[ -f "$NORMAL_SOURCE" ] || { echo "Normal AP config not found: $NORMAL_SOURCE" >&2; exit 1; }
if [ -n "$DNS_SOURCE" ] && [ ! -f "$DNS_SOURCE" ]; then
    echo "dnsmasq config not found: $DNS_SOURCE" >&2
    exit 1
fi

install -d -m 0755 "$INSTALL_DIR" "$INSTALL_DIR/runtime/hostapd" \
    "$CONFIG_DIR" /etc/polkit-1/rules.d
install -m 0755 "$SOURCE_DIR/gamepad-ap-control" "$CONTROL"
install -m 0755 "$SOURCE_DIR/ap-normal.sh" "$SOURCE_DIR/ap-pair.sh" \
    "$SOURCE_DIR/ap-stop.sh" "$INSTALL_DIR/"
install -m 0755 "$HOSTAP_SOURCE/hostapd/hostapd" \
    "$INSTALL_DIR/runtime/hostapd/hostapd"
install -m 0755 "$HOSTAP_SOURCE/hostapd/hostapd_cli" \
    "$INSTALL_DIR/runtime/hostapd/hostapd_cli"
install -m 0600 "$PAIR_SOURCE" "$CONFIG_DIR/pair.conf"
install -m 0600 "$NORMAL_SOURCE" "$CONFIG_DIR/normal.conf"

INSTALLED_DNS=
if [ -n "$DNS_SOURCE" ]; then
    install -m 0600 "$DNS_SOURCE" "$CONFIG_DIR/dnsmasq.conf"
    INSTALLED_DNS=$CONFIG_DIR/dnsmasq.conf
else
    rm -f "$CONFIG_DIR/dnsmasq.conf"
fi

config_tmp=$(mktemp)
rule_tmp=$(mktemp)
cleanup() { rm -f "$config_tmp" "$rule_tmp"; }
trap cleanup EXIT INT TERM
{
    printf 'DRC_IF=%q\n' "$IFACE"
    printf 'DRC_AP_MAC=%q\n' "$AP_MAC"
    printf 'DRC_HOSTAP=%q\n' "$INSTALL_DIR/runtime"
    printf 'DRC_RUN=%q\n' /run/bottom-screen-gamepad
    printf 'DRC_MTU=%q\n' "$MTU"
    printf 'DRC_DNSMASQ_CONF=%q\n' "$INSTALLED_DNS"
    printf 'DRC_UUID=%q\n' "$UUID"
    printf 'DRC_PAIR_CONF=%q\n' "$CONFIG_DIR/pair.conf"
    printf 'DRC_NORMAL_CONF=%q\n' "$CONFIG_DIR/normal.conf"
} > "$config_tmp"
install -m 0600 "$config_tmp" "$CONFIG_DIR/control.conf"

cat > "$rule_tmp" <<EOF
// Bottom Screen: allow only $TARGET_USER's active local session to control
// the root-owned Wii U GamePad AP helper. The helper accepts three fixed
// operations and never executes paths or commands supplied by the caller.
polkit.addRule(function(action, subject) {
    if (action.id == "org.freedesktop.policykit.exec" &&
        action.lookup("program") == "$CONTROL" &&
        subject.user == "$TARGET_USER" && subject.local && subject.active) {
        return polkit.Result.YES;
    }
});
EOF
install -m 0644 "$rule_tmp" "$RULE_FILE"

echo "Installed passwordless GamePad AP control for user $TARGET_USER."
echo "Restart the launcher; Sync, Launch AP / Reconnect and Stop AP will no longer prompt."
echo "Remove it later with: sudo $SOURCE_DIR/install-polkit.sh --uninstall"
