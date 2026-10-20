#!/usr/bin/env bash
#
# caricoder3 - setting up the technician: llama.cpp's server and the model
# Copyright (c) 2026 CariTech Solutions
#
# Run by the installer in the background (it takes a while: a build and a
# 4.7 GB download), or by hand:  sudo /opt/caricoder3/scripts/technician-setup.sh
# What it did is in /var/log/caricoder3-technician-setup.log.
#
#   0. With an NVIDIA card and no CUDA compiler: Ubuntu's CUDA toolkit,
#      only when apt's dry run shows the driver packages are left alone.
#   1. llama-server, built from llama.cpp at a fixed release: with CUDA
#      (for the Tesla cards, compute 6.1) when the CUDA compiler (nvcc) is
#      on this machine, otherwise for the CPU. Installed as
#      /usr/local/bin/caricoder3-llama-server. Built again only when the
#      release or the kind changes.
#   2. The model, Qwen2.5 7B Instruct at Q4_K_M (4.68 GB), downloaded once
#      to /var/lib/caricoder3/models and checked against its SHA-256.
#
# After this nothing is fetched from anywhere: the technician runs offline.
#
set -uo pipefail

LLAMA_TAG="${LLAMA_TAG:-b11200}"            # 2026-09-26
LIBS="${CARICODER3_LIBS:-/opt/caricoder3-libs}"
BIN=/usr/local/bin/caricoder3-llama-server
MODEL_DIR=/var/lib/caricoder3/models
MODEL=Qwen2.5-7B-Instruct-Q4_K_M.gguf
MODEL_URL="https://huggingface.co/bartowski/Qwen2.5-7B-Instruct-GGUF/resolve/main/$MODEL"
MODEL_SHA=65b8fcd92af6b4fefa935c625d1ac27ea29dcb6ee14589c55a8f115ceaaa1423
MARK=/var/lib/caricoder3/technician-setup.running

log() { printf '%s %s\n' "$(date '+%F %T')" "$*"; }

mkdir -p /var/lib/caricoder3 "$MODEL_DIR" "$LIBS/src"
echo $$ > "$MARK"
trap 'rm -f "$MARK"' EXIT

log "technician setup: llama.cpp $LLAMA_TAG, model $MODEL"

# ---- the CUDA compiler, for the cards ------------------------------------------------
# With an NVIDIA card and no nvcc: Ubuntu's CUDA toolkit (12.0, which a 525 or
# later driver runs), but only if apt's dry run shows it would leave every
# driver package alone (on Ubuntu 24.04 an updated 535 driver package now means
# the 580 driver: under running channels that would stop the cards until a
# reboot). Not tried again once refused; CC3_NO_CUDA_TOOLKIT=1 skips it.
REFUSED=/var/lib/caricoder3/cuda-toolkit.refused
DRIVER_PKGS='nvidia-(kernel|firmware|dkms|driver|utils|compute-utils)|libnvidia-(compute|encode|decode|gl|cfg|fbc|extra|common)|xserver-xorg-video-nvidia'
if ! command -v nvcc >/dev/null 2>&1 && [ ! -x /usr/local/cuda/bin/nvcc ] && [ -z "${CC3_NO_CUDA_TOOLKIT:-}" ] &&
   [ ! -f "$REFUSED" ] && command -v nvidia-smi >/dev/null 2>&1 && command -v apt-get >/dev/null 2>&1 &&
   drv_before="$(nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>/dev/null | head -n 1)" && [ -n "$drv_before" ]; then
    log "an NVIDIA card (driver $drv_before) and no CUDA compiler: checking whether Ubuntu's CUDA toolkit leaves the driver alone"
    sim="$(DEBIAN_FRONTEND=noninteractive apt-get install -s --no-install-recommends nvidia-cuda-toolkit nvidia-cuda-toolkit-gcc 2>&1)"
    touch_drv="$(printf '%s\n' "$sim" | grep -E '^(Inst|Remv|Conf) ' | grep -iE "$DRIVER_PKGS")"
    if ! printf '%s\n' "$sim" | grep -qE '^Inst nvidia-cuda-toolkit '; then
        log "the CUDA toolkit is not offered here (apt: $(printf '%s' "$sim" | tail -n 1)); staying on the CPU"
        printf '%s\n' "not offered" > "$REFUSED"
    elif [ -n "$touch_drv" ]; then
        log "NOT installing the CUDA toolkit: it would change the driver:"
        printf '%s\n' "$touch_drv" | sed 's/^/    /'
        log "staying on the CPU; see the README (\"The technician on a card\")"
        printf '%s\n' "$touch_drv" > "$REFUSED"
    else
        log "installing Ubuntu's CUDA toolkit (a few GB; the driver packages are not touched)"
        if DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends \
               nvidia-cuda-toolkit nvidia-cuda-toolkit-gcc </dev/null >"$LIBS/cuda-toolkit-install.log" 2>&1; then
            drv_after="$(nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>/dev/null | head -n 1)"
            if [ "$drv_after" != "$drv_before" ]; then
                log "WARNING: the driver reads '$drv_after' now (was $drv_before): check nvidia-smi"
            fi
            log "CUDA toolkit installed: $(nvcc --version 2>/dev/null | grep -o 'release [0-9.]*')"
        else
            log "the CUDA toolkit did not install (see $LIBS/cuda-toolkit-install.log); staying on the CPU"
            printf '%s\n' "install failed" > "$REFUSED"
        fi
    fi
