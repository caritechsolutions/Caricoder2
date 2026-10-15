/*
 * caricoder3 - a little H.264 syntax: NAL units and the SPS
 * Copyright (c) 2026 CariTech Solutions
 */
#include "h264.h"

#include <stdbool.h>
#include <string.h>


/* Exp-Golomb reader over an RBSP (emulation prevention removed). */
struct bits { const uint8_t *p; size_t len; size_t pos; };

static unsigned bit(struct bits *b)
{
    if (b->pos >= b->len * 8)
        return 0;
    unsigned v = (b->p[b->pos / 8] >> (7 - b->pos % 8)) & 1;

    b->pos++;
    return v;
}

static unsigned bits_n(struct bits *b, int n)
{
    unsigned v = 0;

    while (n-- > 0)
        v = (v << 1) | bit(b);
    return v;
}

static unsigned ue(struct bits *b)
{
    int zeros = 0;

    while (bit(b) == 0 && zeros < 31)
        zeros++;
    return ((1u << zeros) - 1) + bits_n(b, zeros);
}

static int se(struct bits *b)
{
    unsigned v = ue(b);

    return (v & 1) ? (int)((v + 1) / 2) : -(int)(v / 2);
}

/* Width, height and frame rate (if the VUI has it) from an SPS NAL. */
void h264_parse_sps(const uint8_t *nal, int len, int *w, int *h, double *fps)
{
    uint8_t rbsp[1024];
    int n = 0;
    struct bits b;
    unsigned profile, chroma = 1, poc_type;
    int frame_mbs_only, crop_l = 0, crop_r = 0, crop_t = 0, crop_b = 0;
    unsigned pic_w_mbs, pic_h_map;

    for (int i = 1; i < len && n < (int)sizeof(rbsp); i++) {
        if (i + 2 < len && nal[i] == 0 && nal[i + 1] == 0 && nal[i + 2] == 3) {
            if (n + 1 >= (int)sizeof(rbsp))
                break;
            rbsp[n++] = 0;
            rbsp[n++] = 0;
            i += 2;
            continue;
        }
        rbsp[n++] = nal[i];
    }
    b.p = rbsp; b.len = (size_t)n; b.pos = 0;
    profile = bits_n(&b, 8);
    bits_n(&b, 8);                          /* constraints */
    bits_n(&b, 8);                          /* level */
    ue(&b);                                 /* sps id */
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
        profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 ||
        profile == 139 || profile == 134 || profile == 135) {
        chroma = ue(&b);
        if (chroma == 3)
            bit(&b);
        ue(&b); ue(&b); bit(&b);
        if (bit(&b)) {                      /* scaling matrix */
            for (int i = 0; i < (chroma != 3 ? 8 : 12); i++)
                if (bit(&b)) {
                    int size = i < 6 ? 16 : 64, last = 8, next = 8;

                    for (int j = 0; j < size; j++) {
                        if (next)
                            next = (last + se(&b) + 256) % 256;
                        last = next ? next : last;
                    }
                }
        }
    }
    ue(&b);                                 /* log2_max_frame_num */
    poc_type = ue(&b);
    if (poc_type == 0) {
        ue(&b);
    } else if (poc_type == 1) {
        bit(&b); se(&b); se(&b);
        unsigned k = ue(&b);

        for (unsigned i = 0; i < k && i < 256; i++)
            se(&b);
    }
    ue(&b);                                 /* max_num_ref_frames */
    bit(&b);
    pic_w_mbs = ue(&b) + 1;
    pic_h_map = ue(&b) + 1;
    frame_mbs_only = (int)bit(&b);
    if (!frame_mbs_only)
        bit(&b);
    bit(&b);
    if (bit(&b)) {                          /* cropping */
        crop_l = (int)ue(&b); crop_r = (int)ue(&b); crop_t = (int)ue(&b); crop_b = (int)ue(&b);
    }
    {
        int sub_w = chroma == 1 || chroma == 2 ? 2 : 1;
        int sub_h = chroma == 1 ? 2 : 1;
        int unit_y = sub_h * (2 - frame_mbs_only);

        *w = (int)(pic_w_mbs * 16) - sub_w * (crop_l + crop_r);
        *h = (int)((2 - frame_mbs_only) * pic_h_map * 16) - unit_y * (crop_t + crop_b);
    }
    *fps = 0;
    if (bit(&b)) {                          /* VUI */
        if (bit(&b)) {                      /* aspect ratio */
            if (bits_n(&b, 8) == 255)
                bits_n(&b, 32);
        }
        if (bit(&b))
            bit(&b);                        /* overscan */
        if (bit(&b)) {                      /* video signal type */
            bits_n(&b, 4);
            if (bit(&b))
                bits_n(&b, 24);
        }
        if (bit(&b)) {                      /* chroma loc */
            ue(&b); ue(&b);
        }
        if (bit(&b)) {                      /* timing */
            unsigned num = bits_n(&b, 32), den = bits_n(&b, 32);
            int fixed = (int)bit(&b);

            (void)fixed;
            if (num)
                *fps = (double)den / (2.0 * num);
        }
    }
}

/* Finds NAL units in Annex B data: calls fn(type, nal, len) for each. */
void h264_each_nal(const uint8_t *d, size_t len,
                     void (*fn)(void *ctx, unsigned type, const uint8_t *nal, int nlen), void *ctx)
{
    size_t i = 0, start = 0;
    bool in = false;

    while (i + 3 <= len) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            if (in) {
                size_t end = i;

                while (end > start && d[end - 1] == 0)
                    end--;
                if (end > start)
                    fn(ctx, d[start] & 0x1F, d + start, (int)(end - start));
            }
            start = i + 3;
            in = true;
            i += 3;
        } else {
            i++;
        }
    }
    if (in && len > start)
        fn(ctx, d[start] & 0x1F, d + start, (int)(len - start));
}

