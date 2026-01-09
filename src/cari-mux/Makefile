# CariMux - GStreamer MPTS Multiplexer Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g

# GStreamer flags (required: gstreamer-1.0 and gstreamer-mpegts-1.0 for SDT)
GST_PACKAGES = gstreamer-1.0 gstreamer-mpegts-1.0
GST_CFLAGS = $(shell pkg-config --cflags $(GST_PACKAGES) 2>/dev/null)
GST_LIBS = $(shell pkg-config --libs $(GST_PACKAGES) 2>/dev/null)

ifeq ($(GST_CFLAGS),)
$(warning GStreamer development libraries not found. Install libgstreamer1.0-dev libgstreamer-plugins-bad1.0-dev)
endif

CFLAGS += $(GST_CFLAGS)
LDFLAGS = $(GST_LIBS) -lpthread

SRCS = main.c
OBJS = $(SRCS:.c=.o)
BIN = cari-mux

.PHONY: all clean install

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

install: $(BIN)
	install -m 755 $(BIN) /usr/local/bin/

clean:
	rm -f $(OBJS) $(BIN)
