# Top-level convenience Makefile for tiny_pomodoro.
#
# Usage:
#   make install-tiny
#   make install-release
#   make install-debug
#   make install-debugasan
#   make install-debugubsan
#
# Or equivalently:
#   make install BUILD_TYPE=tiny

ROOT_DIR   := $(dir $(realpath $(lastword $(MAKEFILE_LIST))))
PREFIX     ?= /usr/local

# Default build type when using 'make install'
BUILD_TYPE ?= tiny

# Shared install logic – calls cmake --install then refreshes the icon cache
define do_install
	@BUILD_DIR="$(ROOT_DIR)build/$(1)"; \
	if [ ! -d "$$BUILD_DIR" ]; then \
	    echo "ERROR: build/$(1)/ does not exist. Run build_all.sh or cmake first."; \
	    exit 1; \
	fi; \
	echo "Installing build type '$(1)' to $(PREFIX) ..."; \
	cmake --install "$$BUILD_DIR" --prefix $(PREFIX); \
	if command -v gtk-update-icon-cache >/dev/null 2>&1; then \
	    gtk-update-icon-cache -f -t "$(PREFIX)/share/icons/hicolor" 2>/dev/null || true; \
	fi; \
	echo "Done."
endef

.PHONY: install install-tiny install-release install-debug install-debugasan install-debugubsan

install:
	$(call do_install,$(BUILD_TYPE))

install-tiny:
	$(call do_install,tiny)

install-release:
	$(call do_install,release)

install-debug:
	$(call do_install,debug)

install-debugasan:
	$(call do_install,debugasan)

install-debugubsan:
	$(call do_install,debugubsan)
