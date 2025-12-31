# CariTranscoder ABR - Multi-Bitrate Transcoder
# Copyright (c) 2024 CariTech Solutions

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g
LDFLAGS = -lpthread

# GStreamer flags
GST_CFLAGS = $(shell pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 2>/dev/null)
GST_LIBS = $(shell pkg-config --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 2>/dev/null)

# If pkg-config fails, try manual paths for custom GStreamer install
ifeq ($(GST_CFLAGS),)
    GST_CFLAGS = -I/usr/local/include/gstreamer-1.0 \
                 -I/usr/include/glib-2.0 \
                 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include \
                 -I/usr/include/x86_64-linux-gnu \
                 -I/usr/local/include/orc-0.4 \
                 -pthread
    GST_LIBS = -L/usr/local/lib/x86_64-linux-gnu \
               -lgstapp-1.0 -lgstvideo-1.0 -lgstaudio-1.0 \
               -lgstbase-1.0 -lgstreamer-1.0 -lgobject-2.0 -lglib-2.0
endif

TARGET = cari-transcoder-abr
SRCS = main.c
OBJS = $(SRCS:.c=.o)

INSTALL_DIR = /usr/local/bin

.PHONY: all clean install check-deps

all: check-deps $(TARGET)

check-deps:
	@if ! pkg-config --exists gstreamer-1.0 2>/dev/null && \
	   [ ! -f /usr/local/lib/x86_64-linux-gnu/libgstreamer-1.0.so ]; then \
		echo "Error: GStreamer 1.0 not found. Install libgstreamer1.0-dev"; \
		exit 1; \
	fi

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(GST_LIBS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) $(GST_CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

install: $(TARGET)
	install -d $(INSTALL_DIR)
	install -m 755 $(TARGET) $(INSTALL_DIR)/
