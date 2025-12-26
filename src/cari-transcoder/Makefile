# CariTranscoder - GStreamer Transcoder Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g

# GStreamer flags
GST_CFLAGS = $(shell pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-audio-1.0 2>/dev/null)
GST_LIBS = $(shell pkg-config --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-audio-1.0 2>/dev/null)

CFLAGS += $(GST_CFLAGS)
LDFLAGS = $(GST_LIBS) -lpthread

# Source files
SRCS = main.c
OBJS = $(SRCS:.c=.o)

# Binary name
BIN = cari-transcoder

# Installation paths
PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin

.PHONY: all clean install uninstall check-deps

all: check-deps $(BIN)

$(BIN): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

check-deps:
	@pkg-config --exists gstreamer-1.0 || (echo "Error: GStreamer 1.0 not found. Install libgstreamer1.0-dev" && exit 1)
	@pkg-config --exists gstreamer-app-1.0 || (echo "Error: GStreamer app plugin not found" && exit 1)
	@pkg-config --exists gstreamer-video-1.0 || (echo "Error: GStreamer video plugin not found" && exit 1)

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)

clean:
	rm -f $(OBJS) $(BIN)

# Debug build
debug: CFLAGS += -DDEBUG -O0 -g3
debug: clean $(BIN)

# Show GStreamer configuration
show-config:
	@echo "GStreamer CFLAGS: $(GST_CFLAGS)"
	@echo "GStreamer LIBS: $(GST_LIBS)"
	@echo "GStreamer version: $(shell pkg-config --modversion gstreamer-1.0)"
