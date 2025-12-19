CC = gcc
CFLAGS = -Wall -Wextra -O2 -pthread
LIBS = -lmicrohttpd -pthread
TARGET = player_preview

SOURCES = player_preview.c
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
