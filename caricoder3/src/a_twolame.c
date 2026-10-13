/*
 * caricoder3 - MPEG audio layer II encode through twolame
 * Copyright (c) 2026 CariTech Solutions
 */
#ifdef HAVE_TWOLAME

#include <stdbool.h>
#include <stdlib.h>

#include <twolame.h>

#include "codec.h"

struct tl_enc {
    twolame_options *o;
};

/* The rates layer II may run at depend on the sample rate and on whether
 * it is mono. Picks the highest allowed rate that does not exceed `want`. */
static int nearest_rate(int rate, int channels, int want)
{
    static const int full[] = { 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224,
                                256, 320, 384 };            /* 32, 44.1, 48 kHz */
    static const int low[] = { 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128,
                               144, 160 };                  /* 16, 22.05, 24 kHz */
    bool lsf = rate < 32000;
    const int *tab = lsf ? low : full;
    int best = 0;

    for (int i = 0; i < 14; i++) {
        int kb = tab[i];

        if (!lsf && channels == 1 && kb > 192)
            continue;                       /* too much for one channel */
        if (!lsf && channels == 2 && (kb == 32 || kb == 48 || kb == 56 || kb == 80))
            continue;                       /* too little for two */
        if (best == 0 || kb * 1000 <= want)
            best = kb;
    }
    return best * 1000;
}

static void *tl_open(enum codec_id codec, int rate, int channels, int *bitrate,
                     int *frame, int *delay, char *err, size_t errlen)
{
    struct tl_enc *e = calloc(1, sizeof(*e));

    (void)codec;
    if (!e)
        return NULL;
    e->o = twolame_init();
    if (!e->o) {
        free(e);
        return NULL;
    }
    *bitrate = nearest_rate(rate, channels, *bitrate);
    twolame_set_verbosity(e->o, 0);
    if (twolame_set_num_channels(e->o, channels) != 0 ||
        twolame_set_in_samplerate(e->o, rate) != 0 ||
        twolame_set_out_samplerate(e->o, rate) != 0 ||
        twolame_set_mode(e->o, channels == 1 ? TWOLAME_MONO : TWOLAME_STEREO) != 0 ||
        twolame_set_bitrate(e->o, *bitrate / 1000) != 0 ||
        twolame_init_params(e->o) != 0) {
        snprintf(err, errlen, "layer II at %d Hz, %d channel(s), %d kb/s is not a "
                 "combination the standard allows", rate, channels, *bitrate / 1000);
        twolame_close(&e->o);
        free(e);
        return NULL;
    }
    *frame = TWOLAME_SAMPLES_PER_FRAME;
    /* What a standard decoder adds to the encoder's own filter bank
     * delay. Confirmed by measurement: see the sync tests. */
    *delay = 481;
    return e;
}

static int tl_encode(void *ctx, const int16_t *pcm, uint8_t *out, size_t max)
{
    struct tl_enc *e = ctx;
    int n = twolame_encode_buffer_interleaved(e->o, pcm, TWOLAME_SAMPLES_PER_FRAME,
                                              out, (int)max);

    return n < 0 ? -1 : n;
}

static void tl_close(void *ctx)
{
    struct tl_enc *e = ctx;

    twolame_close(&e->o);
    free(e);
}

static bool tl_handles(enum codec_id c)
{
    return c == CODEC_MP2;
}

static const struct aenc_ops tl_ops = { tl_open, tl_encode, tl_close };

const struct codec_backend backend_twolame = {
    .name = "twolame", .codecs = "mp2",
    .media = CODEC_AUDIO, .role = CODEC_ENCODER, .where = WHERE_CPU,
    .priority = 10, .handles = tl_handles, .aenc = &tl_ops,
};

#endif /* HAVE_TWOLAME */
