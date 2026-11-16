PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
SYSTEMD_UNIT_DIR ?= /etc/systemd/system
UDEVDIR ?= /etc/udev/rules.d
DESTDIR ?=

.PHONY: install uninstall

install:
	DESTDIR="$(DESTDIR)" PREFIX="$(PREFIX)" BINDIR="$(BINDIR)" \
	SYSTEMD_UNIT_DIR="$(SYSTEMD_UNIT_DIR)" UDEVDIR="$(UDEVDIR)" \
	./install.sh

uninstall:
	DESTDIR="$(DESTDIR)" PREFIX="$(PREFIX)" BINDIR="$(BINDIR)" \
	SYSTEMD_UNIT_DIR="$(SYSTEMD_UNIT_DIR)" UDEVDIR="$(UDEVDIR)" \
	./uninstall.sh
