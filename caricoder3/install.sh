#!/usr/bin/env bash
#
# caricoder3 installer
# Copyright (c) 2026 CariTech Solutions
#
#   curl -fsSL "https://raw.githubusercontent.com/caritechsolutions/Caricoder2/claude/caricoder3-stage1/caricoder3/install.sh?$(date +%s)" | sudo bash
#
# The "?$(date +%s)" matters: raw.githubusercontent.com caches files for a
# few minutes, and without it you can be handed the previous version.
#
# Options go after "bash -s --":
#
#   ... | sudo bash -s -- --test             also run the self-tests
#   ... | sudo bash -s -- --branch main      install from another branch
#
# What it does, in this order:
#   1. installs gcc and make if they are missing (apt)
#   2. downloads the source (no git needed) and builds it in a scratch copy
#   3. only if the build succeeded: puts the source in /opt/caricoder3 and
#      the binary in /usr/local/bin/caricoder3
#
# What it does not do: it starts nothing, stops nothing and creates no
# services. Channels that are already running keep running on the binary
# they started with until you restart them. Running it again upgrades.
#
set -euo pipefail

REPO="caritechsolutions/Caricoder2"
BRANCH="${CARICODER3_BRANCH:-claude/caricoder3-stage1}"
SRC_DIR="${CARICODER3_SRC:-/opt/caricoder3}"
PREFIX="${PREFIX:-/usr/local}"
RUN_TESTS=0
WORK=""

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

main() {
    while [ $# -gt 0 ]; do
        case "$1" in
            --test)    RUN_TESTS=1 ;;
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
    make -C "$WORK/tree" clean >/dev/null 2>&1 || true
    if ! make -C "$WORK/tree" >"$WORK/build.log" 2>&1 </dev/null; then
        tail -n 30 "$WORK/build.log" >&2
        die "build failed; nothing on this machine was changed"
    fi
    local version
    version="$("$WORK/tree/caricoder3" --version)" ||
        die "the new binary does not run; nothing on this machine was changed"
    note "$version"

    if [ "$RUN_TESTS" -eq 1 ]; then
        say "Self-tests (file tests only: no network ports are used)"
        if command -v python3 >/dev/null 2>&1; then
            if ! (cd "$WORK/tree" && python3 tests/run_tests.py file </dev/null); then
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
    note "try:  caricoder3 --help"
}

# Everything above is only definitions. Running main from the last line
# means a download that was cut short does nothing at all.
main "$@"
