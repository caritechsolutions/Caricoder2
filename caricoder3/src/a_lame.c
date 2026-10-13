/*
 * caricoder3 - MPEG audio layer III encode through LAME
 * Copyright (c) 2026 CariTech Solutions
 */
#ifdef HAVE_LAME

#include <stdlib.h>

#include <lame/lame.h>

#include "codec.h"

struct lame_enc {
    lame_global_flags *g;
    int frame;
    int channels;
};

static void *lm_open(enum codec_id codec, int rate, int channels, int *bitrate,
                     int *frame, int *delay, char *err, size_t errlen)
{
    struct lame_enc *e = calloc(1, sizeof(*e));

    (void)codec;
    if (!e)
        return NULL;
    e->g = lame_init();
    if (!e->g) {
        free(e);
        return NULL;
    }
    lame_set_errorf(e->g, NULL);
    lame_set_debugf(e->g, NULL);
    lame_set_msgf(e->g, NULL);
    lame_set_in_samplerate(e->g, rate);
    lame_set_out_samplerate(e->g, rate);
    lame_set_num_channels(e->g, channels);
    lame_set_mode(e->g, channels == 1 ? MONO : JOINT_STEREO);
    lame_set_brate(e->g, *bitrate / 1000);
    lame_set_quality(e->g, 5);
    lame_set_bWriteVbrTag(e->g, 0);         /* a stream, not a file */
    if (lame_init_params(e->g) < 0 || lame_get_out_samplerate(e->g) != rate) {
        snprintf(err, errlen, "layer III at %d Hz, %d channel(s), %d kb/s is not "
                 "something the encoder accepts", rate, channels, *bitrate / 1000);
        lame_close(e->g);
        free(e);
        return NULL;
    }
    *bitrate = lame_get_brate(e->g) * 1000;    /* it settles on the nearest allowed */
    e->channels = channels;
    e->frame = lame_get_framesize(e->g);
    *frame = e->frame;
    /* The encoder's own delay plus what a standard decoder adds.
     * Confirmed by measurement: see the sync tests. */
    *delay = lame_get_encoder_delay(e->g) + 529;
    return e;
}

static int lm_encode(void *ctx, const int16_t *pcm, uint8_t *out, size_t max)
{
    struct lame_enc *e = ctx;
    int n;

    /* The interleaved call reads two samples per frame whatever the
     * number of channels: given mono it would take every other sample and
     * read past the end. (The fuzzer found this.) */
    if (e->channels == 1)
        n = lame_encode_buffer(e->g, pcm, pcm, e->frame, out, (int)max);
    else
        n = lame_encode_buffer_interleaved(e->g, (short *)(uintptr_t)pcm, e->frame,
                                           out, (int)max);

    return n < 0 ? -1 : n;
}

static void lm_close(void *ctx)
{
    struct lame_enc *e = ctx;

    lame_close(e->g);
    free(e);
}

static bool lm_handles(enum codec_id c)
{
    return c == CODEC_MP3;
}

static const struct aenc_ops lm_ops = { lm_open, lm_encode, lm_close };

const struct codec_backend backend_lame = {
    .name = "lame", .codecs = "mp3",
    .media = CODEC_AUDIO, .role = CODEC_ENCODER, .where = WHERE_CPU,
    .priority = 10, .handles = lm_handles, .aenc = &lm_ops,
};

#endif /* HAVE_LAME */
