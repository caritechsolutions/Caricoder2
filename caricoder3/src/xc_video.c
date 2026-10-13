/*
 * caricoder3 - video transcoding
 * Copyright (c) 2026 CariTech Solutions
 *
 * PES packets -> decoder -> pictures -> encoder -> access units -> PES
 * packets. Picture size and frame rate are left as they are (scaling and
 * deinterlacing belong to a later stage); time stamps pass straight
 * through, so the video stays where it was against the audio.
 *
 * When each new access unit is sent
 * ---------------------------------
 * An encoder that holds to a buffer of B seconds at its bit rate promises
 * that a decoder which starts a picture's data arriving B before the
 * picture is due will always have it complete in time. So each access
 * unit is given to the mux to send from (its decode time - B - one
 * picture), spread over one picture's duration. That is the lead a
 * receiver sees on the transcoded video, whatever the source had.
 *
 * The cost is delay: an access unit can only be sent once it exists, and
 * it exists only after the decoder and the encoder have had the pictures
 * they hold back. --delay must cover that, plus B. The program reports
 * the time each unit had to spare ("slack"), which says how far --delay
 * could come down, or that it must go up.
 */
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "xcode.h"

#define DEFAULT_VBV_MS  1000
#define DEFAULT_GOP_MS  2000
#define RESTART_BACK    (2 * STC_HZ)    /* time going back further than this: a new start */
#define FRAME_DUR_MAX   (STC_HZ / 5)

struct vxc {
    struct xcode_cfg cfg;
    xcode_emit_fn emit;
    void *opaque;
    struct xcode_stats *st;

    const struct codec_backend *db, *eb;
    void *dctx, *ectx;
    bool want_cuda;
    bool enc_failed;
    int e_width, e_height, e_depth;     /* what the encoder was opened for */
    enum vframe_mem e_mem;
    int out_depth;

    stc_t frame_dur;
    stc_t last_in;                      /* time of the last picture encoded */
    stc_t last_t1;
    int64_t last_dts90;
    int vbv_ms;

    /* pictures converted from 10 to 8 bits live here */
    uint8_t *conv;
    size_t conv_size;
};

/* ---- output --------------------------------------------------------- */

static void packet_out(struct vxc *v, const struct vpacket *p)
{
    int64_t pts90 = (p->pts + STC_PER_90K / 2) / STC_PER_90K;
    int64_t dts90 = (p->dts + STC_PER_90K / 2) / STC_PER_90K;
    struct es_unit *u;
    size_t off;
    stc_t dts;

    if (dts90 > pts90)
        dts90 = pts90;
    if (v->last_dts90 != NOPTS && dts90 <= v->last_dts90 &&
        v->last_dts90 - dts90 < 90000)
        dts90 = v->last_dts90 + 1;          /* decode times only move forward */
    v->last_dts90 = dts90;

    u = pes_unit_new(v->cfg.pid, 0xE0, p->len, pts90, dts90, &off);
    if (!u)
        return;
    memcpy(u->data + off, p->data, p->len);
    u->rai = p->key;

    dts = dts90 * STC_PER_90K;
    u->t0 = dts - (stc_t)v->vbv_ms * STC_PER_MS - v->frame_dur;
    u->t1 = u->t0 + v->frame_dur;
    if (u->t0 < v->last_t1 && v->last_t1 - u->t0 < STC_HZ) {
        u->t0 = v->last_t1;
        if (u->t1 <= u->t0)
            u->t1 = u->t0 + 1;
    }
    v->last_t1 = u->t1;
    v->st->frames_out++;
    v->st->units_out++;
    v->emit(v->opaque, u);
}

static void drain_encoder(struct vxc *v)
{
    struct vpacket p;

    while (v->ectx && v->eb->venc->pull(v->ectx, &p) == 1)
        packet_out(v, &p);
}

static void encoder_close(struct vxc *v)
{
    if (!v->ectx)
        return;
    v->eb->venc->push(v->ectx, NULL);       /* let out what it holds back */
    drain_encoder(v);
    v->eb->venc->close(v->ectx);
    v->ectx = NULL;
}

/* ---- pictures ------------------------------------------------------- */

