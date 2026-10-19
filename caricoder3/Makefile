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
#   Ubuntu: apt install libtwolame-dev libmp3lame-dev (fdk-aac: see install.sh) \
#           libmpg123-dev liba52-0.7.4-dev libx264-dev libx265-dev \
#           libavcodec-dev libavutil-dev libsrt-openssl-dev librist-dev pkg-config
#
# Libraries built from source by install.sh (the full fdk-aac, with HE-AAC)
# live under LIBS_PREFIX and take precedence over the distribution's.
LIBS_PREFIX ?= /opt/caricoder3-libs
PKG_CONFIG ?= pkg-config
ifneq ($(wildcard $(LIBS_PREFIX)/lib/pkgconfig),)
PKG_CONFIG := PKG_CONFIG_PATH=$(LIBS_PREFIX)/lib/pkgconfig:$$PKG_CONFIG_PATH $(PKG_CONFIG)
endif
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
# Output transports (apt install libsrt-openssl-dev librist-dev). Absent,
# the srt:// and rist:// output types are left out and say so.
ifeq ($(call have_pkg,srt),yes)
CODEC_DEFS += -DHAVE_SRT $(shell $(PKG_CONFIG) --cflags srt)
CODEC_LIBS += $(shell $(PKG_CONFIG) --libs srt)
SINK_SRC += src/sink_srt.c src/input_srt.c
endif
ifeq ($(call have_pkg,librist),yes)
CODEC_DEFS += -DHAVE_RIST $(shell $(PKG_CONFIG) --cflags librist)
CODEC_LIBS += $(shell $(PKG_CONFIG) --libs librist)
SINK_SRC += src/sink_rist.c src/input_rist.c
endif
# The supervisor's history (apt install libsqlite3-dev). Absent, none is kept.
ifeq ($(call have_pkg,sqlite3),yes)
CODEC_DEFS += -DHAVE_SQLITE $(shell $(PKG_CONFIG) --cflags sqlite3)
CODEC_LIBS += $(shell $(PKG_CONFIG) --libs sqlite3)
endif
CPPFLAGS += $(CODEC_DEFS)
LDLIBS   += $(CODEC_LIBS)

CORE = src/crc32.c src/log.c src/psi.c src/demux.c src/mux.c src/clock.c \
       src/engine.c src/codec.c src/xcode.c src/xc_audio.c src/xc_video.c \
       src/aframe.c src/a_fdk.c src/a_mpg123.c src/a_twolame.c src/a_lame.c \
       src/a_a52.c src/v_avcodec.c src/v_x264.c src/v_x265.c \
       src/nv_common.c src/v_nvdec.c src/v_nvenc.c src/nv_selftest.c
SRC  = $(CORE) src/udp.c src/ring.c src/sink.c src/input.c src/input_http.c src/source.c src/thumb.c src/probe.c src/share.c src/history.c src/sysstat.c src/sink_udp.c src/h264.c src/sink_rtmp.c src/sink_hls.c $(SINK_SRC) src/json.c src/control.c src/http.c src/super.c src/main.c

# The first rule below is the GUI's, so the default goal is named here.
.DEFAULT_GOAL := all

OBJ      = $(SRC:src/%.c=build/rel/%.o) build/rel/gui_html.o
OBJ_ASAN = $(SRC:src/%.c=build/asan/%.o) build/asan/gui_html.o

# The GUI, one HTML file plus the hls.js player, built into the binary.
build/gui_html.c: gui/index.html gui/hls.min.js
	@mkdir -p build
	@( echo 'const unsigned char gui_html[] = {'; od -An -tx1 -v gui/index.html | sed 's/ \([0-9a-f][0-9a-f]\)/0x\1,/g'; \
	   echo '};'; echo "const unsigned int gui_html_len = $$(wc -c < gui/index.html);"; \
	   echo 'const unsigned char gui_hls_js[] = {'; od -An -tx1 -v gui/hls.min.js | sed 's/ \([0-9a-f][0-9a-f]\)/0x\1,/g'; \
	   echo '};'; echo "const unsigned int gui_hls_js_len = $$(wc -c < gui/hls.min.js);" ) > $@
build/rel/gui_html.o: build/gui_html.c
	@mkdir -p build/rel
	$(CC) -c -o $@ $<
build/asan/gui_html.o: build/gui_html.c
	@mkdir -p build/asan
	$(CC) -c -o $@ $<
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
