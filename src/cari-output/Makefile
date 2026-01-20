# CariTranscoder - Output Application Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -I../common
LDFLAGS = -L../common -lcaricommon -lpthread -lrt -lssl -lcrypto

GST_CFLAGS = $(shell pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 2>/dev/null)
GST_LIBS = $(shell pkg-config --libs gstreamer-1.0 gstreamer-app-1.0 2>/dev/null)
SRT_CFLAGS = $(shell pkg-config --cflags srt 2>/dev/null)
SRT_LIBS = $(shell pkg-config --libs srt 2>/dev/null)

CFLAGS += $(GST_CFLAGS) $(SRT_CFLAGS)
LDFLAGS += $(GST_LIBS) $(SRT_LIBS)

SRCS = main.c
OBJS = $(SRCS:.c=.o)
BIN = cari-output

.PHONY: all clean install common

all: common $(BIN)

common:
	$(MAKE) -C ../common

$(BIN): $(OBJS) ../common/libcaricommon.a
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(BIN)

install: $(BIN)
	install -m 755 $(BIN) /usr/local/bin/
