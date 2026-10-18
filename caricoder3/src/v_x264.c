/*
 * caricoder3 - H.264 encode on the CPU through libx264
 * Copyright (c) 2026 CariTech Solutions
 */
#ifdef HAVE_X264

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <x264.h>

#include "codec.h"
#include "log.h"

struct x4_enc {
    x264_t *h;
    int depth;
    bool flushing;
    bool have;                      /* `out` holds an access unit not yet taken */
    struct vpacket out;
};

static void x4_log(void *opaque, int level, const char *fmt, va_list ap)
{
    char buf[256];

    (void)opaque;
    if (level > X264_LOG_WARNING)
        return;
    vsnprintf(buf, sizeof(buf), fmt, ap);
    buf[strcspn(buf, "\n")] = 0;
    if (level <= X264_LOG_ERROR)
        log_err("x264: %s", buf);
    else
        log_dbg("x264: %s", buf);
}

static void *x4_open(const struct venc_cfg *cfg, char *err, size_t errlen)
{
    struct x4_enc *e;
    x264_param_t p;
    const char *preset = cfg->preset ? cfg->preset : "veryfast";
    int kbps = cfg->bitrate / 1000;
    double fps = cfg->fps_den > 0 ? (double)cfg->fps_num / cfg->fps_den : 25.0;

    if (cfg->depth != 8 && cfg->depth != 10) {
        snprintf(err, errlen, "x264: %d-bit video is not supported", cfg->depth);
        return NULL;
    }
    if (x264_param_default_preset(&p, preset, NULL) < 0) {
        snprintf(err, errlen, "x264 knows no preset '%s' (ultrafast, superfast, veryfast, "
                 "faster, fast, medium, slow ...)", preset);
        return NULL;
    }
    p.i_csp = X264_CSP_I420 | (cfg->depth > 8 ? X264_CSP_HIGH_DEPTH : 0);
    p.i_bitdepth = cfg->depth;
    p.i_width = cfg->width;
    p.i_height = cfg->height;
    p.i_fps_num = (uint32_t)(cfg->fps_num > 0 ? cfg->fps_num : 25);
    p.i_fps_den = (uint32_t)(cfg->fps_den > 0 ? cfg->fps_den : 1);
    /* Time stamps are passed through in 90 kHz units and are what the
     * rate control goes by, so a source that skips or stutters is coded
     * for the time it really takes. */
    /* x264 on its own starts one and a half threads per processor, and
     * holds back one more picture for every thread: on a large server
     * that is seconds of delay, for speed a single stream cannot use. */
    p.i_threads = codec_threads(CODEC_THREADS_X264);
    p.b_vfr_input = 1;
    p.i_timebase_num = 1;
    p.i_timebase_den = 90000;

    p.rc.i_rc_method = X264_RC_ABR;
    p.rc.i_bitrate = kbps;
    p.rc.i_vbv_max_bitrate = kbps;
    p.rc.i_vbv_buffer_size = (int)((int64_t)kbps * cfg->vbv_ms / 1000);
    /* Key frames are asked for picture by picture (vframe.force_key), at
     * the same instants in every rendition of a channel; the encoder's
     * own interval is only a backstop, a little longer. */
    p.i_keyint_max = (int)(fps * cfg->gop_ms / 1000.0) + 3;
    if (p.i_keyint_max < 1)
        p.i_keyint_max = 1;
    p.i_scenecut_threshold = 0;             /* no key frames of its own at scene cuts */
    p.b_open_gop = 0;

    /* What a transport stream needs: start codes, parameter sets with
     * every key frame, and an access unit delimiter on each picture. */
    p.b_annexb = 1;
    p.b_repeat_headers = 1;
    p.b_aud = 1;

    p.b_interlaced = cfg->interlaced;
    p.b_tff = cfg->tff;
    if (cfg->sar_num > 0 && cfg->sar_den > 0) {
        p.vui.i_sar_width = cfg->sar_num;
        p.vui.i_sar_height = cfg->sar_den;
    }
    if (cfg->primaries > 0 && cfg->primaries != 2)
        p.vui.i_colorprim = cfg->primaries;
    if (cfg->transfer > 0 && cfg->transfer != 2)
        p.vui.i_transfer = cfg->transfer;
    if (cfg->matrix > 0 && cfg->matrix != 2)
        p.vui.i_colmatrix = cfg->matrix;
    p.vui.b_fullrange = cfg->full_range;

    p.pf_log = x4_log;
    p.i_log_level = X264_LOG_WARNING;
    if (cfg->bframes >= 0)
        p.i_bframe = cfg->bframes;
    {
        /* baseline has no B-frames: x264 takes them out itself */
        const char *prof = cfg->depth > 8 ? "high10" : cfg->profile && strcmp(cfg->profile, "main10") ? cfg->profile : "high";

        if (x264_param_apply_profile(&p, prof) < 0) {
            snprintf(err, errlen, "x264 would not take the profile '%s'", prof);
            return NULL;
        }
    }

    e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->depth = cfg->depth;
    e->h = x264_encoder_open(&p);
    if (!e->h) {
        snprintf(err, errlen, "x264 would not open at %dx%d, %d-bit, %d kb/s",
                 cfg->width, cfg->height, cfg->depth, kbps);
        free(e);
        return NULL;
    }
    return e;
}

