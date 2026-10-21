/*
 * caricoder3 - a picture on the card scaled on the card (nv_scale.cu)
 * Copyright (c) 2026 CariTech Solutions
 *
 * For renditions that share one decoder (card.c): the decoder makes the
 * picture once, at the first rendition's size, and every other rendition
 * scales it to its own on the card, where the encoder takes it from.
 */
#ifndef CC3_NV_SCALE_H
#define CC3_NV_SCALE_H

#include <stdbool.h>
#include <stddef.h>

#include "codec.h"

struct nv_scaler;

/* `in` (VMEM_CUDA, NV12 or P010) at dw x dh into *out, which lives in the
 * scaler's own buffer until its next use. Interlaced pictures come out
 * progressive (field blend). The scaler is made on first use (*s NULL).
 * False with a reason. */
bool nv_scale(struct nv_scaler **s, const struct vframe *in, int dw, int dh, struct vframe *out,
              char *why, size_t len);
void nv_scaler_free(struct nv_scaler *s);

#endif
