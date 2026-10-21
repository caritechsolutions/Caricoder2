/*
 * caricoder3 - the card's own program: scaling a picture (see nv_scale_px.h)
 * Copyright (c) 2026 CariTech Solutions
 *
 * Built by clang into PTX, which the driver compiles for whatever card it
 * meets (make ptx; the result is kept in src/nv_scale_ptx.h, so building
 * caricoder3 needs neither clang's CUDA support nor NVIDIA's toolkit):
 *
 *   clang -x cuda --cuda-device-only --cuda-gpu-arch=sm_52 -nocudainc -nocudalib -O2 -S
 */
#define CC3_PX_FN static __attribute__((device)) inline
#include "nv_scale_px.h"

extern "C" __attribute__((global)) void cc3_scale(const unsigned char *src, int spitch, int sw, int sh,
                                                  unsigned char *dst, int dpitch, int dw, int dh,
                                                  int comps, int wide, int blend)
{
    int ox = __nvvm_read_ptx_sreg_ctaid_x() * __nvvm_read_ptx_sreg_ntid_x() + __nvvm_read_ptx_sreg_tid_x();
    int oy = __nvvm_read_ptx_sreg_ctaid_y() * __nvvm_read_ptx_sreg_ntid_y() + __nvvm_read_ptx_sreg_tid_y();

    cc3_scale_px(src, spitch, sw, sh, dst, dpitch, dw, dh, comps, wide, blend, ox, oy);
}