fi

# ---- the model server --------------------------------------------------------------
NVCC=""
for c in nvcc /usr/local/cuda/bin/nvcc; do
    if command -v "$c" >/dev/null 2>&1; then NVCC="$(command -v "$c")"; break; fi
done
KIND=cpu
[ -n "$NVCC" ] && KIND=cuda
WANT="$LLAMA_TAG $KIND"

if [ -x "$BIN" ] && [ "$(cat "$LIBS/llama-server.version" 2>/dev/null)" = "$WANT" ]; then
    log "llama-server $WANT is installed already"
else
    for p in cmake git g++; do
        if ! command -v "$p" >/dev/null 2>&1; then
            log "installing $p"
            DEBIAN_FRONTEND=noninteractive apt-get install -y -q "$p" >/dev/null 2>&1 ||
                { log "ERROR: could not install $p"; exit 1; }
        fi
    done
    SRC="$LIBS/src/llama.cpp"
    if [ ! -d "$SRC/.git" ] || [ "$(git -C "$SRC" describe --tags 2>/dev/null)" != "$LLAMA_TAG" ]; then
        rm -rf "$SRC"
        log "fetching llama.cpp $LLAMA_TAG"
        git clone -q -c advice.detachedHead=false --depth 1 --branch "$LLAMA_TAG" \
            https://github.com/ggml-org/llama.cpp "$SRC" || { log "ERROR: could not fetch llama.cpp"; exit 1; }
    fi
    FLAGS=(-DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DGGML_NATIVE=ON -DLLAMA_CURL=OFF
           -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF)
    if [ "$KIND" = cuda ]; then
        FLAGS+=(-DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=61 "-DCMAKE_CUDA_COMPILER=$NVCC")
        log "building for CUDA ($NVCC) and the CPU"
    else
        log "building for the CPU (no CUDA compiler here: the cards are not used; see the README)"
    fi
    JOBS=$(( $(nproc) / 2 )); [ "$JOBS" -lt 1 ] && JOBS=1
    rm -rf "$SRC/build"
    if ! nice -n 19 cmake -S "$SRC" -B "$SRC/build" "${FLAGS[@]}" >"$LIBS/llama-build.log" 2>&1 ||
       ! nice -n 19 cmake --build "$SRC/build" --target llama-server -j "$JOBS" >>"$LIBS/llama-build.log" 2>&1; then
        tail -n 20 "$LIBS/llama-build.log"
        if [ "$KIND" = cuda ]; then
            log "the CUDA build failed; building for the CPU instead"
            KIND=cpu; WANT="$LLAMA_TAG cpu"
            FLAGS=(-DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DGGML_NATIVE=ON -DLLAMA_CURL=OFF
                   -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF)
            rm -rf "$SRC/build"
            nice -n 19 cmake -S "$SRC" -B "$SRC/build" "${FLAGS[@]}" >"$LIBS/llama-build.log" 2>&1 &&
                nice -n 19 cmake --build "$SRC/build" --target llama-server -j "$JOBS" >>"$LIBS/llama-build.log" 2>&1 ||
                { log "ERROR: llama-server did not build (see $LIBS/llama-build.log)"; exit 1; }
        else
            log "ERROR: llama-server did not build (see $LIBS/llama-build.log)"; exit 1
        fi
    fi
    install -m 755 "$SRC/build/bin/llama-server" "$BIN.new" && mv -f "$BIN.new" "$BIN"
    echo "$WANT" > "$LIBS/llama-server.version"
    log "installed $BIN ($WANT)"
fi

# ---- the model --------------------------------------------------------------------
if [ -f "$MODEL_DIR/$MODEL" ]; then
    log "the model is here already: $MODEL_DIR/$MODEL"
else
    need_kb=$(( 6 * 1024 * 1024 ))
    have_kb=$(df -Pk "$MODEL_DIR" | awk 'NR == 2 {print $4}')
    if [ "${have_kb:-0}" -lt "$need_kb" ]; then
        log "ERROR: $MODEL_DIR has $(( have_kb / 1024 )) MB free; the model needs 4.7 GB (and room besides)"
        exit 1
    fi
    log "downloading the model (4.68 GB)"
    if ! curl -fL --retry 5 --retry-delay 10 -C - -o "$MODEL_DIR/$MODEL.part" "$MODEL_URL" 2>/dev/null; then
        log "ERROR: the download did not finish; run this again to carry on where it stopped"
        exit 1
    fi
    log "checking it"
    got=$(sha256sum "$MODEL_DIR/$MODEL.part" | awk '{print $1}')
    if [ "$got" != "$MODEL_SHA" ]; then
        log "ERROR: the file is not the one expected (sha256 $got); it is removed"
        rm -f "$MODEL_DIR/$MODEL.part"
        exit 1
    fi
    mv -f "$MODEL_DIR/$MODEL.part" "$MODEL_DIR/$MODEL"
    log "model ready: $MODEL_DIR/$MODEL"
fi
log "technician setup done"
