/*
 * caricoder3 - H.265 encode on the CPU through libx265
 * Copyright (c) 2026 CariTech Solutions
 */
#ifdef HAVE_X265

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <x265.h>

#include "codec.h"
#include "log.h"

struct x5_enc {
    const x265_api *api;
    x265_param *param;
    x265_encoder *h;
    x265_picture *pic;
    int depth;
    int delay;
    int64_t frame90;            /* one picture, 90 kHz */
    bool flushing, done;
    bool have;
    struct vpacket out;
};

static void *x5_open(const struct venc_cfg *cfg, char *err, size_t errlen)
{
    const char *preset = cfg->preset ? cfg->preset : "veryfast";
    const x265_api *api = x265_api_get(cfg->depth);
    struct x5_enc *e;
    x265_param *p;
    int kbps = cfg->bitrate / 1000;
    double fps = cfg->fps_den > 0 ? (double)cfg->fps_num / cfg->fps_den : 25.0;

    if (!api) {
        snprintf(err, errlen, "this libx265 was built without %d-bit support", cfg->depth);
        return NULL;
    }
    e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->api = api;
    e->depth = cfg->depth;
    p = e->param = api->param_alloc();
    e->pic = api->picture_alloc();
    if (!p || !e->pic)
        goto fail;
    if (api->param_default_preset(p, preset, NULL) < 0) {
        snprintf(err, errlen, "x265 knows no preset '%s' (ultrafast, superfast, veryfast, "
                 "faster, fast, medium, slow ...)", preset);
        goto fail;
    }
    p->sourceWidth = cfg->width;
    p->sourceHeight = cfg->height;
    p->internalCsp = X265_CSP_I420;
    p->internalBitDepth = cfg->depth;
    p->fpsNum = (uint32_t)(cfg->fps_num > 0 ? cfg->fps_num : 25);
    p->fpsDenom = (uint32_t)(cfg->fps_den > 0 ? cfg->fps_den : 1);

    p->rc.rateControlMode = X265_RC_ABR;
    p->rc.bitrate = kbps;
    p->rc.vbvMaxBitrate = kbps;
    p->rc.vbvBufferSize = (int)((int64_t)kbps * cfg->vbv_ms / 1000);
    /* Key frames are asked for picture by picture (vframe.force_key); the
     * encoder's own interval is only a backstop, a little longer. */
    p->keyframeMax = (int)(fps * cfg->gop_ms / 1000.0) + 3;
    if (p->keyframeMax < 1)
        p->keyframeMax = 1;
    p->scenecutThreshold = 0;               /* no key frames of its own at scene cuts */
    p->bOpenGOP = 0;

    /* What a transport stream needs. */
    p->bAnnexB = 1;
    p->bRepeatHeaders = 1;
    p->bEnableAccessUnitDelimiters = 1;

    if (cfg->sar_num > 0 && cfg->sar_den > 0) {
        p->vui.aspectRatioIdc = X265_EXTENDED_SAR;
        p->vui.sarWidth = cfg->sar_num;
        p->vui.sarHeight = cfg->sar_den;
    }
    if ((cfg->primaries > 0 && cfg->primaries != 2) || (cfg->transfer > 0 && cfg->transfer != 2) ||
        (cfg->matrix > 0 && cfg->matrix != 2)) {
        p->vui.bEnableVideoSignalTypePresentFlag = 1;
        p->vui.bEnableColorDescriptionPresentFlag = 1;
        p->vui.colorPrimaries = cfg->primaries > 0 ? cfg->primaries : 2;
        p->vui.transferCharacteristics = cfg->transfer > 0 ? cfg->transfer : 2;
        p->vui.matrixCoeffs = cfg->matrix > 0 ? cfg->matrix : 2;
    }
    if (cfg->full_range) {
        p->vui.bEnableVideoSignalTypePresentFlag = 1;
        p->vui.bEnableVideoFullRangeFlag = 1;
    }
    p->logLevel = X265_LOG_NONE;

    e->h = api->encoder_open(p);
    if (!e->h) {
        snprintf(err, errlen, "x265 would not open at %dx%d, %d-bit, %d kb/s",
                 cfg->width, cfg->height, cfg->depth, kbps);
        goto fail;
    }
    api->encoder_parameters(e->h, p);       /* what it actually settled on */
    e->delay = p->lookaheadDepth + p->bframes + p->frameNumThreads + 2;
    e->frame90 = (int64_t)(90000.0 / fps + 0.5);
    api->picture_init(p, e->pic);
    if (cfg->interlaced)
        log_warn("x265: interlaced video is encoded as whole frames");
    return e;

fail:
    if (e->pic)
        api->picture_free(e->pic);
    if (e->param)
        api->param_free(e->param);
    free(e);
    return NULL;
}

