/*
 * caricoder3 - a little H.264 syntax: NAL units and the SPS
 * Copyright (c) 2026 CariTech Solutions
 */
#ifndef CC3_H264_H
#define CC3_H264_H

#include <stddef.h>
#include <stdint.h>

/* Width, height and frame rate (0 when the VUI has none) from an SPS NAL
 * (nal[0] is the NAL header byte). */
void h264_parse_sps(const uint8_t *nal, int len, int *w, int *h, double *fps);

/* Calls fn for every NAL unit in Annex B data (type = nal_unit_type). */
void h264_each_nal(const uint8_t *d, size_t len,
                   void (*fn)(void *ctx, unsigned type, const uint8_t *nal, int nlen), void *ctx);

#endif
