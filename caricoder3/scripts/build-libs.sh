#!/usr/bin/env bash
#
# Builds the libraries caricoder3 carries in its own repository
# (third_party/src) into a folder the Makefile looks in, as static
# libraries: the binary then holds them, and every server runs the same
# version whatever its Ubuntu offers.
# Copyright (c) 2026 CariTech Solutions
#
#   scripts/build-libs.sh PREFIX [WORK]
#
#   PREFIX   where they go (the installer: /opt/caricoder3-libs)
#   WORK     scratch folder (default: a temporary one, removed after)
#
# Each is built once, and again only when its version here changes. A
# library that does not build is reported and left out (exit status 1
# after the others are done): what was in PREFIX before stays.
#
# Needs: gcc g++ cmake (fdk-aac, SRT), meson ninja (RIST), and the OpenSSL
# headers (SRT's encryption).
#
set -uo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$HERE/third_party/src"
PREFIX="${1:?usage: build-libs.sh PREFIX [WORK]}"
WORK="${2:-}"
OWN_WORK=0
FAILED=0

note() { printf '    %s\n' "$*" >&2; }

if [ -z "$WORK" ]; then
    WORK="$(mktemp -d)"
    OWN_WORK=1
fi
mkdir -p "$PREFIX" "$WORK"
JOBS="$(nproc 2>/dev/null || echo 2)"

# name version pkg-config-name
have() {
    [ -f "$PREFIX/$1.version" ] && [ "$(cat "$PREFIX/$1.version")" = "$2" ] &&
        [ -f "$PREFIX/lib/pkgconfig/$3.pc" ]
}

# name version : unpacks third_party/src/NAME-VERSION.tar.gz, after checking it
unpack() {
    local tarball="$SRC/$1-$2.tar.gz"

    [ -f "$tarball" ] || { note "$1 $2: its source is not in third_party/src"; return 1; }
    if command -v sha256sum >/dev/null 2>&1; then
        (cd "$SRC" && grep " $1-$2.tar.gz\$" SHA256SUMS | sha256sum -c --status) ||
            { note "$1 $2: the source in third_party/src is not the one that was checked in"; return 1; }
    fi
    rm -rf "$WORK/$1"
    mkdir -p "$WORK/$1"
    tar -xzf "$tarball" -C "$WORK/$1" --strip-components=1 --no-same-owner
}

# name version what command... : runs the build with its output in a log
build() {
    local name="$1" version="$2" what="$3"
    shift 3

    note "building $name $version from source ($what)"
    if ! (cd "$WORK/$name" && "$@") >"$WORK/$name-build.log" 2>&1 </dev/null; then
        tail -n 20 "$WORK/$name-build.log" >&2
        note "$name $version did not build: left out"
        FAILED=1
        return 1
    fi
    printf '%s\n' "$version" > "$PREFIX/$name.version"
    note "$name $version built into $PREFIX"
}

do_fdk() {
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
          -DBUILD_PROGRAMS=OFF -DCMAKE_INSTALL_PREFIX="$PREFIX" \
          -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON &&
    cmake --build build -j "$JOBS" &&
    cmake --install build
}

do_srt() {
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_SHARED=OFF -DENABLE_STATIC=ON \
          -DENABLE_APPS=OFF -DENABLE_TESTING=OFF -DENABLE_UNITTESTS=OFF \
          -DUSE_ENCLIB=openssl -DCMAKE_INSTALL_PREFIX="$PREFIX" \
          -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON &&
    cmake --build build -j "$JOBS" &&
    cmake --install build
}

do_rist() {
    meson setup build --buildtype=release --default-library=static -Db_staticpic=true \
          --prefix="$PREFIX" --libdir=lib \
          -Dbuiltin_mbedtls=true -Dbuiltin_cjson=true -Dbuilt_tools=false -Dtest=false &&
    ninja -C build -j "$JOBS" &&
    ninja -C build install
}

# name version pkg-config-name what function
one() {
    if have "$1" "$2" "$3"; then
        note "$1 $2 is already built in $PREFIX"
        return 0
    fi
    unpack "$1" "$2" || { FAILED=1; return 0; }
    build "$1" "$2" "$4" "$5" || true
}

one fdk-aac 2.0.3  fdk-aac "AAC, with HE-AAC"  do_fdk
one srt     1.5.4  srt     "SRT in and out"    do_srt
one librist 0.2.11 librist "RIST in and out"   do_rist

[ "$OWN_WORK" -eq 1 ] && rm -rf "$WORK"
exit "$FAILED"