static void take(struct x5_enc *e, x265_nal *nal, uint32_t n_nal, const x265_picture *out)
{
    size_t bytes = 0;

    if (n_nal == 0)
        return;
    /* The library lays the NAL units of one picture end to end. */
    for (uint32_t i = 0; i < n_nal; i++)
        bytes += nal[i].sizeBytes;
    e->out.data = nal[0].payload;
    e->out.len = bytes;
    {
        int64_t pts = out->pts, dts = out->dts;

        /* The decode time of the first pictures out is worked out by the
         * library from a value it only sets once it has been given a few
         * pictures. An encoder closed before that hands back whatever was
         * in memory. A decode time that is not shortly before its picture
         * is not believed. */
        if (dts > pts || dts < pts - 90000 * 10)
            dts = pts - 2 * e->frame90;
        e->out.pts = pts * STC_PER_90K;
        e->out.dts = dts * STC_PER_90K;
    }
    e->out.key = out->sliceType == X265_TYPE_IDR;
    e->have = true;
}

static int x5_push(void *ctx, const struct vframe *f)
{
    struct x5_enc *e = ctx;
    x265_picture out;
    x265_nal *nal = NULL;
    uint32_t n_nal = 0;
    int rc;

    if (!f) {
        e->flushing = true;
        return 0;
    }
    if (f->mem != VMEM_CPU || f->depth != e->depth)
        return -1;
    e->api->picture_init(e->param, e->pic);
    e->pic->bitDepth = e->depth;
    e->pic->colorSpace = X265_CSP_I420;
    for (int i = 0; i < 3; i++) {
        e->pic->planes[i] = f->plane[i];
        e->pic->stride[i] = f->stride[i];
    }
    e->pic->pts = f->pts / STC_PER_90K;
    e->pic->sliceType = f->force_key ? X265_TYPE_IDR : X265_TYPE_AUTO;
    rc = e->api->encoder_encode(e->h, &nal, &n_nal, e->pic, &out);
    if (rc < 0)
        return -1;
    if (rc > 0)
        take(e, nal, n_nal, &out);
    return 0;
}

static int x5_pull(void *ctx, struct vpacket *p)
{
    struct x5_enc *e = ctx;

    if (!e->have && e->flushing && !e->done) {
        x265_picture out;
        x265_nal *nal = NULL;
        uint32_t n_nal = 0;
        int rc = e->api->encoder_encode(e->h, &nal, &n_nal, NULL, &out);

        if (rc > 0)
            take(e, nal, n_nal, &out);
        else
            e->done = true;
    }
    if (!e->have)
        return 0;
    *p = e->out;
    e->have = false;
    return 1;
}

static int x5_delay(void *ctx)
{
    struct x5_enc *e = ctx;

    return e->delay;
}

static void x5_close(void *ctx)
{
    struct x5_enc *e = ctx;

    e->api->encoder_close(e->h);
    e->api->picture_free(e->pic);
    e->api->param_free(e->param);
    free(e);
}

static bool x5_handles(enum codec_id c)
{
    return c == CODEC_HEVC;
}

static const struct venc_ops x5_ops = { x5_open, x5_push, x5_pull, x5_delay, x5_close };

const struct codec_backend backend_x265 = {
    .name = "x265", .codecs = "hevc",
    .media = CODEC_VIDEO, .role = CODEC_ENCODER, .where = WHERE_CPU,
    .priority = 10, .handles = x5_handles, .venc = &x5_ops,
};

#endif /* HAVE_X265 */
