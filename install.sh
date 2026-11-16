#!/bin/sh
set -eu

DESTDIR="${DESTDIR:-}"

if [ -z "$DESTDIR" ] && [ "$(id -u)" -ne 0 ]; then
    echo "Error: install.sh must be run as root (try: sudo ./install.sh)" >&2
    exit 1
fi

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PREFIX="${PREFIX:-/usr/local}"
BINDIR="${BINDIR:-$PREFIX/bin}"
UDEVDIR="${UDEVDIR:-/etc/udev/rules.d}"
SYSTEMD_UNIT_DIR="${SYSTEMD_UNIT_DIR:-/etc/systemd/system}"

install -Dm0755 "$SCRIPT_DIR/bin/thinkpad-battery-fix" "$DESTDIR$BINDIR/thinkpad-battery-fix"

if [ -d /etc/udev/rules.d ] || [ -n "$DESTDIR" ] || command -v udevadm >/dev/null 2>&1; then
    install -Dm0644 "$SCRIPT_DIR/udev/99-thinkpad-battery-fix.rules" "$DESTDIR$UDEVDIR/99-thinkpad-battery-fix.rules"
fi

if command -v systemctl >/dev/null 2>&1 || [ -d /etc/systemd/system ] || [ -n "$DESTDIR" ]; then
    mkdir -p "$DESTDIR$SYSTEMD_UNIT_DIR"
    sed "s|/usr/local/bin/thinkpad-battery-fix|$BINDIR/thinkpad-battery-fix|g" \
        "$SCRIPT_DIR/systemd/thinkpad-battery-fix.service" \
        > "$DESTDIR$SYSTEMD_UNIT_DIR/thinkpad-battery-fix.service"
    chmod 0644 "$DESTDIR$SYSTEMD_UNIT_DIR/thinkpad-battery-fix.service"
fi

if [ -z "$DESTDIR" ]; then
    if command -v udevadm >/dev/null 2>&1; then
        udevadm control --reload-rules || true
    fi

    if [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1; then
        systemctl daemon-reload
        systemctl enable --now thinkpad-battery-fix.service
        echo "Installed and enabled thinkpad-battery-fix.service (systemd)."
    else
        echo "Installed binary and udev rule to $BINDIR/thinkpad-battery-fix."
        echo "Non-systemd init detected: start '$BINDIR/thinkpad-battery-fix daemon' via your init system (OpenRC/runit/SysVinit)."
    fi
fi
