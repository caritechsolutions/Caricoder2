/*
 * caricoder3 - AAC decode and encode through libfdk-aac
 * Copyright (c) 2026 CariTech Solutions
 */
#ifdef HAVE_FDK_AAC

#include <stdlib.h>
#include <string.h>

/* The original library's headers define helper functions they do not use. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include <fdk-aac/aacdecoder_lib.h>
#include <fdk-aac/aacenc_lib.h>
#pragma GCC diagnostic pop

#include "codec.h"
#include "log.h"

/* ---- decoder -------------------------------------------------------- */

#define SBR_DELAY 962           /* samples, at the output rate */

struct fdk_dec {
    HANDLE_AACDECODER h;
    bool has_sbr;               /* the library can decode HE-AAC */
    bool warned;
};

/* Distributions ship this library with the HE-AAC parts taken out
 * ("fdk-aac-free"). Then it still decodes an HE-AAC stream, but only the
 * half-rate AAC-LC core inside it. */
static bool lib_has_sbr(void)
{
    static LIB_INFO info[FDK_MODULE_LAST];

    FDKinitLibInfo(info);
    aacDecoder_GetLibInfo(info);
    for (int i = 0; i < FDK_MODULE_LAST; i++)
        if (info[i].module_id == FDK_SBRDEC)
            return true;
    return false;
}

static void *fdk_dec_open(enum codec_id codec, char *err, size_t errlen)
{
    struct fdk_dec *d = calloc(1, sizeof(*d));

    if (!d)
        return NULL;
    d->h = aacDecoder_Open(codec == CODEC_AAC_LATM ? TT_MP4_LOAS : TT_MP4_ADTS, 1);
    if (!d->h) {
        snprintf(err, errlen, "aacDecoder_Open failed");
        free(d);
        return NULL;
    }
    /* 5.1 and the like are mixed down to stereo by the library; mono
     * stays mono. */
    aacDecoder_SetParam(d->h, AAC_PCM_MAX_OUTPUT_CHANNELS, AUDIO_MAX_CH);
    d->has_sbr = lib_has_sbr();
    return d;
}

static int fdk_dec_decode(void *ctx, const uint8_t *au, size_t len, int16_t *pcm,
                          int *rate, int *channels, int *delay)
{
    struct fdk_dec *d = ctx;
    UCHAR *in = (UCHAR *)(uintptr_t)au;
    UINT size = (UINT)len, valid = (UINT)len;
    AAC_DECODER_ERROR e;
    CStreamInfo *si;

    if (aacDecoder_Fill(d->h, &in, &size, &valid) != AAC_DEC_OK)
        return -1;
    e = aacDecoder_DecodeFrame(d->h, pcm, AUDIO_MAX_FRAME * AUDIO_MAX_CH, 0);
    if (e == AAC_DEC_NOT_ENOUGH_BITS)
        return 0;
    if (e != AAC_DEC_OK) {
        /* Leave nothing of the bad unit behind for the next one to trip on. */
        aacDecoder_SetParam(d->h, AAC_TPDEC_CLEAR_BUFFER, 1);
        return -1;
    }
    si = aacDecoder_GetStreamInfo(d->h);
    if (!si || si->sampleRate <= 0 || si->numChannels < 1 ||
        si->numChannels > AUDIO_MAX_CH || si->frameSize <= 0 ||
        si->frameSize > AUDIO_MAX_FRAME)
        return -1;
    *rate = si->sampleRate;
    *channels = si->numChannels;
    /*
     * How far the samples just returned lag the unit just given. For
     * plain AAC that is nothing. For HE-AAC the library adds the delay of
     * its SBR stage and of its own post-processing, and reports the sum.
     * Of that, 962 samples are the SBR delay every HE-AAC decoder has:
     * receivers play such a stream with that lag and take it for granted,
     * so it is left in, and only the rest is taken out. That keeps our
     * output in step with what a viewer of the original would have heard
     * (measured against a reference decoder in the sync tests).
     */
    *delay = (int)si->outputDelay;
    if (si->aacSampleRate > 0 && si->sampleRate == 2 * si->aacSampleRate)
        *delay -= SBR_DELAY;
    if (*delay < 0)
        *delay = 0;
    if (!d->warned && !d->has_sbr && si->sampleRate <= 24000) {
        d->warned = true;
        log_warn("AAC at %d Hz: if this is HE-AAC, this build cannot decode its upper "
                 "band (the installed fdk-aac library has no SBR). The audio is "
                 "transcoded from the half-rate core only: nothing above %d kHz, and "
                 "HE-AAC v2 becomes mono. See \"HE-AAC\" in the README.",
                 si->sampleRate, si->sampleRate / 2000);
    }
    return si->frameSize;
}

static void fdk_dec_reset(void *ctx)
{
    struct fdk_dec *d = ctx;

    aacDecoder_SetParam(d->h, AAC_TPDEC_CLEAR_BUFFER, 1);
}

static void fdk_dec_close(void *ctx)
{
    struct fdk_dec *d = ctx;

    aacDecoder_Close(d->h);
    free(d);
}

