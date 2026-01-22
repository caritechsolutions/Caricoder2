# HLS Output Generator Makefile
# Generates HLS segments via FFmpeg - no embedded HTTP server needed

CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS =

TARGET = hls_output
SRC = hls_output.c

PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
