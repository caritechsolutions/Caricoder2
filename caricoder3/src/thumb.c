/*
 * caricoder3 - a thumbnail of the channel's picture every few seconds
 * Copyright (c) 2026 CariTech Solutions
 *
 * The JPEG writer is a plain baseline encoder (4:2:0, standard Huffman
 * tables, the usual quantisation tables scaled by quality): a thumbnail
 * is 320 pixels wide, so speed is of no concern, and it keeps libjpeg
 * out of the product. Pictures are brought to 8-bit yuv420p at the
 * thumbnail size by box averaging (an integer factor, then the last
 * fraction by nearest sample).
 */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "thumb.h"
#include "watch.h"
#include "xcode.h"

#ifdef HAVE_NVIDIA
#include "nv_common.h"
#endif

#define THUMB_WIDTH 320
#define THUMB_QUALITY 70

static char g_path[512];
static int g_interval = 5;
static int64_t g_last_ns;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int64_t mono_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void thumb_init(const char *path, int interval_s)
{
    snprintf(g_path, sizeof(g_path), "%s", path ? path : "");
    g_interval = interval_s > 0 ? interval_s : 5;
}

bool thumb_enabled(void)
{
    return g_path[0] != 0;
}

static bool due(void)
{
    int64_t now = mono_ns();
    bool d;

    pthread_mutex_lock(&g_lock);
    d = g_path[0] && now - g_last_ns >= (int64_t)g_interval * 1000000000LL;
    if (d)
        g_last_ns = now;
    pthread_mutex_unlock(&g_lock);
    return d;
}

/* ---- the JPEG writer ---------------------------------------------------- */

struct bits {
    uint8_t *buf;
    size_t len, cap;
    uint32_t acc;
    int nbits;
};

static void put_byte(struct bits *b, uint8_t v)
{
    if (b->len + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 16384;
        uint8_t *nb = realloc(b->buf, nc);

        if (!nb)
            return;
        b->buf = nb;
        b->cap = nc;
    }
    b->buf[b->len++] = v;
}

static void put_bits(struct bits *b, uint32_t v, int n)
{
    b->acc = (b->acc << n) | (v & ((1u << n) - 1));
    b->nbits += n;
    while (b->nbits >= 8) {
        uint8_t byte = (uint8_t)(b->acc >> (b->nbits - 8));

        put_byte(b, byte);
        if (byte == 0xFF)
            put_byte(b, 0);             /* stuffing */
        b->nbits -= 8;
    }
}

static void flush_bits(struct bits *b)
{
    if (b->nbits > 0)
        put_bits(b, 0x7F, 8 - b->nbits);    /* pad with ones */
}

