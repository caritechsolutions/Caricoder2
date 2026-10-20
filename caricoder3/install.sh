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
#   ... | sudo bash -s -- --no-technician    leave the AI technician out (no model download)
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
FDK_URL="${CARICODER3_FDK_URL:-https://codeload.github.com/mstorsjo/fdk-aac/tar.gz/refs/tags/v$FDK_VERSION}"
RUN_TESTS=0
WITH_CODECS=1
WITH_TECHNICIAN=1
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
libsrt-openssl-dev:SRT output
librist-dev:RIST output
libsqlite3-dev:the supervisor's history and event log
iputils-ping:the evidence tools' ping
"

# to stderr: shown even when the output is sent to /dev/null (the build's own output goes to logs)
say()  { printf '\033[1m==>\033[0m %s\n' "$*" >&2; }
note() { printf '    %s\n' "$*" >&2; }
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
    # The library is moved to its final place below: its pkg-config file
    # must name that place, not the scratch folder.
    sed -i "s|^prefix=.*|prefix=$LIBS_DIR|" "$WORK/fdkinst/lib/pkgconfig/fdk-aac.pc"
    printf '%s\n' "$FDK_VERSION" > "$WORK/fdkinst/fdk-aac.version"
    rm -rf "$LIBS_DIR.old"
    [ -e "$LIBS_DIR" ] && mv "$LIBS_DIR" "$LIBS_DIR.old"
    mkdir -p "$(dirname "$LIBS_DIR")"
    mv "$WORK/fdkinst" "$LIBS_DIR"
    rm -rf "$LIBS_DIR.old"
    note "fdk-aac $FDK_VERSION built into $LIBS_DIR"
}

# The technician's notes (shipped: replaced on each install; the site's
# own in /etc/caricoder3/knowledge: never touched), and in the background
# llama.cpp's server and the model (scripts/technician-setup.sh).
install_technician() {
    install -d "$PREFIX/share/caricoder3/knowledge" /etc/caricoder3/knowledge
    rm -f "$PREFIX/share/caricoder3/knowledge/"*.md
    install -m 644 "$SRC_DIR/knowledge/"*.md "$PREFIX/share/caricoder3/knowledge/"
    note "notes   $PREFIX/share/caricoder3/knowledge (yours go in /etc/caricoder3/knowledge)"
    if [ ! -e /etc/caricoder3/knowledge/README.md ]; then
        cat > /etc/caricoder3/knowledge/README.md <<'NOTES'
# Notes for the technician (this site's own)

Every .md file here is given to the technician with each incident, after
the notes that come with caricoder3. Write what you know about this site:
each IRD and what it carries, which provider runs which channel and how
to reach them, what usually goes wrong and what fixed it. Plain sentences,
one fact to a line. The installer never changes this folder.
NOTES
    fi
    local mark=/var/lib/caricoder3/technician-setup.running
    if [ -f "$mark" ] && kill -0 "$(cat "$mark" 2>/dev/null)" 2>/dev/null; then
        note "the model server and model are still being set up (/var/log/caricoder3-technician-setup.log)"
        return 0
    fi
    if [ -x "$PREFIX/bin/caricoder3-llama-server" ] && [ -f /var/lib/caricoder3/models/Qwen2.5-7B-Instruct-Q4_K_M.gguf ] &&
       grep -q "^b11200 " "$LIBS_DIR/llama-server.version" 2>/dev/null; then
        if grep -q " cpu$" "$LIBS_DIR/llama-server.version" &&
           { command -v nvcc >/dev/null 2>&1 || [ -x /usr/local/cuda/bin/nvcc ] ||
             { command -v nvidia-smi >/dev/null 2>&1 && nvidia-smi -L >/dev/null 2>&1 &&
               [ ! -f /var/lib/caricoder3/cuda-toolkit.refused ]; }; }; then
            note "an NVIDIA card is here: the CUDA toolkit is checked and the model server rebuilt for the card"
        else
            note "model server and model are installed"
            return 0
        fi
    fi
    setsid nohup bash "$SRC_DIR/scripts/technician-setup.sh" >>/var/log/caricoder3-technician-setup.log 2>&1 </dev/null &
    note "the model server is being built and the model (4.7 GB) downloaded, in the background:"
    note "  tail -f /var/log/caricoder3-technician-setup.log   (the Server page shows it too)"
}

main() {
    while [ $# -gt 0 ]; do
        case "$1" in
            --test)    RUN_TESTS=1 ;;
            --no-codecs) WITH_CODECS=0 ;;
            --no-technician) WITH_TECHNICIAN=0 ;;
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
    if [ "$WITH_CODECS" -eq 1 ] &&
       ! "$WORK/tree/caricoder3" --backends 2>/dev/null | grep -q "incl. HE-AAC"; then
        die "the build did not pick up the fdk-aac in $LIBS_DIR (no HE-AAC); nothing on this machine was changed"
    fi

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

    # The supervisor as a service: enabled and started on the first
    # install, restarted on an update so it runs the new binary (its
    # channels restart with it; a soak channel with its own unit does not).
    if command -v systemctl >/dev/null 2>&1 && [ -d /etc/systemd/system ]; then
        install -m 644 "$SRC_DIR/systemd/caricoder3.service" /etc/systemd/system/caricoder3.service
        systemctl daemon-reload
        if systemctl is-active --quiet caricoder3; then
            systemctl restart caricoder3
            note "supervisor restarted on the new binary (systemctl status caricoder3)"
        else
            systemctl enable --now caricoder3 >/dev/null 2>&1 || true
            note "supervisor started: systemctl status caricoder3"
        fi
        sleep 1
        if [ -f /etc/caricoder3/caricoder3.json ]; then
            note "GUI: http://$(hostname -I 2>/dev/null | awk '{print $1}'):$("$PREFIX/bin/caricoder3" serve --show-port 2>/dev/null || echo 8550)/  user admin, password: caricoder3 serve --show-password"
        fi
    fi

    if [ "$WITH_TECHNICIAN" -eq 1 ]; then
        say "Technician"
        install_technician
    fi

    say "Done: $("$PREFIX/bin/caricoder3" --version)"
    say "What this build can do (caricoder3 --backends)"
    "$PREFIX/bin/caricoder3" --backends 2>&1 | while IFS= read -r line; do note "$line"; done
    note ""
    note "try:  caricoder3 --help"
    note "      caricoder3 --gpu-test        (checks the NVIDIA path on card 0)"
    note "      caricoder3 serve --show-password   (the GUI password)"
}

# Everything above is only definitions. Running main from the last line
# means a download that was cut short does nothing at all.
main "$@"
