# Makefile for rist_input
CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lmicrohttpd -lpthread

TARGET = rist_input
SRCS = rist_input.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
