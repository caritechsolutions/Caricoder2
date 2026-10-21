/*
 * caricoder3 - a picture on the card scaled on the card (nv_scale.h)
 * Copyright (c) 2026 CariTech Solutions
 */
#include "nv_scale.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_NVIDIA

#include "nv_common.h"
#include "nv_scale_ptx.h"

struct nv_scaler {
    CUcontext ctx;              /* the one it was made in (a reference held) */
    CUmodule mod;
    CUfunction fn;
    CUstream st;
    CUdeviceptr buf;
    size_t pitch;
    int w, h, bps;
};

static bool fail(char *why, size_t len, const char *what, CUresult r)
{
    snprintf(why, len, "%s: %s", what, nv_cu_err(r));
    return false;
}

static bool make(struct nv_scaler *s, CUcontext ctx, char *why, size_t len)
{
    CudaFunctions *cu = nv_cu();
    CUcontext dummy;
    CUresult r;

    s->ctx = ctx;
    nv_ctx_ref(ctx);
    cu->cuCtxPushCurrent(ctx);
    r = cu->cuModuleLoadData(&s->mod, nv_scale_ptx);
    if (r == CUDA_SUCCESS)
        r = cu->cuModuleGetFunction(&s->fn, s->mod, "cc3_scale");
    if (r == CUDA_SUCCESS)
        r = cu->cuStreamCreate(&s->st, 1 /* CU_STREAM_NON_BLOCKING */);
    cu->cuCtxPopCurrent(&dummy);
    return r == CUDA_SUCCESS || fail(why, len, "the scaling program would not load", r);
}

static bool launch(struct nv_scaler *s, CUdeviceptr src, int spitch, int sw, int sh, CUdeviceptr dst,
                   int dpitch, int dw, int dh, int comps, int wide, int blend)
{
    CudaFunctions *cu = nv_cu();
    void *args[] = { &src, &spitch, &sw, &sh, &dst, &dpitch, &dw, &dh, &comps, &wide, &blend };

    return cu->cuLaunchKernel(s->fn, (unsigned)(dw + 15) / 16, (unsigned)(dh + 15) / 16, 1, 16, 16, 1, 0,
                              s->st, args, NULL) == CUDA_SUCCESS;
}

bool nv_scale(struct nv_scaler **sp, const struct vframe *in, int dw, int dh, struct vframe *out,
              char *why, size_t len)
{
    CudaFunctions *cu = nv_cu();
    struct nv_scaler *s = *sp;
    int bps = in->depth > 8 ? 2 : 1;
    int blend = in->interlaced ? 1 : 0;
    int spitch = in->stride[0], dpitch;
    CUdeviceptr src = (CUdeviceptr)(uintptr_t)in->plane[0];
    CUcontext dummy;
    CUresult r = CUDA_SUCCESS;
    bool ok;

    if (in->mem != VMEM_CUDA || !in->cuda_ctx || !cu || dw < 16 || dh < 16 || (dw & 1) || (dh & 1)) {
        snprintf(why, len, "nothing the card can scale");
        return false;
    }
    if (s && s->ctx != in->cuda_ctx) {
        nv_scaler_free(s);
        s = *sp = NULL;
    }
    if (!s) {
        s = calloc(1, sizeof(*s));
        if (!s) {
            snprintf(why, len, "out of memory");
            return false;
        }
        if (!make(s, in->cuda_ctx, why, len)) {
            nv_scaler_free(s);
            return false;
        }
        *sp = s;
    }
    cu->cuCtxPushCurrent(s->ctx);
    if (!s->buf || s->w != dw || s->h != dh || s->bps != bps) {
        if (s->buf)
            cu->cuMemFree(s->buf);
        s->buf = 0;
        r = cu->cuMemAllocPitch(&s->buf, &s->pitch, (size_t)dw * bps, (size_t)dh * 3 / 2, 16);
        if (r != CUDA_SUCCESS) {
            s->buf = 0;
            cu->cuCtxPopCurrent(&dummy);
            return fail(why, len, "no card memory for the scaled picture", r);
        }
        s->w = dw;
        s->h = dh;
        s->bps = bps;
    }
    dpitch = (int)s->pitch;
    ok = launch(s, src, spitch, in->width, in->height, s->buf, dpitch, dw, dh, 1, bps == 2, blend) &&
         launch(s, src + (CUdeviceptr)spitch * in->height, spitch, in->width / 2, in->height / 2,
                s->buf + (CUdeviceptr)dpitch * dh, dpitch, dw / 2, dh / 2, 2, bps == 2, blend);
    if (ok)
        r = cu->cuStreamSynchronize(s->st);
    cu->cuCtxPopCurrent(&dummy);
    if (!ok || r != CUDA_SUCCESS)
        return fail(why, len, "scaling on the card failed", ok ? r : CUDA_ERROR_UNKNOWN);

    *out = *in;
    out->width = dw;
    out->height = dh;
    out->plane[0] = (uint8_t *)(uintptr_t)s->buf;
    out->stride[0] = dpitch;
    out->interlaced = false;
    /* the shape of a pixel follows the picture: the display shape stays */
    if (in->sar_num > 0 && in->sar_den > 0) {
        long long n = (long long)in->sar_num * in->width * dh, d = (long long)in->sar_den * in->height * dw;
        long long a = n, b = d;

        while (b) {
            long long t = a % b;

            a = b;
            b = t;
        }
        if (a > 0 && n / a < 65536 && d / a < 65536) {
            out->sar_num = (int)(n / a);
            out->sar_den = (int)(d / a);
        }
    }
    return true;
}

void nv_scaler_free(struct nv_scaler *s)
{
    CudaFunctions *cu = nv_cu();
    CUcontext dummy;

    if (!s)
        return;
    if (s->ctx && cu) {
        cu->cuCtxPushCurrent(s->ctx);
        if (s->buf)
            cu->cuMemFree(s->buf);
        if (s->st)
            cu->cuStreamDestroy(s->st);
        if (s->mod)
            cu->cuModuleUnload(s->mod);
        cu->cuCtxPopCurrent(&dummy);
        nv_ctx_put(s->ctx);
    }
    free(s);
}

#else

bool nv_scale(struct nv_scaler **s, const struct vframe *in, int dw, int dh, struct vframe *out,
              char *why, size_t len)
{
    (void)s; (void)in; (void)dw; (void)dh; (void)out;
    snprintf(why, len, "built without NVIDIA support");
    return false;
}

void nv_scaler_free(struct nv_scaler *s)
{
    (void)s;
}

#endif
