CC = gcc
CFLAGS = -Wall -Wextra -O2 -pthread
LIBS = -lmicrohttpd -pthread
TARGET = udp_input

SOURCES = udp_input.c
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