static bool encoder_open(struct vxc *v, const struct vframe *f)
{
    struct venc_cfg c;
    char err[200] = "";
    int64_t rate = v->cfg.bitrate;

    memset(&c, 0, sizeof(c));
    c.codec = v->cfg.out;
    c.width = f->width;
    c.height = f->height;
    /* H.264 goes out at 8 bits whatever comes in: that is what receivers
     * take. H.265 keeps the depth of the source. */
    c.depth = v->cfg.out == CODEC_HEVC ? f->depth : 8;
    c.fps_num = f->fps_num > 0 && f->fps_den > 0 ? f->fps_num : 25;
    c.fps_den = f->fps_num > 0 && f->fps_den > 0 ? f->fps_den : 1;
    if ((double)c.fps_num / c.fps_den < 5 || (double)c.fps_num / c.fps_den > 240) {
        c.fps_num = 25;
        c.fps_den = 1;
    }
    c.sar_num = f->sar_num;
    c.sar_den = f->sar_den;
    c.interlaced = f->interlaced;
    c.tff = f->tff;
    c.primaries = f->primaries;
    c.transfer = f->transfer;
    c.matrix = f->matrix;
    c.full_range = f->full_range;
    if (rate <= 0) {
        /* A tenth of a bit per pixel: about 5 Mb/s for 1080p25. */
        rate = (int64_t)f->width * f->height * c.fps_num / c.fps_den / 10;
        if (rate < 300000)
            rate = 300000;
    }
    c.bitrate = (int)rate;
    c.vbv_ms = v->vbv_ms;
    c.gop_ms = v->cfg.gop_ms > 0 ? v->cfg.gop_ms : DEFAULT_GOP_MS;
    c.preset = v->cfg.preset;
    c.gpu = v->cfg.gpu;
    c.cuda_ctx = f->cuda_ctx;

    v->frame_dur = STC_HZ * c.fps_den / c.fps_num;

    /* The encoder asked for, or the best there is; and if that one will
     * not open, the next, as long as it can take pictures from where
     * this decoder leaves them. */
    for (int skip = 0; ; skip++) {
        const struct codec_backend *b =
            codec_find(CODEC_VIDEO, CODEC_ENCODER, v->cfg.out, v->cfg.encoder, skip);

        if (!b)
            break;
        if (f->mem == VMEM_CUDA && b->where != WHERE_NVIDIA)
            continue;
        v->ectx = b->venc->open(&c, err, sizeof(err));
        if (v->ectx) {
            v->eb = b;
            break;
        }
        log_warn("PID 0x%04X: %s: %s", v->cfg.pid, b->name, err);
    }
    if (!v->ectx) {
        log_err("PID 0x%04X: no encoder for %s could be opened at %dx%d; the stream "
                "is silent", v->cfg.pid, codec_name(v->cfg.out), f->width, f->height);
        v->enc_failed = true;
        v->st->failed = true;
        return false;
    }
    v->e_width = f->width;
    v->e_height = f->height;
    v->e_depth = f->depth;
    v->e_mem = f->mem;
    v->out_depth = c.depth;
    v->enc_failed = false;
    v->st->failed = false;
    snprintf(v->st->encoder, sizeof(v->st->encoder), "%s", v->eb->name);
    log_info("PID 0x%04X: video %s -> %s, %dx%d%s, %.3f fps, %d-bit, %.2f Mb/s, "
             "key frame every %d ms (%s -> %s, holds back up to %d pictures)",
             v->cfg.pid, codec_name(v->cfg.in), codec_name(v->cfg.out), f->width, f->height,
             f->interlaced ? " interlaced" : "", (double)c.fps_num / c.fps_den, c.depth,
             c.bitrate / 1e6, c.gop_ms, v->st->decoder, v->eb->name,
             v->eb->venc->delay_frames(v->ectx));
    return true;
}

/* 10-bit samples (two bytes each) to 8-bit, rounded. */
static bool to_8bit(struct vxc *v, const struct vframe *in, struct vframe *out)
{
    int cw = (in->width + 1) / 2, ch = (in->height + 1) / 2;
    size_t need = (size_t)in->width * in->height + 2 * (size_t)cw * ch;
    uint8_t *dst;

    if (need > v->conv_size) {
        uint8_t *n = realloc(v->conv, need);

        if (!n)
            return false;
        v->conv = n;
        v->conv_size = need;
    }
    *out = *in;
    out->depth = 8;
    dst = v->conv;
    for (int pl = 0; pl < 3; pl++) {
        int w = pl ? cw : in->width, h = pl ? ch : in->height;

        out->plane[pl] = dst;
        out->stride[pl] = w;
        for (int y = 0; y < h; y++) {
            const uint8_t *s = in->plane[pl] + (size_t)y * in->stride[pl];

            for (int x = 0; x < w; x++) {
                unsigned val = (unsigned)s[2 * x] | ((unsigned)s[2 * x + 1] << 8);

                val = (val + 2) >> 2;
                dst[x] = (uint8_t)(val > 255 ? 255 : val);
            }
            dst += w;
        }
    }
    return true;
}

