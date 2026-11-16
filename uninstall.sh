#!/bin/sh
set -eu

DESTDIR="${DESTDIR:-}"

if [ -z "$DESTDIR" ] && [ "$(id -u)" -ne 0 ]; then
    echo "Error: uninstall.sh must be run as root (try: sudo ./uninstall.sh)" >&2
    exit 1
fi

PREFIX="${PREFIX:-/usr/local}"
BINDIR="${BINDIR:-$PREFIX/bin}"
UDEVDIR="${UDEVDIR:-/etc/udev/rules.d}"
SYSTEMD_UNIT_DIR="${SYSTEMD_UNIT_DIR:-/etc/systemd/system}"

if [ -z "$DESTDIR" ] && [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1; then
    systemctl disable --now thinkpad-battery-fix.service 2>/dev/null || true
fi

rm -f "$DESTDIR$BINDIR/thinkpad-battery-fix" \
      "$DESTDIR/usr/local/bin/thinkpad-battery-fix" \
      "$DESTDIR/usr/bin/thinkpad-battery-fix" \
      "$DESTDIR$SYSTEMD_UNIT_DIR/thinkpad-battery-fix.service" \
      "$DESTDIR$UDEVDIR/99-thinkpad-battery-fix.rules"

if [ -z "$DESTDIR" ]; then
    if command -v udevadm >/dev/null 2>&1; then
        udevadm control --reload-rules || true
    fi
    if [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1; then
        systemctl daemon-reload
    fi
fi

echo "Uninstalled thinkpad-battery-fix."
