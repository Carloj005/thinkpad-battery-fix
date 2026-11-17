PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
SYSTEMD_UNIT_DIR ?= /etc/systemd/system
UDEVDIR ?= /etc/udev/rules.d
KVER ?= $(shell uname -r)
DESTDIR ?=

.PHONY: install uninstall kmod clean

kmod:
	$(MAKE) -C /lib/modules/$(KVER)/build M=$(CURDIR)/kmod modules

clean:
	$(MAKE) -C /lib/modules/$(KVER)/build M=$(CURDIR)/kmod clean

install:
	DESTDIR="$(DESTDIR)" PREFIX="$(PREFIX)" BINDIR="$(BINDIR)" \
	SYSTEMD_UNIT_DIR="$(SYSTEMD_UNIT_DIR)" UDEVDIR="$(UDEVDIR)" \
	KVER="$(KVER)" ./install.sh

uninstall:
	DESTDIR="$(DESTDIR)" PREFIX="$(PREFIX)" BINDIR="$(BINDIR)" \
	SYSTEMD_UNIT_DIR="$(SYSTEMD_UNIT_DIR)" UDEVDIR="$(UDEVDIR)" \
	KVER="$(KVER)" ./uninstall.sh