static const uint8_t zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21,
    28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61,
    54, 47, 55, 62, 63
};
static const uint8_t std_lum_q[64] = {
    16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51,
    87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120,
    101, 72, 92, 95, 98, 112, 100, 103, 99
};
static const uint8_t std_chr_q[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99
};
/* the standard Huffman tables (ITU T.81 K.3) as bits/values */
static const uint8_t dc_lum_bits[17] = { 0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t dc_lum_val[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8_t dc_chr_bits[17] = { 0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const uint8_t dc_chr_val[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8_t ac_lum_bits[17] = { 0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const uint8_t ac_lum_val[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14,
    0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09,
    0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
    0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65,
    0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
    0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9,
    0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca,
    0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
    0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa
};
static const uint8_t ac_chr_bits[17] = { 0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
static const uint8_t ac_chr_val[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32,
    0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16,
    0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64,
    0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86,
    0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8,
    0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
    0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa
};

struct huff {
    uint16_t code[256];
    uint8_t len[256];
};

static void huff_build(struct huff *h, const uint8_t *bits, const uint8_t *val)
{
    int code = 0, k = 0;

    memset(h, 0, sizeof(*h));
    for (int l = 1; l <= 16; l++) {
        for (int i = 0; i < bits[l]; i++) {
            h->code[val[k]] = (uint16_t)code;
            h->len[val[k]] = (uint8_t)l;
            k++;
            code++;
        }
        code <<= 1;
    }
}

static void write_dht(struct bits *b, int class_id, const uint8_t *bits, const uint8_t *val)
{
    int n = 0;

    for (int l = 1; l <= 16; l++)
        n += bits[l];
    put_byte(b, 0xFF); put_byte(b, 0xC4);
    put_byte(b, (uint8_t)((3 + 16 + n) >> 8)); put_byte(b, (uint8_t)(3 + 16 + n));
    put_byte(b, (uint8_t)class_id);
    for (int l = 1; l <= 16; l++)
        put_byte(b, bits[l]);
    for (int i = 0; i < n; i++)
        put_byte(b, val[i]);
}

static void fdct8x8(const float in[64], float out[64])
{
    static float c[8][8];
    static bool init;

    if (!init) {
        for (int u = 0; u < 8; u++)
            for (int x = 0; x < 8; x++)
                c[u][x] = (float)((u == 0 ? sqrt(0.125) : 0.5) * cos((2 * x + 1) * u * M_PI / 16));
        init = true;
    }
    float tmp[64];

    for (int y = 0; y < 8; y++)
        for (int u = 0; u < 8; u++) {
            float s = 0;

            for (int x = 0; x < 8; x++)
                s += in[y * 8 + x] * c[u][x];
            tmp[y * 8 + u] = s;
        }
    for (int u = 0; u < 8; u++)
        for (int v = 0; v < 8; v++) {
            float s = 0;

            for (int y = 0; y < 8; y++)
                s += tmp[y * 8 + u] * c[v][y];
            out[v * 8 + u] = s;
        }
}

static int bit_length(int v)
{
    int n = 0;

    if (v < 0)
        v = -v;
    while (v) {
        n++;
        v >>= 1;
    }
    return n;
}

static void encode_block(struct bits *b, const uint8_t *src, int stride, const uint8_t *q,
                         const struct huff *dc, const struct huff *ac, int *pred)
{
    float in[64], out[64];
    int coef[64];

    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            in[y * 8 + x] = (float)src[y * stride + x] - 128.0f;
    fdct8x8(in, out);
    for (int i = 0; i < 64; i++) {
        float v = out[zigzag[i]] / (float)q[i];

        coef[i] = (int)(v < 0 ? v - 0.5f : v + 0.5f);
    }
    /* DC */
    {
        int diff = coef[0] - *pred, n = bit_length(diff);

        *pred = coef[0];
        put_bits(b, dc->code[n], dc->len[n]);
        if (n)
            put_bits(b, (uint32_t)(diff < 0 ? diff - 1 : diff), n);
    }
    /* AC */
    {
        int run = 0;

        for (int i = 1; i < 64; i++) {
            if (coef[i] == 0) {
                run++;
                continue;
            }
            while (run > 15) {
                put_bits(b, ac->code[0xF0], ac->len[0xF0]);
                run -= 16;
            }
            {
                int n = bit_length(coef[i]), sym = (run << 4) | n;

                put_bits(b, ac->code[sym], ac->len[sym]);
                put_bits(b, (uint32_t)(coef[i] < 0 ? coef[i] - 1 : coef[i]), n);
            }
            run = 0;
        }
        if (run > 0)
            put_bits(b, ac->code[0], ac->len[0]);     /* EOB */
    }
}

size_t thumb_jpeg(const uint8_t *yp, int ys, const uint8_t *up, int us, const uint8_t *vp, int vs,
                  int width, int height, int quality, uint8_t **out)
{
    struct bits b;
    uint8_t lq[64], cq[64];
    struct huff dcl, acl, dcc, acc;
    int scale = quality < 50 ? 5000 / (quality < 1 ? 1 : quality) : 200 - 2 * (quality > 100 ? 100 : quality);
    int mbw = (width + 15) / 16, mbh = (height + 15) / 16;
    int pred_y = 0, pred_u = 0, pred_v = 0;
    /* padded copies: the right and bottom edges repeated to a whole MCU */
    int pw = mbw * 16, ph = mbh * 16;
    uint8_t *Y = malloc((size_t)pw * ph), *U = malloc((size_t)pw * ph / 4), *V = malloc((size_t)pw * ph / 4);

    *out = NULL;
    if (!Y || !U || !V) {
        free(Y); free(U); free(V);
        return 0;
    }
    for (int y = 0; y < ph; y++)
        for (int x = 0; x < pw; x++)
            Y[y * pw + x] = yp[(y < height ? y : height - 1) * ys + (x < width ? x : width - 1)];
    for (int y = 0; y < ph / 2; y++)
        for (int x = 0; x < pw / 2; x++) {
            int sy = y < (height + 1) / 2 ? y : (height + 1) / 2 - 1, sx = x < (width + 1) / 2 ? x : (width + 1) / 2 - 1;

            U[y * pw / 2 + x] = up[sy * us + sx];
            V[y * pw / 2 + x] = vp[sy * vs + sx];
        }
    for (int i = 0; i < 64; i++) {
        int l = (std_lum_q[zigzag[i]] * scale + 50) / 100, c = (std_chr_q[zigzag[i]] * scale + 50) / 100;

        lq[i] = (uint8_t)(l < 1 ? 1 : l > 255 ? 255 : l);
        cq[i] = (uint8_t)(c < 1 ? 1 : c > 255 ? 255 : c);
    }
    huff_build(&dcl, dc_lum_bits, dc_lum_val);
    huff_build(&acl, ac_lum_bits, ac_lum_val);
    huff_build(&dcc, dc_chr_bits, dc_chr_val);
    huff_build(&acc, ac_chr_bits, ac_chr_val);

    memset(&b, 0, sizeof(b));
    put_byte(&b, 0xFF); put_byte(&b, 0xD8);                   /* SOI */
    /* APP0 JFIF */
    {
        static const uint8_t app0[] = { 0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0 };

        for (size_t i = 0; i < sizeof(app0); i++)
            put_byte(&b, app0[i]);
    }
    /* DQT, both tables */
    put_byte(&b, 0xFF); put_byte(&b, 0xDB); put_byte(&b, 0); put_byte(&b, 132);
    put_byte(&b, 0);
    for (int i = 0; i < 64; i++)
        put_byte(&b, lq[i]);
    put_byte(&b, 1);
    for (int i = 0; i < 64; i++)
        put_byte(&b, cq[i]);
    /* SOF0: 3 components, Y at 2x2 */
    put_byte(&b, 0xFF); put_byte(&b, 0xC0); put_byte(&b, 0); put_byte(&b, 17); put_byte(&b, 8);
    put_byte(&b, (uint8_t)(height >> 8)); put_byte(&b, (uint8_t)height);
    put_byte(&b, (uint8_t)(width >> 8)); put_byte(&b, (uint8_t)width);
    put_byte(&b, 3);
    put_byte(&b, 1); put_byte(&b, 0x22); put_byte(&b, 0);
    put_byte(&b, 2); put_byte(&b, 0x11); put_byte(&b, 1);
    put_byte(&b, 3); put_byte(&b, 0x11); put_byte(&b, 1);
    /* DHT x4 */
    write_dht(&b, 0x00, dc_lum_bits, dc_lum_val);
    write_dht(&b, 0x10, ac_lum_bits, ac_lum_val);
    write_dht(&b, 0x01, dc_chr_bits, dc_chr_val);
    write_dht(&b, 0x11, ac_chr_bits, ac_chr_val);
    /* SOS */
    put_byte(&b, 0xFF); put_byte(&b, 0xDA); put_byte(&b, 0); put_byte(&b, 12); put_byte(&b, 3);
    put_byte(&b, 1); put_byte(&b, 0x00);
    put_byte(&b, 2); put_byte(&b, 0x11);
    put_byte(&b, 3); put_byte(&b, 0x11);
    put_byte(&b, 0); put_byte(&b, 63); put_byte(&b, 0);
    /* the MCUs: 4 luma blocks, then Cb, then Cr */
    for (int my = 0; my < mbh; my++)
        for (int mx = 0; mx < mbw; mx++) {
            for (int k = 0; k < 4; k++)
                encode_block(&b, Y + (my * 16 + (k / 2) * 8) * pw + mx * 16 + (k % 2) * 8, pw, lq, &dcl, &acl, &pred_y);
            encode_block(&b, U + my * 8 * (pw / 2) + mx * 8, pw / 2, cq, &dcc, &acc, &pred_u);
            encode_block(&b, V + my * 8 * (pw / 2) + mx * 8, pw / 2, cq, &dcc, &acc, &pred_v);
        }
    flush_bits(&b);
    put_byte(&b, 0xFF); put_byte(&b, 0xD9);                   /* EOI */
    free(Y); free(U); free(V);
    if (!b.buf)
        return 0;
    *out = b.buf;
    return b.len;
}

/* ---- from a picture to the file ----------------------------------------- */

/* 8-bit yuv420p at the thumbnail size from a CPU picture of any size and
 * depth: box averaging by an integer factor, the rest by nearest. */
static bool shrink(const struct vframe *f, int tw, int th, uint8_t *Y, uint8_t *U, uint8_t *V)
{
    int fx = f->width / tw > 1 ? f->width / tw : 1, fy = f->height / th > 1 ? f->height / th : 1;
    int shift = f->depth > 8 ? f->depth - 8 : 0;
    int bps = f->depth > 8 ? 2 : 1;

    for (int p = 0; p < 3; p++) {
        int sw = p ? f->width / 2 : f->width, sh = p ? f->height / 2 : f->height;
        int dw = p ? tw / 2 : tw, dh = p ? th / 2 : th;
        uint8_t *dst = p == 0 ? Y : p == 1 ? U : V;

        for (int y = 0; y < dh; y++) {
            int sy = (int)((int64_t)y * sh / dh);

            for (int x = 0; x < dw; x++) {
                int sx = (int)((int64_t)x * sw / dw);
                int sum = 0, n = 0;

                for (int yy = sy; yy < sy + fy && yy < sh; yy++)
                    for (int xx = sx; xx < sx + fx && xx < sw; xx++) {
                        const uint8_t *s = f->plane[p] + (size_t)yy * f->stride[p] + (size_t)xx * bps;

                        sum += bps == 2 ? ((s[0] | (s[1] << 8)) >> shift) : s[0];
                        n++;
                    }
                dst[y * dw + x] = (uint8_t)(n ? sum / n : 0);
            }
        }
    }
    return true;
}

static void write_file(const uint8_t *jpeg, size_t len)
{
    char tmp[560];
    FILE *f;

    snprintf(tmp, sizeof(tmp), "%s.tmp", g_path);
    f = fopen(tmp, "wb");
    if (!f)
        return;
    if (fwrite(jpeg, 1, len, f) != len) {
        fclose(f);
        unlink(tmp);
        return;
    }
    fclose(f);
    rename(tmp, g_path);
}

static void make_from_cpu(const struct vframe *f)
{
    int tw = THUMB_WIDTH, th;
    double sar = f->sar_num > 0 && f->sar_den > 0 ? (double)f->sar_num / f->sar_den : 1.0;
    uint8_t *Y, *U, *V, *jpeg = NULL;
    size_t len;

    if (f->width < 16 || f->height < 16)
        return;
    th = (int)((double)f->height * tw / ((double)f->width * sar) + 0.5);
    th = (th + 1) & ~1;
    if (th < 16)
        th = 16;
    Y = malloc((size_t)tw * th);
    U = malloc((size_t)tw * th / 4);
    V = malloc((size_t)tw * th / 4);
    if (Y && U && V && shrink(f, tw, th, Y, U, V)) {
        watch_picture(Y, tw, th);
        len = thumb_jpeg(Y, tw, U, tw / 2, V, tw / 2, tw, th, THUMB_QUALITY, &jpeg);
        if (len)
            write_file(jpeg, len);
    }
    free(jpeg);
    free(Y);
    free(U);
    free(V);
}

#ifdef HAVE_NVIDIA
/* An NV12/P010 surface on the card: fetched to ordinary memory and
 * re-arranged into three planes. */
static void make_from_cuda(const struct vframe *f)
{
    CudaFunctions *cu = nv_cu();
    int bps = f->depth > 8 ? 2 : 1;
    size_t row = (size_t)f->width * bps;
    uint8_t *buf = malloc(row * (size_t)f->height * 3 / 2);
    CUDA_MEMCPY2D c;
    CUcontext dummy;

    if (!buf || !cu)
        goto out;
    memset(&c, 0, sizeof(c));
    c.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    c.srcDevice = (CUdeviceptr)(uintptr_t)f->plane[0];
    c.srcPitch = (size_t)f->stride[0];
    c.dstMemoryType = CU_MEMORYTYPE_HOST;
    c.dstHost = buf;
    c.dstPitch = row;
    c.WidthInBytes = row;
    c.Height = (size_t)f->height * 3 / 2;
    if (f->cuda_ctx)
        cu->cuCtxPushCurrent(f->cuda_ctx);
    {
        CUresult r = cu->cuMemcpy2D(&c);

        if (f->cuda_ctx)
            cu->cuCtxPopCurrent(&dummy);
        if (r != CUDA_SUCCESS)
            goto out;
    }
    {
        /* de-interleave the chroma into U and V planes */
        struct vframe g = *f;
        size_t cw = (size_t)f->width / 2, ch = (size_t)f->height / 2;
        uint8_t *u = malloc(cw * ch * bps), *v = malloc(cw * ch * bps);

        if (u && v) {
            const uint8_t *src = buf + row * (size_t)f->height;

            for (size_t y = 0; y < ch; y++)
                for (size_t x = 0; x < cw; x++)
                    for (int k = 0; k < bps; k++) {
                        u[(y * cw + x) * bps + k] = src[y * row + (2 * x) * bps + k];
                        v[(y * cw + x) * bps + k] = src[y * row + (2 * x + 1) * bps + k];
                    }
            g.mem = VMEM_CPU;
            g.plane[0] = buf; g.stride[0] = (int)row;
            g.plane[1] = u; g.stride[1] = (int)(cw * bps);
            g.plane[2] = v; g.stride[2] = (int)(cw * bps);
            make_from_cpu(&g);
        }
        free(u);
        free(v);
    }
out:
    free(buf);
}
#endif

void thumb_offer(const struct vframe *f)
{
    if (!g_path[0] || !due())
        return;
    if (f->mem == VMEM_CPU)
        make_from_cpu(f);
#ifdef HAVE_NVIDIA
    else if (f->mem == VMEM_CUDA)
        make_from_cuda(f);
#endif
}

/* ---- a copied video stream: one key frame decoded now and then ---------- */

static struct {
    pthread_mutex_t lock;
    const struct codec_backend *backend;
    void *dec;
    enum codec_id codec;
    bool waiting;                       /* a thumbnail is due: the next key unit is taken */
    bool failed;
} tap = { PTHREAD_MUTEX_INITIALIZER, NULL, NULL, CODEC_NONE, false, false };

void thumb_tap_unit(const struct es_unit *u, enum codec_id codec)
{
    int off;
    struct vframe f;

    if (!g_path[0] || tap.failed)
        return;
    pthread_mutex_lock(&tap.lock);
    if (!tap.waiting) {
        if (!due()) {
            pthread_mutex_unlock(&tap.lock);
            return;
        }
        tap.waiting = true;
    }
    off = pes_payload_offset(u->data, u->len);
    if (off < 0 || !codec_es_has_key(codec, u->data + off, u->len - (size_t)off, true)) {
        pthread_mutex_unlock(&tap.lock);
        return;                         /* not a key frame: wait for one */
    }
    if (tap.dec && tap.codec != codec) {
        tap.backend->vdec->close(tap.dec);
        tap.dec = NULL;
    }
    if (!tap.dec) {
        struct vdec_cfg dc = { .codec = codec };
        char err[160];

        tap.backend = codec_find(CODEC_VIDEO, CODEC_DECODER, codec, WHERE_CPU, 0);
        tap.dec = tap.backend ? tap.backend->vdec->open(&dc, err, sizeof(err)) : NULL;
        if (!tap.dec) {
            log_warn("thumbnails: no CPU decoder for %s; none will be made", codec_name(codec));
            tap.failed = true;
            pthread_mutex_unlock(&tap.lock);
            return;
        }
        tap.codec = codec;
    }
    /* the key unit alone: pushed, drained, one picture out */
    tap.backend->vdec->push(tap.dec, u->data + off, u->len - (size_t)off, u->pts, u->dts);
    tap.backend->vdec->drain(tap.dec);
    if (tap.backend->vdec->pull(tap.dec, &f) == 1 && f.mem == VMEM_CPU)
        make_from_cpu(&f);
    tap.backend->vdec->reset(tap.dec);
    tap.waiting = false;
    pthread_mutex_unlock(&tap.lock);
}
