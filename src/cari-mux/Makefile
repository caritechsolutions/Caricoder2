# CariMux - GStreamer MPTS Multiplexer Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g

# GStreamer flags (required)
GST_CFLAGS = $(shell pkg-config --cflags gstreamer-1.0 2>/dev/null)
GST_LIBS = $(shell pkg-config --libs gstreamer-1.0 2>/dev/null)

ifeq ($(GST_CFLAGS),)
$(warning GStreamer development libraries not found. Install libgstreamer1.0-dev)
endif

CFLAGS += $(GST_CFLAGS)
LDFLAGS = $(GST_LIBS) -lpthread

SRCS = main.c
OBJS = $(SRCS:.c=.o)
BIN = cari-mux

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(BIN)
