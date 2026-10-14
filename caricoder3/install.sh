#!/usr/bin/env bash
#
# caricoder3 installer
# Copyright (c) 2026 CariTech Solutions
#
#   curl -fsSL "https://raw.githubusercontent.com/caritechsolutions/Caricoder2/claude/caricoder3-stage2/caricoder3/install.sh?$(date +%s)" | sudo bash
#
# The "?$(date +%s)" matters: raw.githubusercontent.com caches files for a
# few minutes, and without it you can be handed the previous version.
#
# Options go after "bash -s --":
#
#   ... | sudo bash -s -- --test             also run the self-tests
#   ... | sudo bash -s -- --branch main      install from another branch
#   ... | sudo bash -s -- --no-codecs        build the plain remultiplexer only
#
# What it does, in this order:
#   1. installs gcc and make if they are missing (apt)
#   2. installs the codec libraries that transcoding needs (apt). One that
#      cannot be installed is reported and left out: the program is built
#      with the others
#   3. downloads the source (no git needed) and builds it in a scratch copy
#   4. only if the build succeeded: puts the source in /opt/caricoder3 and
#      the binary in /usr/local/bin/caricoder3
#
# NVIDIA needs nothing installed here: the driver's own libraries are used
# when the program runs.
#
# What it does not do: it starts nothing, stops nothing and creates no
# services. Channels that are already running keep running on the binary
# they started with until you restart them. Running it again upgrades.
#
set -euo pipefail

REPO="caritechsolutions/Caricoder2"
BRANCH="${CARICODER3_BRANCH:-claude/caricoder3-stage2}"
SRC_DIR="${CARICODER3_SRC:-/opt/caricoder3}"
PREFIX="${PREFIX:-/usr/local}"
# Libraries built from source go here (the Makefile looks for them).
LIBS_DIR="${CARICODER3_LIBS:-/opt/caricoder3-libs}"
# The original fdk-aac. Ubuntu's package is "fdk-aac-free", with the
# HE-AAC parts removed, and the feeds this is for carry HE-AAC.
FDK_VERSION="2.0.3"
FDK_URL="https://codeload.github.com/mstorsjo/fdk-aac/tar.gz/refs/tags/v$FDK_VERSION"
RUN_TESTS=0
WITH_CODECS=1
WORK=""

# package : what it is for
CODEC_PKGS="
pkg-config:finding the libraries below
libmpg123-dev:MP2 / MP3 decode
liba52-0.7.4-dev:AC-3 decode
libtwolame-dev:MP2 encode
libmp3lame-dev:MP3 encode
libavcodec-dev:H.264 / H.265 decode on the CPU
libavutil-dev:(needed by libavcodec)
libx264-dev:H.264 encode on the CPU
libx265-dev:H.265 encode on the CPU
"