static void picture_in(struct vxc *v, const struct vframe *in)
{
    struct vframe f = *in, tmp;

    v->st->frames_in++;
    if (v->ectx && (f.width != v->e_width || f.height != v->e_height ||
                    f.depth != v->e_depth || f.mem != v->e_mem)) {
        log_info("PID 0x%04X: the picture format changed", v->cfg.pid);
        encoder_close(v);
        v->enc_failed = false;
    }
    if (!v->ectx) {
        if (v->enc_failed || !encoder_open(v, &f))
            return;
        v->last_in = NOPTS;
    }

    /* The encoder needs a time for every picture, and each later than
     * the last. */
    if (f.pts == NOPTS) {
        if (v->last_in == NOPTS)
            return;
        f.pts = v->last_in + v->frame_dur;
    }
    if (v->last_in != NOPTS) {
        stc_t step = f.pts - v->last_in;

        if (step < -RESTART_BACK) {
            /* Time has gone back a long way: a new start. */
            v->st->restarts++;
            encoder_close(v);
            if (!encoder_open(v, &f))
                return;
            v->last_t1 = 0;
            v->last_dts90 = NOPTS;
        } else if (step < STC_PER_90K) {
            v->st->errors++;                /* a repeat, or out of order: let it go */
            return;
        }
    }
    v->last_in = f.pts;

    if (f.mem == VMEM_CPU && f.depth != v->out_depth) {
        if (f.depth != 10 || v->out_depth != 8 || !to_8bit(v, &f, &tmp))
            return;
        f = tmp;
    }
    if (v->eb->venc->push(v->ectx, &f) < 0)
        v->st->errors++;
    drain_encoder(v);
}

/* ---- input ---------------------------------------------------------- */

static void drain_decoder(struct vxc *v)
{
    struct vframe f;

    while (v->db->vdec->pull(v->dctx, &f) == 1)
        picture_in(v, &f);
}

void vxc_unit(struct vxc *v, const struct es_unit *u)
{
    int off = pes_payload_offset(u->data, u->len);

    v->st->units_in++;
    if (off < 0)
        return;
    if (v->db->vdec->push(v->dctx, u->data + off, u->len - (size_t)off, u->pts,
                          u->dts) < 0)
        v->st->errors++;
    drain_decoder(v);
}

void vxc_finish(struct vxc *v)
{
    v->db->vdec->drain(v->dctx);
    drain_decoder(v);
    encoder_close(v);
}

struct vxc *vxc_new(const struct xcode_cfg *cfg, xcode_emit_fn emit, void *opaque,
                    struct xcode_stats *st, char *err, size_t errlen)
{
    const struct codec_backend *eb;
    struct vxc *v;
    char why[160] = "";

    eb = codec_find(CODEC_VIDEO, CODEC_ENCODER, cfg->out, cfg->encoder, 0);
    if (!eb) {
        snprintf(err, errlen, "no usable %s encoder%s", codec_name(cfg->out),
                 cfg->encoder == WHERE_NVIDIA ? " on an NVIDIA card" :
                 cfg->encoder == WHERE_CPU ? " for the CPU" : "");
        return NULL;
    }
    v = calloc(1, sizeof(*v));
    if (!v)
        return NULL;
    v->cfg = *cfg;
    v->emit = emit;
    v->opaque = opaque;
    v->st = st;
    v->last_in = NOPTS;
    v->last_dts90 = NOPTS;
    v->frame_dur = STC_HZ / 25;
    v->vbv_ms = cfg->vbv_ms > 0 ? cfg->vbv_ms : DEFAULT_VBV_MS;
    v->want_cuda = eb->where == WHERE_NVIDIA;

    /* The decoder asked for, or the best that opens. */
    for (int skip = 0; !v->dctx; skip++) {
        struct vdec_cfg dc = { cfg->in, cfg->gpu, v->want_cuda };
        const struct codec_backend *b =
            codec_find(CODEC_VIDEO, CODEC_DECODER, cfg->in, cfg->decoder, skip);

        if (!b)
            break;
        v->dctx = b->vdec->open(&dc, why, sizeof(why));
        if (v->dctx)
            v->db = b;
        else
            log_warn("PID 0x%04X: %s: %s", cfg->pid, b->name, why);
    }
    if (!v->dctx) {
        snprintf(err, errlen, "no usable %s decoder%s%s%s", codec_name(cfg->in),
                 cfg->decoder == WHERE_NVIDIA ? " on an NVIDIA card" :
                 cfg->decoder == WHERE_CPU ? " for the CPU" : "",
                 why[0] ? ": " : "", why);
        free(v);
        return NULL;
    }
    snprintf(st->decoder, sizeof(st->decoder), "%s", v->db->name);
    snprintf(st->encoder, sizeof(st->encoder), "%s", eb->name);
    return v;
}

void vxc_free(struct vxc *v)
{
    if (!v)
        return;
    if (v->ectx)
        v->eb->venc->close(v->ectx);
    if (v->dctx)
        v->db->vdec->close(v->dctx);
    free(v->conv);
    free(v);
}
