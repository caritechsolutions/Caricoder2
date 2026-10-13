/*
 * caricoder3 - AC-3 decode through liba52
 * Copyright (c) 2026 CariTech Solutions
 *
 * liba52 is old and does not check a frame's CRC. The framer does that
 * (aframe.c) before anything reaches this file.
 */
#ifdef HAVE_A52

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <a52dec/a52.h>

#include "codec.h"

#define A52_MAX_FRAME 3840

struct a52_dec {
    a52_state_t *st;
    uint8_t buf[A52_MAX_FRAME];
};

static void *a52_open(enum codec_id codec, char *err, size_t errlen)
{
    struct a52_dec *d = calloc(1, sizeof(*d));

    (void)codec;
    if (!d)
        return NULL;
    d->st = a52_init(0);
    if (!d->st) {
        snprintf(err, errlen, "a52_init failed");
        free(d);
        return NULL;
    }
    return d;
}

static int16_t to_s16(sample_t v)
{
    float s = v * 32767.0f;

    if (s > 32767.0f)
        return 32767;
    if (s < -32768.0f)
        return -32768;
    return (int16_t)(s < 0 ? s - 0.5f : s + 0.5f);
}

static int a52_decode(void *ctx, const uint8_t *au, size_t len, int16_t *pcm,
                      int *rate, int *channels, int *delay)
{
    struct a52_dec *d = ctx;
    int flags = 0, sr = 0, br = 0, n, ch;
    sample_t level = 1.0f;

    if (len > sizeof(d->buf) || len < 7)
        return -1;
    memcpy(d->buf, au, len);                /* the library wants it writable */
    n = a52_syncinfo(d->buf, &flags, &sr, &br);
    if (n <= 0 || (size_t)n != len || sr <= 0)
        return -1;

    /* Mono stays mono; everything else, 5.1 included, comes out as a
     * stereo mix. The library applies the stream's own dynamic range
     * control, as a set-top box does on its stereo output. */
    ch = (flags & A52_CHANNEL_MASK) == A52_MONO ? 1 : 2;
    flags = (ch == 1 ? A52_MONO : A52_STEREO) | A52_ADJUST_LEVEL;
    if (a52_frame(d->st, d->buf, &flags, &level, 0))
        return -1;
    for (int blk = 0; blk < 6; blk++) {
        const sample_t *s;

        if (a52_block(d->st))
            return -1;
        s = a52_samples(d->st);
        for (int i = 0; i < 256; i++)
            for (int c = 0; c < ch; c++)
                pcm[(blk * 256 + i) * ch + c] = to_s16(s[c * 256 + i]);
    }
    *rate = sr;
    *channels = ch;
    *delay = 0;
    return 6 * 256;
}

static void a52_reset(void *ctx)
{
    (void)ctx;                              /* each frame decodes on its own
                                             * apart from the overlap, which
                                             * the next frame overwrites */
}

static void a52_close(void *ctx)
{
    struct a52_dec *d = ctx;

    a52_free(d->st);
    free(d);
}

static bool a52_handles(enum codec_id c)
{
    return c == CODEC_AC3;
}

static const struct adec_ops a52_ops = { a52_open, a52_decode, a52_reset, a52_close };

const struct codec_backend backend_a52 = {
    .name = "liba52", .codecs = "ac3",
    .media = CODEC_AUDIO, .role = CODEC_DECODER, .where = WHERE_CPU,
    .priority = 10, .handles = a52_handles, .adec = &a52_ops,
};

#endif /* HAVE_A52 */
