/*
 * caricoder3 - NVIDIA: what the decoder and the encoder share
 * Copyright (c) 2026 CariTech Solutions
 *
 * The NVIDIA libraries (libcuda, libnvcuvid, libnvidia-encode) come with
 * the driver. They are opened at run time, not linked, so the same binary
 * starts on a machine with no NVIDIA card: there the backends report
 * themselves unavailable and that is all.
 *
 * The declarations in third_party/ffnvcodec are NVIDIA's own Video Codec
 * SDK headers as repackaged by the nv-codec-headers project (MIT licence).
 * API version 12.1: needs driver 530.41.03 or newer.
 */
#ifndef CC3_NV_COMMON_H
#define CC3_NV_COMMON_H
#ifdef HAVE_NVIDIA

#include <stdbool.h>
#include <stddef.h>

/* The loader would print to stderr when a library is missing. Finding out
 * whether it is missing is our job (probe), and no cause for noise. */
#define FFNV_LOG_FUNC(logctx, msg, ...)       do { (void)(logctx); } while (0)
#define FFNV_DEBUG_LOG_FUNC(logctx, msg, ...) do { (void)(logctx); } while (0)

#include "../third_party/ffnvcodec/dynlink_loader.h"

/* Loads the three libraries and starts CUDA, once. False with a reason. */
bool nv_load(char *why, size_t len);

CudaFunctions *nv_cu(void);
CuvidFunctions *nv_cv(void);
NvencFunctions *nv_ne(void);

/* Number of cards, after nv_load(). */
int nv_device_count(void);

/*
 * The CUDA context of card `gpu`. One per card for the whole process,
 * shared by everything that works on that card, so that a picture decoded
 * there can be handed to the encoder without leaving it. Counted: every
 * get needs a put.
 */
CUcontext nv_ctx_get(int gpu, char *why, size_t len);
void nv_ctx_put(CUcontext ctx);
/* One more user of a context somebody else got: it lives until every user
 * has put it back, whoever goes first. */
void nv_ctx_ref(CUcontext ctx);

const char *nv_cu_err(CUresult r);

/* For the listing and for --gpu-test. */
bool nv_probe(char *why, size_t len);
bool nv_device_name(int gpu, char *name, size_t len, size_t *mem_bytes);

#endif
#endif