static bool fdk_dec_handles(enum codec_id c)
{
    return c == CODEC_AAC || c == CODEC_AAC_LATM;
}

static const struct adec_ops fdk_dec_ops = {
    fdk_dec_open, fdk_dec_decode, fdk_dec_reset, fdk_dec_close,
};

static const char *fdk_dec_detail(void)
{
    return lib_has_sbr() ? " incl. HE-AAC" : " NO HE-AAC";
}

const struct codec_backend backend_fdk_dec = {
    .name = "fdk-aac", .codecs = "aac (ADTS, LATM)", .detail = fdk_dec_detail,
    .media = CODEC_AUDIO, .role = CODEC_DECODER, .where = WHERE_CPU,
    .priority = 10, .handles = fdk_dec_handles, .adec = &fdk_dec_ops,
};

/* ---- encoder -------------------------------------------------------- */

struct fdk_enc {
    HANDLE_AACENCODER h;
    int channels, frame;
};

static void *fdk_enc_open(enum codec_id codec, int rate, int channels, int *bitrate,
                          int *frame, int *delay, char *err, size_t errlen)
{
    struct fdk_enc *e = calloc(1, sizeof(*e));
    AACENC_InfoStruct info;
    AACENC_ERROR rc;

    (void)codec;
    if (!e)
        return NULL;
    if ((rc = aacEncOpen(&e->h, 0, (UINT)channels)) != AACENC_OK) {
        snprintf(err, errlen, "aacEncOpen failed (0x%x)", rc);
        free(e);
        return NULL;
    }
    if (aacEncoder_SetParam(e->h, AACENC_AOT, 2) != AACENC_OK ||            /* AAC-LC */
        aacEncoder_SetParam(e->h, AACENC_SAMPLERATE, (UINT)rate) != AACENC_OK ||
        aacEncoder_SetParam(e->h, AACENC_CHANNELMODE,
                            channels == 1 ? MODE_1 : MODE_2) != AACENC_OK ||
        aacEncoder_SetParam(e->h, AACENC_CHANNELORDER, 1) != AACENC_OK ||
        aacEncoder_SetParam(e->h, AACENC_BITRATE, (UINT)*bitrate) != AACENC_OK ||
        aacEncoder_SetParam(e->h, AACENC_TRANSMUX, TT_MP4_ADTS) != AACENC_OK ||
        aacEncoder_SetParam(e->h, AACENC_AFTERBURNER, 1) != AACENC_OK ||
        (rc = aacEncEncode(e->h, NULL, NULL, NULL, NULL)) != AACENC_OK ||
        aacEncInfo(e->h, &info) != AACENC_OK) {
        snprintf(err, errlen, "AAC-LC at %d Hz, %d channel(s), %d b/s is not "
                 "something the encoder accepts", rate, channels, *bitrate);
        aacEncClose(&e->h);
        free(e);
        return NULL;
    }
    *bitrate = (int)aacEncoder_GetParam(e->h, AACENC_BITRATE);
    e->channels = channels;
    e->frame = (int)info.frameLength;
    *frame = e->frame;
    *delay = (int)info.nDelay;
    return e;
}

static int fdk_enc_encode(void *ctx, const int16_t *pcm, uint8_t *out, size_t max)
{
    struct fdk_enc *e = ctx;
    AACENC_BufDesc ib = { 0 }, ob = { 0 };
    AACENC_InArgs ia = { 0 };
    AACENC_OutArgs oa = { 0 };
    void *iptr = (void *)(uintptr_t)pcm, *optr = out;
    INT iid = IN_AUDIO_DATA, oid = OUT_BITSTREAM_DATA;
    INT isize = e->frame * e->channels * (INT)sizeof(int16_t);
    INT iel = sizeof(int16_t), osize = (INT)max, oel = 1;

    ib.numBufs = 1;
    ib.bufs = &iptr;
    ib.bufferIdentifiers = &iid;
    ib.bufSizes = &isize;
    ib.bufElSizes = &iel;
    ob.numBufs = 1;
    ob.bufs = &optr;
    ob.bufferIdentifiers = &oid;
    ob.bufSizes = &osize;
    ob.bufElSizes = &oel;
    ia.numInSamples = e->frame * e->channels;
    if (aacEncEncode(e->h, &ib, &ob, &ia, &oa) != AACENC_OK)
        return -1;
    return oa.numOutBytes;
}

static void fdk_enc_close(void *ctx)
{
    struct fdk_enc *e = ctx;

    aacEncClose(&e->h);
    free(e);
}

static bool fdk_enc_handles(enum codec_id c)
{
    return c == CODEC_AAC;
}

static const struct aenc_ops fdk_enc_ops = {
    fdk_enc_open, fdk_enc_encode, fdk_enc_close,
};

const struct codec_backend backend_fdk_enc = {
    .name = "fdk-aac", .codecs = "aac (LC, ADTS)",
    .media = CODEC_AUDIO, .role = CODEC_ENCODER, .where = WHERE_CPU,
    .priority = 10, .handles = fdk_enc_handles, .aenc = &fdk_enc_ops,
};

#endif /* HAVE_FDK_AAC */
