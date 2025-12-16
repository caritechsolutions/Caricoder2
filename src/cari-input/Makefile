# CariTranscoder - Input Application Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -I../common
LDFLAGS = -L../common -lcaricommon -lpthread -lrt -lssl -lcrypto

# GStreamer flags
GST_CFLAGS = $(shell pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 2>/dev/null)
GST_LIBS = $(shell pkg-config --libs gstreamer-1.0 gstreamer-app-1.0 2>/dev/null)

# SRT support (optional)
SRT_CFLAGS = $(shell pkg-config --cflags srt 2>/dev/null)
SRT_LIBS = $(shell pkg-config --libs srt 2>/dev/null)

CFLAGS += $(GST_CFLAGS) $(SRT_CFLAGS)
LDFLAGS += $(GST_LIBS) $(SRT_LIBS)

# Source files
SRCS = main.c
OBJS = $(SRCS:.c=.o)

# Binary name
BIN = cari-input

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJS) ../common/libcaricommon.a
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

../common/libcaricommon.a:
	$(MAKE) -C ../common

clean:
	rm -f $(OBJS) $(BIN)
