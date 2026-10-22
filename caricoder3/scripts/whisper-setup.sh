#!/usr/bin/env bash
#
# caricoder3 - setting up the ears: whisper.cpp and its model
# Copyright (c) 2026 CariTech Solutions
#
# Run by the installer in the background, or by hand:
#   sudo /opt/caricoder3/scripts/whisper-setup.sh
# What it did is in /var/log/caricoder3-whisper-setup.log.
#
#   1. whisper-cli, built from whisper.cpp at a fixed release for the CPU
#      (a clip of 20 s takes a second or two on a few cores; the cards stay
#      the channels'), installed as /usr/local/bin/caricoder3-whisper-cli.
#      Built again only when the release changes.
#   2. The model, Whisper "base" (multilingual, 142 MiB), downloaded once to
#      /var/lib/caricoder3/models/whisper and checked against its SHA-1 (the
#      one whisper.cpp's model list gives).
#
# caricoder3 listen uses them to say which language a channel speaks.
# After this nothing is fetched from anywhere: it runs offline.
#
set -uo pipefail

WHISPER_TAG="${WHISPER_TAG:-v1.8.7}"
LIBS="${CARICODER3_LIBS:-/opt/caricoder3-libs}"
BIN=/usr/local/bin/caricoder3-whisper-cli
MODEL_DIR=/var/lib/caricoder3/models/whisper
MODEL=ggml-base.bin
MODEL_URL="https://huggingface.co/ggerganov/whisper.cpp/resolve/main/$MODEL"
MODEL_SHA1=465707469ff3a37a2b9b8d8f89f2f99de7299dac
MARK=/var/lib/caricoder3/whisper-setup.running

log() { printf '%s %s\n' "$(date '+%F %T')" "$*"; }

mkdir -p /var/lib/caricoder3 "$MODEL_DIR" "$LIBS/src"
echo $$ > "$MARK"
trap 'rm -f "$MARK"' EXIT

log "whisper setup: whisper.cpp $WHISPER_TAG, model $MODEL"

if [ -x "$BIN" ] && [ "$(cat "$LIBS/whisper-cli.version" 2>/dev/null)" = "$WHISPER_TAG" ]; then
    log "whisper-cli $WHISPER_TAG is installed already"
else
    for p in cmake git g++; do
        if ! command -v "$p" >/dev/null 2>&1; then
            log "installing $p"
            DEBIAN_FRONTEND=noninteractive apt-get install -y -q "$p" >/dev/null 2>&1 ||
                { log "ERROR: could not install $p"; exit 1; }
        fi
    done
    SRC="$LIBS/src/whisper.cpp"
    if [ ! -d "$SRC/.git" ] || [ "$(git -C "$SRC" describe --tags 2>/dev/null)" != "$WHISPER_TAG" ]; then
        rm -rf "$SRC"
        log "fetching whisper.cpp $WHISPER_TAG"
        git clone -q -c advice.detachedHead=false --depth 1 --branch "$WHISPER_TAG" \
            https://github.com/ggml-org/whisper.cpp "$SRC" || { log "ERROR: could not fetch whisper.cpp"; exit 1; }
    fi
    JOBS=$(( $(nproc) / 2 )); [ "$JOBS" -lt 1 ] && JOBS=1
    rm -rf "$SRC/build"
    log "building whisper-cli for the CPU"
    if ! nice -n 19 cmake -S "$SRC" -B "$SRC/build" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
             -DGGML_NATIVE=ON -DWHISPER_BUILD_TESTS=OFF -DWHISPER_BUILD_SERVER=OFF >"$LIBS/whisper-build.log" 2>&1 ||
       ! nice -n 19 cmake --build "$SRC/build" --target whisper-cli -j "$JOBS" >>"$LIBS/whisper-build.log" 2>&1; then
        tail -n 20 "$LIBS/whisper-build.log"
        log "ERROR: whisper-cli did not build (see $LIBS/whisper-build.log)"
        exit 1
    fi
    install -m 755 "$SRC/build/bin/whisper-cli" "$BIN.new" && mv -f "$BIN.new" "$BIN"
    echo "$WHISPER_TAG" > "$LIBS/whisper-cli.version"
    log "installed $BIN ($WHISPER_TAG)"
    rm -rf "$SRC/build"
fi

if [ -f "$MODEL_DIR/$MODEL" ]; then
    log "the model is here already: $MODEL_DIR/$MODEL"
else
    log "downloading the model (142 MiB)"
    if ! curl -fL --retry 5 --retry-delay 10 -C - -o "$MODEL_DIR/$MODEL.part" "$MODEL_URL" 2>/dev/null; then
        log "ERROR: the download did not finish; run this again to carry on where it stopped"
        exit 1
    fi
    got=$(sha1sum "$MODEL_DIR/$MODEL.part" | awk '{print $1}')
    if [ "$got" != "$MODEL_SHA1" ]; then
        log "ERROR: the file is not the one expected (sha1 $got); it is removed"
        rm -f "$MODEL_DIR/$MODEL.part"
        exit 1
    fi
    mv -f "$MODEL_DIR/$MODEL.part" "$MODEL_DIR/$MODEL"
    log "model ready: $MODEL_DIR/$MODEL"
fi
command -v ffmpeg >/dev/null 2>&1 || {
    log "installing ffmpeg (to take the sound out of a clip)"
    DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends ffmpeg >/dev/null 2>&1 ||
        log "ERROR: could not install ffmpeg"
}
log "whisper setup done"