static void take(struct x4_enc *e, x264_nal_t *nal, int n_nal, int bytes,
                 const x264_picture_t *out)
{
    if (bytes <= 0 || n_nal <= 0)
        return;
    /* The library lays the NAL units of one picture end to end. */
    e->out.data = nal[0].p_payload;
    e->out.len = (size_t)bytes;
    e->out.pts = out->i_pts * STC_PER_90K;
    e->out.dts = out->i_dts * STC_PER_90K;
    e->out.key = out->b_keyframe != 0;
    e->have = true;
}

static int x4_push(void *ctx, const struct vframe *f)
{
    struct x4_enc *e = ctx;
    x264_picture_t pic, out;
    x264_nal_t *nal = NULL;
    int n_nal = 0, bytes;

    if (!f) {
        e->flushing = true;
        return 0;
    }
    if (f->mem != VMEM_CPU || f->depth != e->depth)
        return -1;
    x264_picture_init(&pic);
    pic.img.i_csp = X264_CSP_I420 | (e->depth > 8 ? X264_CSP_HIGH_DEPTH : 0);
    pic.img.i_plane = 3;
    for (int i = 0; i < 3; i++) {
        pic.img.plane[i] = f->plane[i];
        pic.img.i_stride[i] = f->stride[i];
    }
    pic.i_pts = f->pts / STC_PER_90K;
    if (f->force_key)
        pic.i_type = X264_TYPE_IDR;
    bytes = x264_encoder_encode(e->h, &nal, &n_nal, &pic, &out);
    if (bytes < 0)
        return -1;
    take(e, nal, n_nal, bytes, &out);
    return 0;
}

static int x4_pull(void *ctx, struct vpacket *p)
{
    struct x4_enc *e = ctx;

    /* Emptying the encoder: a call may come back with nothing while
     * pictures are still inside (its threads have not finished one), so
     * it is asked until it has none left, as the library's own examples
     * do. Stopping at the first empty answer could lose the last
     * pictures. */
    while (!e->have && e->flushing && x264_encoder_delayed_frames(e->h) > 0) {
        x264_picture_t out;
        x264_nal_t *nal = NULL;
        int n_nal = 0;
        int bytes = x264_encoder_encode(e->h, &nal, &n_nal, NULL, &out);

        if (bytes < 0)
            break;
        take(e, nal, n_nal, bytes, &out);
    }
    if (!e->have)
        return 0;
    *p = e->out;
    e->have = false;
    return 1;
}

static int x4_delay(void *ctx)
{
    struct x4_enc *e = ctx;

    return x264_encoder_maximum_delayed_frames(e->h);
}

static void x4_close(void *ctx)
{
    struct x4_enc *e = ctx;

    x264_encoder_close(e->h);
    free(e);
}

static bool x4_handles(enum codec_id c)
{
    return c == CODEC_H264;
}

static const struct venc_ops x4_ops = { x4_open, x4_push, x4_pull, x4_delay, x4_close };

const struct codec_backend backend_x264 = {
    .name = "x264", .codecs = "h264",
    .media = CODEC_VIDEO, .role = CODEC_ENCODER, .where = WHERE_CPU,
    .priority = 10, .handles = x4_handles, .venc = &x4_ops,
};

#endif /* HAVE_X264 */