say()  { printf '\033[1m==>\033[0m %s\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die()  { printf '\033[1;31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }

cleanup() {
    [ -n "$WORK" ] && rm -rf "$WORK"
    return 0
}

usage() {
    cat <<'USAGE'
caricoder3 installer

  curl -fsSL "<raw URL of this script>?$(date +%s)" | sudo bash
  curl -fsSL "<raw URL of this script>?$(date +%s)" | sudo bash -s -- [options]

  --test           also run the self-tests before installing (needs python3)
  --branch NAME    install from another branch
  --no-codecs      do not install codec libraries; build the remultiplexer only
  -h, --help       this text

Builds in a scratch folder first. Source goes to /opt/caricoder3, the binary
to /usr/local/bin/caricoder3. Starts nothing, stops nothing.
USAGE
}

install_build_tools() {
    local missing=()

    command -v gcc  >/dev/null 2>&1 || missing+=(gcc)
    command -v make >/dev/null 2>&1 || missing+=(make)
    command -v tar  >/dev/null 2>&1 || missing+=(tar)
    command -v gzip >/dev/null 2>&1 || missing+=(gzip)
    [ -e /usr/include/stdio.h ]     || missing+=(libc6-dev)

    if [ "${#missing[@]}" -eq 0 ]; then
        note "gcc and make are already here"
        return 0
    fi
    command -v apt-get >/dev/null 2>&1 ||
        die "need ${missing[*]} and this is not an apt system; install them and run again"

    note "installing: ${missing[*]}"
    # </dev/null: this script arrives on stdin, apt must not read from it.
    DEBIAN_FRONTEND=noninteractive apt-get update -qq </dev/null >/dev/null 2>&1 ||
        note "apt-get update reported a problem; carrying on with the lists we have"
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
        "${missing[@]}" </dev/null >/dev/null ||
        die "could not install ${missing[*]}"
}

install_codec_libs() {
    local line pkg what missing=() failed=()

    command -v apt-get >/dev/null 2>&1 || {
        note "not an apt system: install the codec libraries yourself (see README)"
        return 0
    }
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        pkg="${line%%:*}"
        dpkg -s "$pkg" >/dev/null 2>&1 || missing+=("$line")
    done <<< "$CODEC_PKGS"
    if [ "${#missing[@]}" -eq 0 ]; then
        note "all codec libraries are already here"
        return 0
    fi
    DEBIAN_FRONTEND=noninteractive apt-get update -qq </dev/null >/dev/null 2>&1 ||
        note "apt-get update reported a problem; carrying on with the lists we have"
    # One at a time: a package this system does not offer must not stop
    # the others from being installed.
    for line in "${missing[@]}"; do
        pkg="${line%%:*}"
        what="${line#*:}"
        if DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
               "$pkg" </dev/null >/dev/null 2>&1; then
            note "installed $pkg ($what)"
        else
            failed+=("$pkg: $what")
        fi
    done
    if [ "${#failed[@]}" -gt 0 ]; then
        note "could NOT be installed, and will be missing from the build:"
        for line in "${failed[@]}"; do note "    $line"; done
    fi
}

# Builds the original fdk-aac into $LIBS_DIR (static, so the binary
# carries it). Done once; again only when the version here changes.
install_fdk_aac() {
    local stamp="$LIBS_DIR/fdk-aac.version" tarball="$WORK/fdk.tar.gz"

    if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$FDK_VERSION" ] &&
       [ -f "$LIBS_DIR/lib/pkgconfig/fdk-aac.pc" ]; then
        note "fdk-aac $FDK_VERSION (with HE-AAC) is already built in $LIBS_DIR"
        return 0
    fi
    if ! command -v cmake >/dev/null 2>&1 || ! command -v c++ >/dev/null 2>&1; then
        command -v apt-get >/dev/null 2>&1 ||
            die "cmake and a C++ compiler are needed to build fdk-aac; install them and run again"
        note "installing cmake and g++"
        DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
            cmake g++ </dev/null >/dev/null || die "could not install cmake and g++"
    fi
    note "building fdk-aac $FDK_VERSION from source (the Ubuntu package has no HE-AAC)"
    curl -fsSL --retry 3 -o "$tarball" "$FDK_URL" </dev/null ||
        die "could not download fdk-aac $FDK_VERSION"
    mkdir -p "$WORK/fdk"
    tar -xzf "$tarball" -C "$WORK/fdk" --strip-components=1 --no-same-owner ||
        die "the fdk-aac download is not a tar archive"
    if ! (cd "$WORK/fdk" &&
          cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
                -DBUILD_PROGRAMS=OFF -DCMAKE_INSTALL_PREFIX="$WORK/fdkinst" \
                -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON &&
          cmake --build build -j "$(nproc)" &&
          cmake --install build) >"$WORK/fdk-build.log" 2>&1 </dev/null; then
        tail -n 20 "$WORK/fdk-build.log" >&2
        die "fdk-aac did not build; nothing on this machine was changed"
    fi
    [ -f "$WORK/fdkinst/lib/pkgconfig/fdk-aac.pc" ] || die "fdk-aac built but left no library"
    printf '%s\n' "$FDK_VERSION" > "$WORK/fdkinst/fdk-aac.version"
    rm -rf "$LIBS_DIR.old"
    [ -e "$LIBS_DIR" ] && mv "$LIBS_DIR" "$LIBS_DIR.old"
    mkdir -p "$(dirname "$LIBS_DIR")"
    mv "$WORK/fdkinst" "$LIBS_DIR"
    rm -rf "$LIBS_DIR.old"
    note "fdk-aac $FDK_VERSION built into $LIBS_DIR"
}

main() {
    while [ $# -gt 0 ]; do
        case "$1" in
            --test)    RUN_TESTS=1 ;;
            --no-codecs) WITH_CODECS=0 ;;
            --branch)  [ $# -ge 2 ] || die "--branch needs a name"
                       BRANCH="$2"; shift ;;
            -h|--help) usage; exit 0 ;;
            *)         die "unknown option: $1" ;;
        esac
        shift
    done

    [ "$(id -u)" -eq 0 ] || die "run this with sudo"
    command -v curl >/dev/null 2>&1 || die "curl is needed"
    case "$SRC_DIR" in
        /*/*) ;;
        *) die "refusing to use '$SRC_DIR' as the source folder" ;;
    esac
    umask 022

    say "caricoder3 installer (branch: $BRANCH)"

    say "Build tools"
    install_build_tools

    # Scratch space next to the destination: same disk, so the finished
    # tree can be moved into place in one step, and never a noexec /tmp.
    mkdir -p "$(dirname "$SRC_DIR")"
    WORK="$(mktemp -d "$(dirname "$SRC_DIR")/.caricoder3-install.XXXXXX")"
    trap cleanup EXIT

    if [ "$WITH_CODECS" -eq 1 ]; then
        say "Codec libraries"
        install_codec_libs
        install_fdk_aac
    fi

    say "Downloading the source"
    local tarball="$WORK/src.tar.gz"
    curl -fsSL --retry 3 -o "$tarball" \
        "https://codeload.github.com/$REPO/tar.gz/refs/heads/$BRANCH" </dev/null ||
        die "download failed (is the branch name '$BRANCH' right?)"

    # GitHub writes the commit id into the archive header.
    local commit
    commit="$(gzip -dc "$tarball" 2>/dev/null | head -c 4096 |
              grep -a -o 'comment=[0-9a-f]\{40\}' | head -n 1 | cut -d= -f2 || true)"
    note "commit ${commit:-unknown}"

    mkdir "$WORK/tree"
    tar -xzf "$tarball" -C "$WORK/tree" --strip-components=2 \
        --no-same-owner --no-same-permissions \
        --wildcards '*/caricoder3/*' ||
        die "this branch has no caricoder3 folder"
    [ -f "$WORK/tree/Makefile" ] && [ -f "$WORK/tree/src/main.c" ] ||
        die "the download does not look like caricoder3"
    printf '%s\n' "${commit:-unknown}" > "$WORK/tree/COMMIT"

    say "Building"
    local make_args=("LIBS_PREFIX=$LIBS_DIR")
    [ "$WITH_CODECS" -eq 1 ] || make_args+=(CODECS=none)
    make -C "$WORK/tree" clean >/dev/null 2>&1 || true
    if ! make -C "$WORK/tree" ${make_args[@]+"${make_args[@]}"} >"$WORK/build.log" 2>&1 </dev/null; then
        tail -n 30 "$WORK/build.log" >&2
        die "build failed; nothing on this machine was changed"
    fi
    local version
    version="$("$WORK/tree/caricoder3" --version)" ||
        die "the new binary does not run; nothing on this machine was changed"
    note "$version"

    if [ "$RUN_TESTS" -eq 1 ]; then
        say "Self-tests (file tests only: no network ports are used)"
        note "the transcoding tests need ffmpeg and python3-numpy and skip themselves without"
        if command -v python3 >/dev/null 2>&1; then
            if ! (cd "$WORK/tree" && CC3_FDK_FULL="$LIBS_DIR" python3 tests/run_tests.py file xc_ </dev/null); then
                die "self-tests failed; nothing on this machine was changed"
            fi
            rm -rf "$WORK/tree/build/test-work"
            find "$WORK/tree" -name __pycache__ -type d -prune -exec rm -rf {} +
        else
            note "python3 is not installed: skipped"
        fi
    fi

    say "Installing"
    # Source: swap the folder in one move, so a half-copied tree never exists.
    rm -rf "$SRC_DIR.old"
    if [ -e "$SRC_DIR" ]; then
        mv "$SRC_DIR" "$SRC_DIR.old"
    fi
    mv "$WORK/tree" "$SRC_DIR"
    chmod 755 "$SRC_DIR"
    rm -rf "$SRC_DIR.old"
    note "source  $SRC_DIR"

    # Binary: write beside the target, then rename over it. Running
    # channels keep the old file open and are not disturbed.
    install -d "$PREFIX/bin"
    install -m 755 "$SRC_DIR/caricoder3" "$PREFIX/bin/.caricoder3.new"
    mv -f "$PREFIX/bin/.caricoder3.new" "$PREFIX/bin/caricoder3"
    note "binary  $PREFIX/bin/caricoder3"

    local running
    running="$(pgrep -x caricoder3 2>/dev/null | wc -l || true)"
    if [ "${running:-0}" -gt 0 ]; then
        note "$running channel(s) are running on the previous binary until restarted"
    fi

    say "Done: $("$PREFIX/bin/caricoder3" --version)"
    say "What this build can do (caricoder3 --backends)"
    "$PREFIX/bin/caricoder3" --backends 2>&1 | while IFS= read -r line; do note "$line"; done
    note ""
    note "try:  caricoder3 --help"
    note "      caricoder3 --gpu-test        (checks the NVIDIA path on card 0)"
}

# Everything above is only definitions. Running main from the last line
# means a download that was cut short does nothing at all.
main "$@"
