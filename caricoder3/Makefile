# caricoder3 - MPEG-TS to MPEG-TS live processor
# Copyright (c) 2026 CariTech Solutions
#
#   make            optimised build        -> ./caricoder3
#   make debug      AddressSanitizer build -> ./caricoder3-asan
#   make test       build both and run the test suite
#   make fuzz       libFuzzer harnesses (need clang) -> build/fuzz_engine, fuzz_live, fuzz_xcode
#   make install    copy the binary to $(PREFIX)/bin

CC      ?= gcc
PREFIX  ?= /usr/local
CFLAGS  ?= -O2 -g
WARN     = -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes
CPPFLAGS += -D_GNU_SOURCE -MMD -MP
LDLIBS   = -lpthread -lm

# ---- codec libraries ---------------------------------------------------
# Each is used if it is installed and left out if it is not: the program
# builds and runs either way, and `caricoder3 --backends` shows what it
# got. `make CODECS=none` builds the plain remultiplexer whatever is
# installed.
#
#   Ubuntu: apt install libfdk-aac-dev libtwolame-dev libmp3lame-dev \
#           libmpg123-dev liba52-0.7.4-dev libx264-dev libx265-dev \
#           libavcodec-dev libavutil-dev pkg-config
PKG_CONFIG ?= pkg-config
have_pkg = $(shell $(PKG_CONFIG) --exists $(1) 2>/dev/null && echo yes)
have_hdr = $(shell echo '\#include <$(1)>' | $(CC) -E -x c - >/dev/null 2>&1 && echo yes)

CODEC_DEFS :=
CODEC_LIBS :=
ifneq ($(CODECS),none)
ifeq ($(call have_pkg,fdk-aac),yes)
CODEC_DEFS += -DHAVE_FDK_AAC $(shell $(PKG_CONFIG) --cflags fdk-aac)
CODEC_LIBS += $(shell $(PKG_CONFIG) --libs fdk-aac)
endif
ifeq ($(call have_pkg,libmpg123),yes)
CODEC_DEFS += -DHAVE_MPG123 $(shell $(PKG_CONFIG) --cflags libmpg123)
CODEC_LIBS += $(shell $(PKG_CONFIG) --libs libmpg123)
endif
ifeq ($(call have_pkg,twolame),yes)
CODEC_DEFS += -DHAVE_TWOLAME $(shell $(PKG_CONFIG) --cflags twolame)
CODEC_LIBS += $(shell $(PKG_CONFIG) --libs twolame)
endif
ifeq ($(call have_hdr,lame/lame.h),yes)
CODEC_DEFS += -DHAVE_LAME
CODEC_LIBS += -lmp3lame
endif
ifeq ($(call have_hdr,a52dec/a52.h),yes)
CODEC_DEFS += -DHAVE_A52
CODEC_LIBS += -la52
endif
ifeq ($(call have_pkg,libavcodec libavutil),yes)
CODEC_DEFS += -DHAVE_AVCODEC $(shell $(PKG_CONFIG) --cflags libavcodec libavutil)
CODEC_LIBS += $(shell $(PKG_CONFIG) --libs libavcodec libavutil)
endif
ifeq ($(call have_pkg,x264),yes)
CODEC_DEFS += -DHAVE_X264 $(shell $(PKG_CONFIG) --cflags x264)
CODEC_LIBS += $(shell $(PKG_CONFIG) --libs x264)
endif
ifeq ($(call have_pkg,x265),yes)
CODEC_DEFS += -DHAVE_X265 $(shell $(PKG_CONFIG) --cflags x265)
CODEC_LIBS += $(shell $(PKG_CONFIG) --libs x265)
endif
# NVIDIA needs nothing installed to build: the headers are in third_party
# and the driver's libraries are opened when the program runs. On a
# machine without the driver the two backends say "unavailable".
# `make NVIDIA=0` leaves them out.
ifneq ($(NVIDIA),0)
CODEC_DEFS += -DHAVE_NVIDIA
CODEC_LIBS += -ldl
endif
endif
CPPFLAGS += $(CODEC_DEFS)
LDLIBS   += $(CODEC_LIBS)

CORE = src/crc32.c src/log.c src/psi.c src/demux.c src/mux.c src/clock.c \
       src/engine.c src/codec.c src/xcode.c src/xc_audio.c src/xc_video.c \
       src/aframe.c src/a_fdk.c src/a_mpg123.c src/a_twolame.c src/a_lame.c \
       src/a_a52.c src/v_avcodec.c src/v_x264.c src/v_x265.c \
       src/nv_common.c src/v_nvdec.c src/v_nvenc.c src/nv_selftest.c
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

# Which libraries were found is written to a file that every object
# depends on, so installing or removing one rebuilds what it affects
# instead of leaving objects from two different configurations side by
# side.
build/config: FORCE
	@mkdir -p build
	@echo '$(CC) $(CFLAGS) $(CODEC_DEFS)' | cmp -s - $@ || echo '$(CC) $(CFLAGS) $(CODEC_DEFS)' > $@

FORCE:

build/rel/%.o: src/%.c build/config
	@mkdir -p $(dir $@)
	$(CC) -std=c11 $(CPPFLAGS) $(CFLAGS) $(WARN) -c $< -o $@

build/asan/%.o: src/%.c build/config
	@mkdir -p $(dir $@)
	$(CC) -std=c11 $(CPPFLAGS) -O1 -g $(SAN) $(WARN) -c $< -o $@

fuzz: build/fuzz_engine build/fuzz_live build/fuzz_xcode

build/fuzz_%: tests/fuzz_%.c $(CORE) build/config
	@mkdir -p build
	clang -std=c11 -D_GNU_SOURCE -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION -O1 -g -Isrc \
	    $(CODEC_DEFS) -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
	    -o $@ $< $(CORE) $(LDLIBS)

test: caricoder3 caricoder3-asan
	python3 tests/run_tests.py

install: caricoder3
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 caricoder3 $(DESTDIR)$(PREFIX)/bin/caricoder3

clean:
	rm -rf build caricoder3 caricoder3-asan

.PHONY: all debug fuzz test install clean FORCE
-include $(OBJ:.o=.d) $(OBJ_ASAN:.o=.d)
