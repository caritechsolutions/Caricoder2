#!/bin/sh
# caricoder3 - builds the card's own program (src/nv_scale.cu) into PTX and
# wraps it as src/nv_scale_ptx.h. Needs clang with the NVPTX target; the
# result is kept in the source tree, so nobody else needs it.
set -e
cd "$(dirname "$0")/.."
clang -x cuda --cuda-device-only --cuda-gpu-arch=sm_52 -nocudainc -nocudalib -O2 -S \
    -Isrc src/nv_scale.cu -o build/nv_scale.ptx
{
    echo "/* Generated from src/nv_scale.cu by scripts/make-ptx.sh: do not edit. */"
    echo "static const char nv_scale_ptx[] ="
    sed 's/\\/\\\\/g; s/"/\\"/g; s/^/    "/; s/$/\\n"/' build/nv_scale.ptx
    echo "    ;"
} > src/nv_scale_ptx.h
echo "src/nv_scale_ptx.h: $(wc -l < build/nv_scale.ptx) lines of PTX"
