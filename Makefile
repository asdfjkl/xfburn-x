# Copyright (c) 2026 Dominik Klein
# Convenience Makefile wrapper around Meson for Xfburn-X (GPL-2.0-or-later)

PREFIX   ?= $(HOME)/.local
BUILDDIR ?= build

.PHONY: all setup build test install uninstall run deb clean help

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
	@if [ -d "$(PREFIX)/share/icons/hicolor" ]; then \
		if find "$(PREFIX)/share/icons/hicolor" -type f \( -name '*.png' -o -name '*.svg' \) | grep -q .; then \
			if command -v gtk-update-icon-cache >/dev/null 2>&1; then \
				gtk-update-icon-cache -f -t "$(PREFIX)/share/icons/hicolor" || true; \
			fi; \
		else \
			rm -f "$(PREFIX)/share/icons/hicolor/icon-theme.cache"; \
		fi; \
	fi
	@if command -v update-desktop-database >/dev/null 2>&1 && [ -d "$(PREFIX)/share/applications" ]; then \
		update-desktop-database "$(PREFIX)/share/applications" || true; \
	fi

run: install
	"$(PREFIX)/bin/xfburn-x"

deb:
	./packaging/build-deb.sh

clean:
	rm -rf "$(BUILDDIR)" build-deb

help:
	@echo "Xfburn-X build targets (PREFIX=$(PREFIX), BUILDDIR=$(BUILDDIR)):"
	@echo "  make           / make build     Configure and compile Xfburn-X"
	@echo "  make test                       Compile and run unit tests"
	@echo "  make install                    Compile and install to PREFIX (default: ~/.local)"
	@echo "  make uninstall                  Uninstall files installed from BUILDDIR"
	@echo "  make run                        Build, install to PREFIX, and launch xfburn-x"
	@echo "  make deb                        Build and verify amd64 .deb package in dist/"
	@echo "  make clean                      Remove $(BUILDDIR)/ and build-deb/"
	@echo ""
	@echo "Examples:"
	@echo "  make install                    Install for current user (~/.local)"
	@echo "  sudo make install PREFIX=/usr   Install system-wide"
