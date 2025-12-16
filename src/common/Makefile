# CariTranscoder - Common Library Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -fPIC
LDFLAGS = -shared

# Source files
SRCS = config.c logging.c ring_buffer.c license.c ts_packet.c
OBJS = $(SRCS:.c=.o)

# Library name
LIB = libcaricommon.a
SHLIB = libcaricommon.so

.PHONY: all clean

all: $(LIB)

# Static library
$(LIB): $(OBJS)
	ar rcs $@ $^

# Shared library (optional)
$(SHLIB): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lssl -lcrypto -lpthread

# Object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Dependencies
config.o: config.c config.h logging.h
logging.o: logging.c logging.h
ring_buffer.o: ring_buffer.c ring_buffer.h ts_packet.h logging.h
license.o: license.c license.h logging.h config.h
ts_packet.o: ts_packet.c ts_packet.h

clean:
	rm -f $(OBJS) $(LIB) $(SHLIB)
