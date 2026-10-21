/*
 * caricoder3 - one output sample of a picture scaled on the card
 * Copyright (c) 2026 CariTech Solutions
 *
 * Plain C with no library calls, compiled three ways: into the card's own
 * program (src/nv_scale.cu, by clang to PTX: src/nv_scale_ptx.h), and
 * into the test stand-in for the card (tests/nvmock.c), which so runs the
 * very same arithmetic on the CPU.
 *
 * The same resampling as the CPU's (scale_plane in xc_video.c): each
 * output sample is the mean of the input samples its footprint covers,
 * with fractional weights at the edges, at most 4 inputs a side; for an
 * interlaced source each line is read as the mean of itself and the next
 * (field blend, which removes the comb). `comps`: 1 for the luma plane, 2
 * for NV12/P010's interleaved chroma. `wide`: 16-bit samples (P010).
 */
#ifndef CC3_NV_SCALE_PX_H
#define CC3_NV_SCALE_PX_H

#ifndef CC3_PX_FN
#define CC3_PX_FN static inline
#endif

CC3_PX_FN void cc3_scale_px(const unsigned char *src, int spitch, int sw, int sh,
                            unsigned char *dst, int dpitch, int dw, int dh,
                            int comps, int wide, int blend, int ox, int oy)
{
    float stx = (float)sw / (float)dw, sty = (float)sh / (float)dh;
    float ax = (float)ox * stx, bx = ax + stx, ay = (float)oy * sty, by = ay + sty;
    int x0 = (int)ax, x1 = (int)(bx - 0.0001f), y0 = (int)ay, y1 = (int)(by - 0.0001f);
    float acc0 = 0, acc1 = 0, wsum = 0;
    int bps = wide ? 2 : 1;

    if (ox >= dw || oy >= dh)
        return;
    if (x1 >= sw)
        x1 = sw - 1;
    if (x1 - x0 > 3)
        x1 = x0 + 3;
    if (x1 < x0)
        x1 = x0;
    if (y1 >= sh)
        y1 = sh - 1;
    if (y1 - y0 > 3)
        y1 = y0 + 3;
    if (y1 < y0)
        y1 = y0;
    for (int y = y0; y <= y1; y++) {
        float lo = (float)y > ay ? (float)y : ay, hi = (float)(y + 1) < by ? (float)(y + 1) : by;
        float wy = hi - lo;

        if (wy <= 0)
            wy = 0.0001f;
        for (int half = 0; half < (blend ? 2 : 1); half++) {
            int ys = y + half < sh ? y + half : sh - 1;
            float wr = blend ? wy * 0.5f : wy;
            const unsigned char *row = src + (long)ys * spitch;

            for (int x = x0; x <= x1; x++) {
                float l2 = (float)x > ax ? (float)x : ax, h2 = (float)(x + 1) < bx ? (float)(x + 1) : bx;
                float w = (h2 - l2 > 0 ? h2 - l2 : 0.0001f) * wr;
                const unsigned char *s = row + (long)x * comps * bps;

                acc0 += w * (float)(wide ? (s[0] | (s[1] << 8)) : s[0]);
                if (comps == 2)
                    acc1 += w * (float)(wide ? (s[2] | (s[3] << 8)) : s[1]);
                wsum += w;
            }
        }
    }
    {
        unsigned char *d = dst + (long)oy * dpitch + (long)ox * comps * bps;
        unsigned v0 = (unsigned)(acc0 / wsum + 0.5f), v1 = (unsigned)(acc1 / wsum + 0.5f);

        if (wide) {
            d[0] = (unsigned char)v0;
            d[1] = (unsigned char)(v0 >> 8);
            if (comps == 2) {
                d[2] = (unsigned char)v1;
                d[3] = (unsigned char)(v1 >> 8);
            }
        } else {
            d[0] = (unsigned char)(v0 > 255 ? 255 : v0);
            if (comps == 2)
                d[1] = (unsigned char)(v1 > 255 ? 255 : v1);
        }
    }
}

#endif
