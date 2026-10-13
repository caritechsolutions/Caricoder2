/*
 * caricoder3 - audio framing
 * Copyright (c) 2026 CariTech Solutions
 */
#include "aframe.h"

#include <stdlib.h>
#include <string.h>

#define BUF_CAP     (64 * 1024)     /* several of the largest frames */
#define MARKS       8
#define HDR_NEED    7               /* bytes enough to judge any header */

struct mark {
    size_t off;                     /* position in buf the time belongs to */
    stc_t  pts;
};

struct aframer {
    enum codec_id codec;
    uint8_t buf[BUF_CAP];
    size_t rd, wr;
    struct mark marks[MARKS];
    int n_marks;
    bool locked;                    /* the last frame ended where this begins */
    uint64_t skipped, foreign;
};

/* ---- headers -------------------------------------------------------- */

/* Each returns the frame length in bytes, or -1 if `p` (HDR_NEED bytes)
 * is not the start of a frame. */

static int hdr_adts(const uint8_t *p)
{
    int len;

    if (p[0] != 0xFF || (p[1] & 0xF6) != 0xF0)
        return -1;
    if (((p[2] >> 2) & 0x0F) > 12)          /* sampling_frequency_index */
        return -1;
    len = ((p[3] & 3) << 11) | (p[4] << 3) | (p[5] >> 5);
    return len >= 7 ? len : -1;
}

static int hdr_loas(const uint8_t *p)
{
    int len;

    if (p[0] != 0x56 || (p[1] & 0xE0) != 0xE0)
        return -1;
    len = ((p[1] & 0x1F) << 8) | p[2];
    return len > 0 ? len + 3 : -1;
}

static int hdr_mpa(const uint8_t *p)
{
    static const int kbps[2][3][16] = {
        {                                   /* MPEG-1: layer I, II, III */
            { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0 },
            { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0 },
            { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 },
        },
        {                                   /* MPEG-2 and 2.5 */
            { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0 },
            { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 },
            { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 },
        },
    };
    static const int sr[4][3] = {
        { 11025, 12000, 8000 },             /* MPEG-2.5 */
        { 0, 0, 0 },
        { 22050, 24000, 16000 },            /* MPEG-2 */
        { 44100, 48000, 32000 },            /* MPEG-1 */
    };
    int ver, layer, bri, sri, pad, br, rate;

    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0)
        return -1;
    ver = (p[1] >> 3) & 3;
    layer = 4 - ((p[1] >> 1) & 3);          /* 1, 2, 3; 4 = reserved */
    bri = p[2] >> 4;
    sri = (p[2] >> 2) & 3;
    pad = (p[2] >> 1) & 1;
    if (ver == 1 || layer == 4 || bri == 0 || bri == 15 || sri == 3)
        return -1;
    br = kbps[ver == 3 ? 0 : 1][layer - 1][bri] * 1000;
    rate = sr[ver][sri];
    if (layer == 1)
        return (12 * br / rate + pad) * 4;
    if (layer == 2 || ver == 3)
        return 144 * br / rate + pad;
    return 72 * br / rate + pad;
}

static int hdr_ac3(struct aframer *f, const uint8_t *p)
{
    static const uint16_t words_44[38] = {
        69, 70, 87, 88, 104, 105, 121, 122, 139, 140, 174, 175, 208, 209,
        243, 244, 278, 279, 348, 349, 417, 418, 487, 488, 557, 558, 696, 697,
        835, 836, 975, 976, 1114, 1115, 1253, 1254, 1393, 1394,
    };
    static const uint16_t kbps[19] = {
        32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384,
        448, 512, 576, 640,
    };
    int fscod, code, bsid;

    if (p[0] != 0x0B || p[1] != 0x77)
        return -1;
    bsid = p[5] >> 3;
    if (bsid > 10) {                        /* E-AC-3: another coding */
        if (bsid <= 16)
            f->foreign++;
        return -1;
    }
    fscod = p[4] >> 6;
    code = p[4] & 0x3F;
    if (fscod == 3 || code >= 38)
        return -1;
    if (fscod == 0)
        return 2 * kbps[code / 2] * 2;      /* 48 kHz */
    if (fscod == 1)
        return 2 * words_44[code];
    return 2 * kbps[code / 2] * 3;          /* 32 kHz */
}

static int hdr(struct aframer *f, const uint8_t *p)
{
    switch (f->codec) {
    case CODEC_AAC:      return hdr_adts(p);
    case CODEC_AAC_LATM: return hdr_loas(p);
    case CODEC_AC3:      return hdr_ac3(f, p);
    default:             return hdr_mpa(p);
    }
}

