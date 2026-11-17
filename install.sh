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
KVER="${KVER:-$(uname -r)}"
if [ -L /lib ]; then
    MODDIR="${MODDIR:-/usr/lib/modules}"
else
    MODDIR="${MODDIR:-/lib/modules}"
fi

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

KMOD_BUILT=0
if [ -d "$SCRIPT_DIR/kmod" ] && [ -d "/lib/modules/$KVER/build" ] && command -v make >/dev/null 2>&1; then
    echo "Building thinkpad_battery_fix_kmod for kernel $KVER..."
    TMP_KMOD="$(mktemp -d)"
    cp -a "$SCRIPT_DIR/kmod/." "$TMP_KMOD/"
    if make -C "/lib/modules/$KVER/build" M="$TMP_KMOD" modules; then
        install -Dm0644 "$TMP_KMOD/thinkpad_battery_fix_kmod.ko" \
            "$DESTDIR$MODDIR/$KVER/extra/thinkpad_battery_fix_kmod.ko"
        mkdir -p "$DESTDIR/etc/modules-load.d"
        printf 'thinkpad_battery_fix_kmod\n' > "$DESTDIR/etc/modules-load.d/thinkpad-battery-fix.conf"
        chmod 0644 "$DESTDIR/etc/modules-load.d/thinkpad-battery-fix.conf"
        KMOD_BUILT=1
    else
        echo "Warning: failed to build thinkpad_battery_fix_kmod; continuing with userspace daemon only." >&2
    fi
    rm -rf "$TMP_KMOD"
else
    echo "Warning: kernel headers for $KVER not found; skipping kernel module build." >&2
fi

if [ -z "$DESTDIR" ]; then
    if [ "$KMOD_BUILT" -eq 1 ]; then
        if [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1; then
            systemctl stop thinkpad-battery-fix.service 2>/dev/null || true
        fi
        depmod -a "$KVER"
        if grep -q '^thinkpad_battery_fix_kmod ' /proc/modules 2>/dev/null; then
            rmmod thinkpad_battery_fix_kmod 2>/dev/null || true
        fi
        modprobe thinkpad_battery_fix_kmod || true
    fi

    if command -v udevadm >/dev/null 2>&1; then
        udevadm control --reload-rules || true
    fi

    if [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1; then
        systemctl daemon-reload
        systemctl enable thinkpad-battery-fix.service
        systemctl restart thinkpad-battery-fix.service
        echo "Installed, enabled, and started thinkpad-battery-fix.service (systemd)."
    else
        echo "Installed binary, kmod, and udev rule to $BINDIR/thinkpad-battery-fix."
        echo "Non-systemd init detected: start '$BINDIR/thinkpad-battery-fix daemon' via your init system (OpenRC/runit/SysVinit)."
    fi
fi
