# Copyright (c) 2026 Dominik Klein
# Convenience Makefile wrapper around Meson for Xfburn-X (GPL-2.0-or-later)

PREFIX   ?= $(HOME)/.local
BUILDDIR ?= build

.PHONY: all setup build test install uninstall run clean help

all: build

setup:
	@if [ -d "$(BUILDDIR)/meson-private" ]; then \
		meson configure "$(BUILDDIR)" --prefix="$(PREFIX)"; \
	else \
		meson setup "$(BUILDDIR)" --prefix="$(PREFIX)"; \
	fi

build: setup
	meson compile -C "$(BUILDDIR)"

test: build
	meson test -C "$(BUILDDIR)" --verbose

install: build
	meson install -C "$(BUILDDIR)"
	@if command -v gtk-update-icon-cache >/dev/null 2>&1 && [ -d "$(PREFIX)/share/icons/hicolor" ]; then \
		gtk-update-icon-cache -f -t "$(PREFIX)/share/icons/hicolor" || true; \
	fi
	@if command -v update-desktop-database >/dev/null 2>&1 && [ -d "$(PREFIX)/share/applications" ]; then \
		update-desktop-database "$(PREFIX)/share/applications" || true; \
	fi

uninstall:
	ninja -C "$(BUILDDIR)" uninstall

run: install
	"$(PREFIX)/bin/xfburn-x"

clean:
	rm -rf "$(BUILDDIR)"

help:
	@echo "Xfburn-X build targets (PREFIX=$(PREFIX), BUILDDIR=$(BUILDDIR)):"
	@echo "  make           / make build     Configure and compile Xfburn-X"
	@echo "  make test                       Compile and run unit tests"
	@echo "  make install                    Compile and install to PREFIX (default: ~/.local)"
	@echo "  make uninstall                  Uninstall files installed from BUILDDIR"
	@echo "  make run                        Build, install to PREFIX, and launch xfburn-x"
	@echo "  make clean                      Remove $(BUILDDIR)/"
	@echo ""
	@echo "Examples:"
	@echo "  make install                    Install for current user (~/.local)"
	@echo "  sudo make install PREFIX=/usr   Install system-wide"
