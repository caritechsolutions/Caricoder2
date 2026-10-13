/*
 * caricoder3 - MPEG audio (layers I, II, III) decode through libmpg123
 * Copyright (c) 2026 CariTech Solutions
 */
#ifdef HAVE_MPG123

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <mpg123.h>

#include "codec.h"

struct mp_dec {
    mpg123_handle *h;
    int spf;                    /* samples per frame, once known */
};

static pthread_once_t mp_once = PTHREAD_ONCE_INIT;

static void mp_init(void)
{
    mpg123_init();
}

static bool mp_start(struct mp_dec *d)
{
    static const long rates[] = { 8000, 11025, 12000, 16000, 22050, 24000,
                                  32000, 44100, 48000 };

    /* Quiet, no trimming of "gapless" padding (every sample of every
     * frame is wanted, so that time adds up), and the stream's own
     * sample rate, 16 bit. */
    mpg123_param(d->h, MPG123_ADD_FLAGS, MPG123_QUIET, 0);
    mpg123_param(d->h, MPG123_REMOVE_FLAGS, MPG123_GAPLESS, 0);
    mpg123_format_none(d->h);
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
        mpg123_format(d->h, rates[i], MPG123_MONO | MPG123_STEREO, MPG123_ENC_SIGNED_16);
    return mpg123_open_feed(d->h) == MPG123_OK;
}

static void *mp_open(enum codec_id codec, char *err, size_t errlen)
{
    struct mp_dec *d = calloc(1, sizeof(*d));
    int rc;

    (void)codec;
    if (!d)
        return NULL;
    pthread_once(&mp_once, mp_init);
    d->h = mpg123_new(NULL, &rc);
    if (!d->h || !mp_start(d)) {
        snprintf(err, errlen, "mpg123: %s",
                 d->h ? mpg123_strerror(d->h) : mpg123_plain_strerror(rc));
        if (d->h)
            mpg123_delete(d->h);
        free(d);
        return NULL;
    }
    return d;
}

static void mp_reset(void *ctx)
{
    struct mp_dec *d = ctx;

    mpg123_close(d->h);
    mp_start(d);
}

static int mp_decode(void *ctx, const uint8_t *au, size_t len, int16_t *pcm,
                     int *rate, int *channels, int *delay)
{
    struct mp_dec *d = ctx;
    size_t room = (size_t)AUDIO_MAX_FRAME * AUDIO_MAX_CH * sizeof(int16_t);
    size_t total = 0, done = 0;
    long r = 0;
    int ch = 0, enc = 0, rc, samples;

    rc = mpg123_decode(d->h, au, len, (unsigned char *)pcm, room, &done);
    total = done;
    /* The first call after a start only announces the format. */
    while (rc == MPG123_NEW_FORMAT || (rc == MPG123_OK && done > 0 && total < room)) {
        rc = mpg123_decode(d->h, NULL, 0, (unsigned char *)pcm + total, room - total, &done);
        total += done;
    }
    if (rc == MPG123_ERR || rc == MPG123_DONE) {
        mp_reset(d);
        return -1;
    }
    if (total == 0)
        return 0;                           /* wants to see the next header first */
    if (mpg123_getformat(d->h, &r, &ch, &enc) != MPG123_OK || r <= 0 ||
        ch < 1 || ch > AUDIO_MAX_CH || enc != MPG123_ENC_SIGNED_16) {
        mp_reset(d);
        return -1;
    }
    samples = (int)(total / sizeof(int16_t) / (size_t)ch);
    d->spf = mpg123_spf(d->h);

    /*
     * The library holds the very first frame back until it has seen the
     * header of the second, then hands over both at once. Only the later
     * one belongs to the unit just given; the earlier is let go (24 ms,
     * once, at the very start or after damage), so that every call
     * answers for exactly its own unit.
     */
    if (d->spf > 0 && samples > d->spf) {
        memmove(pcm, pcm + (size_t)(samples - d->spf) * (size_t)ch,
                (size_t)d->spf * (size_t)ch * sizeof(int16_t));
        samples = d->spf;
    }
    *delay = 0;
    *rate = (int)r;
    *channels = ch;
    return samples;
}

static void mp_close(void *ctx)
{
    struct mp_dec *d = ctx;

    mpg123_close(d->h);
    mpg123_delete(d->h);
    free(d);
}

static bool mp_handles(enum codec_id c)
{
    return c == CODEC_MPA;
}

static const struct adec_ops mp_ops = { mp_open, mp_decode, mp_reset, mp_close };

const struct codec_backend backend_mpg123 = {
    .name = "mpg123", .codecs = "mp1, mp2, mp3",
    .media = CODEC_AUDIO, .role = CODEC_DECODER, .where = WHERE_CPU,
    .priority = 10, .handles = mp_handles, .adec = &mp_ops,
};

#endif /* HAVE_MPG123 */
