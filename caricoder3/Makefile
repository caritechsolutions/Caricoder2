# caricoder3 - MPEG-TS to MPEG-TS live processor
# Copyright (c) 2026 CariTech Solutions
#
#   make            optimised build        -> ./caricoder3
#   make debug      AddressSanitizer build -> ./caricoder3-asan
#   make test       build both and run the test suite
#   make fuzz       libFuzzer harnesses (need clang) -> build/fuzz_engine, build/fuzz_live
#   make install    copy the binary to $(PREFIX)/bin

CC      ?= gcc
PREFIX  ?= /usr/local
CFLAGS  ?= -O2 -g
WARN     = -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes
CPPFLAGS += -D_GNU_SOURCE -MMD -MP
LDLIBS   = -lpthread -lm

CORE = src/crc32.c src/log.c src/psi.c src/demux.c src/mux.c src/clock.c \
       src/engine.c src/codec.c
SRC  = $(CORE) src/udp.c src/main.c

OBJ      = $(SRC:src/%.c=build/rel/%.o)
OBJ_ASAN = $(SRC:src/%.c=build/asan/%.o)
SAN      = -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined

all: caricoder3

caricoder3: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

debug: caricoder3-asan

caricoder3-asan: $(OBJ_ASAN)
	$(CC) -O1 -g $(SAN) $(LDFLAGS) -o $@ $(OBJ_ASAN) $(LDLIBS)

build/rel/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=c11 $(CPPFLAGS) $(CFLAGS) $(WARN) -c $< -o $@

build/asan/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=c11 $(CPPFLAGS) -O1 -g $(SAN) $(WARN) -c $< -o $@

fuzz: build/fuzz_engine build/fuzz_live

build/fuzz_%: tests/fuzz_%.c $(CORE)
	@mkdir -p build
	clang -std=c11 -D_GNU_SOURCE -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION -O1 -g -Isrc \
	    -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
	    -o $@ $< $(CORE) -lpthread -lm

test: caricoder3 caricoder3-asan
	python3 tests/run_tests.py

install: caricoder3
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 caricoder3 $(DESTDIR)$(PREFIX)/bin/caricoder3

clean:
	rm -rf build caricoder3 caricoder3-asan

.PHONY: all debug fuzz test install clean
-include $(OBJ:.o=.d) $(OBJ_ASAN:.o=.d)
