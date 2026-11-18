#!/bin/sh
set -eu

REPO_URL="${THINKPAD_BATTERY_FIX_REPO:-https://github.com/Carloj005/thinkpad-battery-fix}"
BRANCH="${THINKPAD_BATTERY_FIX_REF:-main}"

if [ "$(id -u)" -ne 0 ]; then
    echo "Error: run as root, e.g.: curl -fsSL https://raw.githubusercontent.com/Carloj005/thinkpad-battery-fix/main/install-remote.sh | sudo bash" >&2
    exit 1
fi

TMP_DIR="$(mktemp -d)"
cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT INT TERM

if command -v git >/dev/null 2>&1; then
    echo "Cloning thinkpad-battery-fix ($BRANCH)..."
    git clone --depth 1 --branch "$BRANCH" "${REPO_URL}.git" "$TMP_DIR/src"
elif command -v curl >/dev/null 2>&1 && command -v tar >/dev/null 2>&1; then
    echo "Downloading thinkpad-battery-fix ($BRANCH) archive..."
    mkdir -p "$TMP_DIR/src"
    curl -fsSL "${REPO_URL}/archive/refs/heads/${BRANCH}.tar.gz" | tar -xz -C "$TMP_DIR/src" --strip-components=1
else
    echo "Error: requires git, or curl + tar." >&2
    exit 1
fi

chmod +x "$TMP_DIR/src/install.sh"
exec "$TMP_DIR/src/install.sh"