/* Could `q` be the header after `p` in one stream? Guards the first lock
 * against a chance match inside audio data. */
static bool same_stream(const struct aframer *f, const uint8_t *p, const uint8_t *q)
{
    switch (f->codec) {
    case CODEC_AAC:
        return p[1] == q[1] && (p[2] & 0xFC) == (q[2] & 0xFC);
    case CODEC_AC3:
        return (p[4] & 0xC0) == (q[4] & 0xC0);
    case CODEC_AAC_LATM:
        return true;
    default:                                /* version, layer, sample rate */
        return (p[1] & 0xFE) == (q[1] & 0xFE) && (p[2] & 0x0C) == (q[2] & 0x0C);
    }
}

/* AC-3 carries a CRC over the whole frame. The decoder library does not
 * look at it, so we do: a damaged frame is not handed on. */
static bool ac3_crc_ok(const uint8_t *p, int len)
{
    unsigned crc = 0;

    for (int i = 2; i < len; i++) {
        crc ^= (unsigned)p[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? ((crc << 1) ^ 0x8005) & 0xFFFF : (crc << 1) & 0xFFFF;
    }
    return crc == 0;
}

/* ---- the buffer ----------------------------------------------------- */

struct aframer *aframer_new(enum codec_id codec)
{
    struct aframer *f = calloc(1, sizeof(*f));

    if (f)
        f->codec = codec;
    return f;
}

void aframer_free(struct aframer *f)
{
    free(f);
}

void aframer_reset(struct aframer *f)
{
    f->rd = f->wr = 0;
    f->n_marks = 0;
    f->locked = false;
}

uint64_t aframer_skipped(const struct aframer *f)
{
    return f->skipped;
}

uint64_t aframer_foreign(const struct aframer *f)
{
    return f->foreign;
}

static void compact(struct aframer *f)
{
    if (f->rd == 0)
        return;
    memmove(f->buf, f->buf + f->rd, f->wr - f->rd);
    for (int i = 0; i < f->n_marks; i++)
        f->marks[i].off = f->marks[i].off > f->rd ? f->marks[i].off - f->rd : 0;
    f->wr -= f->rd;
    f->rd = 0;
}

void aframer_push(struct aframer *f, const uint8_t *data, size_t len, stc_t pts)
{
    if (len > BUF_CAP) {                    /* cannot be audio frames */
        f->skipped += len;
        aframer_reset(f);
        return;
    }
    if (f->wr + len > BUF_CAP)
        compact(f);
    if (f->wr + len > BUF_CAP) {
        /* Full of bytes no frame was found in. Start again from here. */
        f->skipped += f->wr - f->rd;
        aframer_reset(f);
    }
    if (pts != NOPTS) {
        if (f->n_marks == MARKS) {
            memmove(f->marks, f->marks + 1, sizeof(f->marks[0]) * (MARKS - 1));
            f->n_marks--;
        }
        f->marks[f->n_marks].off = f->wr;
        f->marks[f->n_marks].pts = pts;
        f->n_marks++;
    }
    memcpy(f->buf + f->wr, data, len);
    f->wr += len;
}

bool aframer_next(struct aframer *f, const uint8_t **au, size_t *len, stc_t *pts)
{
    for (;;) {
        size_t avail = f->wr - f->rd;
        const uint8_t *p = f->buf + f->rd;
        int n;

        if (avail < HDR_NEED)
            return false;
        n = hdr(f, p);
        if (n < HDR_NEED)
            goto skip;
        if (avail < (size_t)n)
            return false;                   /* the rest is still to come */
        if (!f->locked) {
            /* Believe a first frame only if another follows it. */
            if (avail < (size_t)n + HDR_NEED)
                return false;
            if (hdr(f, p + n) < HDR_NEED || !same_stream(f, p, p + n))
                goto skip;
        }
        if (f->codec == CODEC_AC3 && !ac3_crc_ok(p, n))
            goto skip;

        /* The newest time stamp given at or before this frame's start. */
        *pts = NOPTS;
        while (f->n_marks > 0 && f->marks[0].off <= f->rd) {
            *pts = f->marks[0].pts;
            memmove(f->marks, f->marks + 1, sizeof(f->marks[0]) * (size_t)(f->n_marks - 1));
            f->n_marks--;
        }
        *au = p;
        *len = (size_t)n;
        f->rd += (size_t)n;
        f->locked = true;
        return true;

skip:
        f->rd++;
        f->skipped++;
        f->locked = false;
    }
}
