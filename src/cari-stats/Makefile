# CariTranscoder - Stats Daemon Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -I../common
LDFLAGS = -L../common -lcaricommon -lpthread -lrt

SRCS = main.c
OBJS = $(SRCS:.c=.o)
BIN = cari-stats

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJS) ../common/libcaricommon.a
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(BIN)
