# CariTranscoder - Main Makefile
# Copyright (c) 2024 CariTech Solutions

.PHONY: all clean install uninstall common input transcoder mux output stats ha

# Installation directories
PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
LIBDIR = $(PREFIX)/lib
CONFDIR = /etc/caritrans
WEBDIR = /var/www/caritrans
SYSTEMDDIR = /etc/systemd/system

# Compiler settings
CC = gcc
CFLAGS = -Wall -Wextra -O2 -g
LDFLAGS = -lpthread -lrt -lssl -lcrypto

# GStreamer flags
GST_CFLAGS = $(shell pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 2>/dev/null)
GST_LIBS = $(shell pkg-config --libs gstreamer-1.0 gstreamer-app-1.0 2>/dev/null)

# Subdirectories
SUBDIRS = src/common src/cari-input src/cari-transcoder src/cari-mux src/cari-output src/cari-stats src/cari-ha

# Build all
all: common input transcoder mux output stats ha

# Build common library
common:
	@echo "Building common library..."
	$(MAKE) -C src/common

# Build applications
input: common
	@echo "Building cari-input..."
	$(MAKE) -C src/cari-input

transcoder: common
	@echo "Building cari-transcoder..."
	$(MAKE) -C src/cari-transcoder

mux: common
	@echo "Building cari-mux..."
	$(MAKE) -C src/cari-mux

output: common
	@echo "Building cari-output..."
	$(MAKE) -C src/cari-output

stats: common
	@echo "Building cari-stats..."
	$(MAKE) -C src/cari-stats

ha: common
	@echo "Building cari-ha..."
	$(MAKE) -C src/cari-ha

# Clean all
clean:
	@for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir clean 2>/dev/null || true; \
	done

# Install
install: all install-bin install-config install-web install-systemd
	@echo "Installation complete!"
	@echo ""
	@echo "Post-installation steps:"
	@echo "  1. Create caritrans user: useradd -r -s /bin/false caritrans"
	@echo "  2. Set permissions: chown -R caritrans:caritrans $(CONFDIR)"
	@echo "  3. Reload systemd: systemctl daemon-reload"
	@echo "  4. Start web interface: systemctl start cari-web"

install-bin:
	@echo "Installing binaries..."
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 src/cari-input/cari-input $(DESTDIR)$(BINDIR)/ 2>/dev/null || true
	install -m 755 src/cari-transcoder/cari-transcoder $(DESTDIR)$(BINDIR)/ 2>/dev/null || true
	install -m 755 src/cari-mux/cari-mux $(DESTDIR)$(BINDIR)/ 2>/dev/null || true
	install -m 755 src/cari-output/cari-output $(DESTDIR)$(BINDIR)/ 2>/dev/null || true
	install -m 755 src/cari-stats/cari-stats $(DESTDIR)$(BINDIR)/ 2>/dev/null || true
	install -m 755 src/cari-ha/cari-ha $(DESTDIR)$(BINDIR)/ 2>/dev/null || true

install-config:
	@echo "Installing configuration files..."
	install -d $(DESTDIR)$(CONFDIR)
	install -d $(DESTDIR)$(CONFDIR)/inputs
	install -d $(DESTDIR)$(CONFDIR)/transcoders
	install -d $(DESTDIR)$(CONFDIR)/muxers
	install -d $(DESTDIR)$(CONFDIR)/outputs
	install -m 644 config/caritrans.conf $(DESTDIR)$(CONFDIR)/
	install -m 600 config/users.conf $(DESTDIR)$(CONFDIR)/
	@# Install example configs
	install -m 644 config/inputs/*.conf $(DESTDIR)$(CONFDIR)/inputs/ 2>/dev/null || true
	install -m 644 config/transcoders/*.conf $(DESTDIR)$(CONFDIR)/transcoders/ 2>/dev/null || true
	install -m 644 config/muxers/*.conf $(DESTDIR)$(CONFDIR)/muxers/ 2>/dev/null || true
	install -m 644 config/outputs/*.conf $(DESTDIR)$(CONFDIR)/outputs/ 2>/dev/null || true

install-web:
	@echo "Installing web interface..."
	install -d $(DESTDIR)$(WEBDIR)
	cp -r web/* $(DESTDIR)$(WEBDIR)/
	chown -R www-data:www-data $(DESTDIR)$(WEBDIR) 2>/dev/null || true

install-systemd:
	@echo "Installing systemd service files..."
	install -d $(DESTDIR)$(SYSTEMDDIR)
	install -m 644 systemd/*.service $(DESTDIR)$(SYSTEMDDIR)/

# Uninstall
uninstall:
	@echo "Uninstalling CariTranscoder..."
	rm -f $(DESTDIR)$(BINDIR)/cari-input
	rm -f $(DESTDIR)$(BINDIR)/cari-transcoder
	rm -f $(DESTDIR)$(BINDIR)/cari-mux
	rm -f $(DESTDIR)$(BINDIR)/cari-output
	rm -f $(DESTDIR)$(BINDIR)/cari-stats
	rm -f $(DESTDIR)$(BINDIR)/cari-ha
	rm -f $(DESTDIR)$(SYSTEMDDIR)/cari-*.service
	@echo "Note: Configuration and web files not removed. Remove manually if needed."

# Create required directories
dirs:
	install -d /run/caritrans
	install -d /var/log/caritrans
	install -d /var/lib/caritrans
	chown caritrans:caritrans /run/caritrans /var/log/caritrans /var/lib/caritrans

# Development helpers
dev-deps:
	@echo "Installing development dependencies..."
	apt-get update
	apt-get install -y \
		build-essential \
		pkg-config \
		libgstreamer1.0-dev \
		libgstreamer-plugins-base1.0-dev \
		libssl-dev \
		php-cli \
		php-json

.PHONY: test
test:
	@echo "Running tests..."
	@# Add test commands here
