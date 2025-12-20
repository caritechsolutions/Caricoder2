CC = gcc
CFLAGS = -Wall -Wextra -O2 -pthread
LIBS = -lmicrohttpd -lm -pthread
TARGET = cari-avsync

SOURCES = cari-avsync.c
OBJECTS = $(SOURCES:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

clean:
	rm -f $(OBJECTS) $(TARGET)

.PHONY: all install clean
