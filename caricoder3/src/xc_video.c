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
 * An encoder that holds to a buffer of B seconds at its bit rate R
 * promises this: if its output is delivered at R, starting B before the
 * first picture is due, every picture is complete in the receiver before
 * it is needed. So that is how the output is delivered: each access unit
 * takes (its size / R) to send (a little faster in fact, and faster still
 * when behind: see packet_out), one follows the other, and none starts
 * earlier than (its decode time - B - one picture). A large key frame is
 * therefore spread over the time it takes at R, not pushed out in one
 * picture's time, and the audio beside it is not held up.
 *
 * The cost is delay: an access unit can only be sent once it exists, and
 * it exists only after the decoder and the encoder have had the pictures
 * they hold back. --delay must cover that, plus B. The program reports
 * the time each unit had to spare ("slack"), which says how far --delay
 * could come down, or that it must go up.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "thumb.h"
#include "xcode.h"

#define DEFAULT_VBV_MS  1000
#define DEFAULT_GOP_MS  2000
#define RESTART_BACK    (2 * STC_HZ)    /* time going back further than this: a new start */
#define PROGRESSIVE_RUN 500             /* pictures in a row before an interlaced
                                         * encoder is given up for a progressive one */
#define SOFT_RUN        10              /* pictures in a row before a new colour
                                         * description or field order is believed */

struct vxc {
    struct xcode_cfg cfg;
    xcode_emit_fn emit;
    void *opaque;
    struct xcode_stats *st;

    const struct codec_backend *db, *eb;
    void *dctx, *ectx;
    bool want_cuda;
    bool enc_failed;
    bool fall_back;                     /* the card let us down: carry on on the CPU */
    bool dec_failed;                    /* the decoder cannot do this stream; said once */
    struct vframe e_fmt;                /* the picture the encoder was opened for */
    int progressive_run;
    int soft_run;
    int out_depth;
    int64_t rate;                       /* bits per second the encoder holds to */

    stc_t frame_dur;
    stc_t last_in;                      /* time of the last picture encoded */
    stc_t gop;                          /* key frames at multiples of this */
    int64_t last_gop_n;                 /* the multiple the last picture fell in */

    /* pictures scaled (and deinterlaced) on the CPU live here */
    uint8_t *scaled;
    size_t scaled_size;
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
    stc_t dts, floor, lag, vbv = (stc_t)v->vbv_ms * STC_PER_MS;
    int64_t pace;

    if (dts90 > pts90)
        dts90 = pts90;
    if (v->last_dts90 != NOPTS && dts90 <= v->last_dts90 &&
        v->last_dts90 - dts90 < 90000)
        dts90 = v->last_dts90 + 1;          /* decode times only move forward */
    v->last_dts90 = dts90;

    u = pes_unit_new(v->cfg.out_pid, 0xE0, p->len, pts90, dts90, &off);
    if (!u)
        return;
    memcpy(u->data + off, p->data, p->len);
    u->rai = p->key;

    /* Sent at the encoder's own bit rate, one after the other, and not
     * sooner than the buffer allows (see the top of this file). */
    dts = dts90 * STC_PER_90K;
    floor = dts - vbv - v->frame_dur;
    u->t0 = floor;
    if (u->t0 < v->last_t1 && v->last_t1 - u->t0 < vbv + RESTART_BACK)
        u->t0 = v->last_t1;
    /*
     * The pace. The encoder counts its own bytes, not the PES header put
     * in front of them here. It is a little above R (1.5 %). Being
     * behind the "not sooner than" line is normal: a large key frame
     * takes the schedule a good part of a buffer behind, and the encoder
     * has allowed for that. Only past three tenths of a buffer behind
     * does the pace rise, with the square of the excess (half a buffer
     * behind: 16 % more; a whole buffer: three times R). An encoder that
     * keeps to R is thus sent at R but for a short while after its
     * largest pictures; one that runs over its rate (x265 does, by a few
     * per cent, more when the rate is too low for the pictures) settles
     * some way behind the line instead of sliding until its pictures
     * come too late.
     */
    lag = u->t0 - floor;
    if (lag < 0)
        lag = 0;
    if (lag > vbv)
        lag = vbv;
    {
        double over = (double)lag / (double)vbv - 0.3;

        pace = v->rate + v->rate / 64 + 1;
        if (over > 0)
            pace += (int64_t)((double)v->rate * 4.0 * over * over);
    }
    u->t1 = u->t0 + (stc_t)((int64_t)p->len * 8 * STC_HZ / pace);
    /* An encoder that overran its buffer, or one just opened while the
     * last one's output is still going out: the unit must be there in
     * good time for its decode time all the same. */
    if (u->t1 > dts - v->frame_dur / 2)
        u->t1 = dts - v->frame_dur / 2;
    if (u->t1 <= u->t0)
        u->t1 = u->t0 + 1;
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
    c.depth = v->cfg.out == CODEC_HEVC && !(v->cfg.profile && !strcmp(v->cfg.profile, "main")) ? f->depth : 8;
    c.fps_num = f->fps_num > 0 && f->fps_den > 0 ? f->fps_num : 25;
    c.fps_den = f->fps_num > 0 && f->fps_den > 0 ? f->fps_den : 1;
    if ((double)c.fps_num / c.fps_den < 5 || (double)c.fps_num / c.fps_den > 240) {
        c.fps_num = 25;
        c.fps_den = 1;
    }
    c.sar_num = f->sar_num;
    c.sar_den = f->sar_den;
    /* A size rounded to even numbers leaves pixels a hair off square
     * (854x480 at 16:9 is 1280:1281): called square, as players expect. */
    if (c.sar_num > 0 && c.sar_den > 0 && c.sar_num != c.sar_den &&
        fabs((double)c.sar_num / c.sar_den - 1.0) < 0.005)
        c.sar_num = c.sar_den = 1;
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
    v->rate = rate;
    c.vbv_ms = v->vbv_ms;
    c.gop_ms = v->cfg.gop_ms > 0 ? v->cfg.gop_ms : DEFAULT_GOP_MS;
    c.preset = v->cfg.preset;
    c.bframes = v->cfg.bframes;
    c.profile = v->cfg.profile;
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
        v->enc_failed = true;
        if (f->mem == VMEM_CUDA && v->cfg.decoder == WHERE_AUTO &&
            v->cfg.encoder == WHERE_AUTO &&
            codec_find(CODEC_VIDEO, CODEC_ENCODER, v->cfg.out, WHERE_CPU, 0) &&
            codec_find(CODEC_VIDEO, CODEC_DECODER, v->cfg.in, WHERE_CPU, 0)) {
            /* The pictures are on a card whose encoder will not open, and
             * nobody insisted on the card. Decode again on the CPU. */
            log_warn("PID 0x%04X: falling back to the CPU for decoding and encoding",
                     v->cfg.pid);
            v->fall_back = true;
            return false;
        }
        log_err("PID 0x%04X: no encoder for %s could be opened at %dx%d; the stream "
                "is silent", v->cfg.pid, codec_name(v->cfg.out), f->width, f->height);
        v->st->failed = true;
        return false;
    }
    v->e_fmt = *f;
    v->st->width = f->width;
    v->st->height = f->height;
    v->progressive_run = 0;
    v->soft_run = 0;
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

/*
 * Is this picture of another kind than the encoder was opened for?
 * Size, depth and where the picture lives always count. The rest is what
 * the encoder writes into its headers: the shape of a pixel (a channel
 * going from 4:3 to 16:9), the frame rate, the colour description, and
 * whether the picture is two fields. A value the decoder does not know
 * (0) is no change.
 */
static bool format_changed(struct vxc *v, const struct vframe *f)
{
    const struct vframe *e = &v->e_fmt;

    if (f->width != e->width || f->height != e->height || f->depth != e->depth ||
        f->mem != e->mem)
        return true;
    if (f->sar_num > 0 && f->sar_den > 0 &&
        (int64_t)f->sar_num * e->sar_den != (int64_t)e->sar_num * f->sar_den)
        return true;
    if (f->fps_num > 0 && f->fps_den > 0 &&
        (int64_t)f->fps_num * e->fps_den != (int64_t)e->fps_num * f->fps_den)
        return true;
    /* The colour description and the field order can differ from one
     * picture to the next without the stream having changed (a decoder
     * that works on several pictures at once reports an SEI's colour on
     * some and the sequence's on others; film material alternates field
     * order). They count once they have stayed. */
    if (f->primaries != e->primaries || f->transfer != e->transfer ||
        f->matrix != e->matrix || f->full_range != e->full_range ||
        (f->interlaced && e->interlaced && f->tff != e->tff)) {
        if (++v->soft_run >= SOFT_RUN)
            return true;
    } else {
        v->soft_run = 0;
    }
    /* Field or frame is a flag on every picture, and interlaced streams
     * carry progressive pictures among the others. One interlaced
     * picture makes the stream interlaced; only a long run of progressive
     * ones makes it progressive again. */
    if (f->interlaced && !e->interlaced)
        return true;
    if (!f->interlaced && e->interlaced) {
        if (++v->progressive_run >= PROGRESSIVE_RUN)
            return true;
    } else {
        v->progressive_run = 0;
    }
    return false;
}

/*
 * Resampling one plane by area averaging: each output sample is the mean
 * of the input samples its footprint covers, with fractional weights at
 * the edges. Good for scaling down (every input sample counts once, no
 * aliasing); for scaling up it is a soft bilinear. Separable: rows first
 * into a line buffer, then columns. Samples are 8-bit, or 16-bit words
 * holding 10 bits.
 */
struct span { int from, n; float w[4]; };   /* up to 4 inputs per output */

static void spans(int in_n, int out_n, struct span *sp)
{
    double step = (double)in_n / out_n;

    for (int o = 0; o < out_n; o++) {
        double a = o * step, b = a + step;
        int i0 = (int)a, i1 = (int)(b - 1e-9);
        float sum = 0;

        if (i1 >= in_n)
            i1 = in_n - 1;
        if (i1 - i0 > 3)
            i1 = i0 + 3;                    /* more than 4:1 is capped: good enough */
        sp[o].from = i0;
        sp[o].n = i1 - i0 + 1;
        for (int i = i0; i <= i1; i++) {
            double lo = i > a ? i : a, hi = i + 1 < b ? i + 1 : b;

            sp[o].w[i - i0] = (float)(hi - lo);
            sum += sp[o].w[i - i0];
        }
        for (int k = 0; k < sp[o].n; k++)
            sp[o].w[k] /= sum;
    }
}

static void scale_plane(const uint8_t *src, int sstride, int sw, int sh, bool wide,
                        uint8_t *dst, int dstride, int dw, int dh,
                        struct span *sx, struct span *sy, float *line, bool blend)
{
    spans(sw, dw, sx);
    spans(sh, dh, sy);
    for (int y = 0; y < dh; y++) {
        memset(line, 0, sizeof(float) * (size_t)dw);
        for (int k = 0; k < sy[y].n; k++) {
            int yy = sy[y].from + k;
            float wy = sy[y].w[k];
            /* field blend: an interlaced source is read as the mean of
             * each line and the next, which removes the comb */
            for (int half = 0; half < (blend ? 2 : 1); half++) {
                int ys = yy + half < sh ? yy + half : sh - 1;
                const uint8_t *row = src + (size_t)ys * sstride;
                float wr = blend ? wy * 0.5f : wy;

                for (int x = 0; x < dw; x++) {
                    float acc = 0;

                    for (int j = 0; j < sx[x].n; j++) {
                        int xs = sx[x].from + j;
                        float sv = wide ? (float)(row[2 * xs] | (row[2 * xs + 1] << 8))
                                        : (float)row[xs];

                        acc += sv * sx[x].w[j];
                    }
                    line[x] += acc * wr;
                }
            }
        }
        if (wide) {
            uint8_t *d = dst + (size_t)y * dstride;

            for (int x = 0; x < dw; x++) {
                unsigned val = (unsigned)(line[x] + 0.5f);

                d[2 * x] = (uint8_t)val;
                d[2 * x + 1] = (uint8_t)(val >> 8);
            }
        } else {
            uint8_t *d = dst + (size_t)y * dstride;

            for (int x = 0; x < dw; x++)
                d[x] = (uint8_t)(line[x] + 0.5f);
        }
    }
}

/* The picture at the rendition's size, deinterlaced if it was two fields. */
static bool scale_picture(struct vxc *v, const struct vframe *in, struct vframe *out, int dw, int dh)
{
    int cw = (dw + 1) / 2, ch = (dh + 1) / 2;
    int bps = in->depth > 8 ? 2 : 1;
    size_t need = ((size_t)dw * dh + 2 * (size_t)cw * ch) * (size_t)bps;
    size_t extra = sizeof(struct span) * ((size_t)dw + dh) + sizeof(float) * (size_t)dw;
    struct span *sx, *sy;
    float *line;
    uint8_t *dst;

    if (need + extra > v->scaled_size) {
        uint8_t *n = realloc(v->scaled, need + extra);

        if (!n)
            return false;
        v->scaled = n;
        v->scaled_size = need + extra;
    }
    dst = v->scaled;
    sx = (struct span *)(v->scaled + need);
    sy = sx + dw;
    line = (float *)(sy + dh);
    *out = *in;
    out->width = dw;
    out->height = dh;
    out->interlaced = false;
    /* The shape of a pixel follows the picture: the display shape stays. */
    if (in->sar_num > 0 && in->sar_den > 0) {
        int64_t n = (int64_t)in->sar_num * in->width * dh;
        int64_t d = (int64_t)in->sar_den * in->height * dw;
        int64_t g = n, t = d;

        while (t) {                         /* Euclid */
            int64_t r = g % t;

            g = t;
            t = r;
        }
        if (g < 1)
            g = 1;
        if (n / g < 65536 && d / g < 65536) {
            out->sar_num = (int)(n / g);
            out->sar_den = (int)(d / g);
        }
    }
    for (int pl = 0; pl < 3; pl++) {
        int sw = pl ? (in->width + 1) / 2 : in->width;
        int sh = pl ? (in->height + 1) / 2 : in->height;
        int w = pl ? cw : dw, h = pl ? ch : dh;

        out->plane[pl] = dst;
        out->stride[pl] = w * bps;
        scale_plane(in->plane[pl], in->stride[pl], sw, sh, bps == 2, dst, w * bps, w, h,
                    sx, sy, line, in->interlaced);
        dst += (size_t)w * h * bps;
    }
    return true;
}

static void picture_in(struct vxc *v, const struct vframe *in)
{
    struct vframe f = *in, tmp;

    v->st->frames_in++;
    if (v->cfg.first_rendition)
        thumb_offer(in);
    /* Another size wanted and the decoder did not make it: scaled here. */
    if (f.mem == VMEM_CPU && (v->cfg.width || v->cfg.height)) {
        int tw, th;

        video_size(v->cfg.width, v->cfg.height, v->cfg.fit, f.width, f.height,
                   f.sar_num > 0 && f.sar_den > 0 ? f.width * f.sar_num : 0,
                   f.sar_num > 0 && f.sar_den > 0 ? f.height * f.sar_den : 0, &tw, &th);
        if (f.width != tw || f.height != th) {
            if (!scale_picture(v, &f, &tmp, tw, th))
                return;
            f = tmp;
        }
    }
    if (v->ectx && format_changed(v, &f)) {
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
    /* A key frame wherever the picture's own time crosses a multiple of
     * the key-frame interval: every rendition of the channel, on any
     * machine, keys on the same picture (see docs/DESIGN.md, stage 4). */
    {
        int64_t n = f.pts / v->gop;

        if (f.pts < 0)
            n--;
        if (n != v->last_gop_n) {
            f.force_key = true;
            v->last_gop_n = n;
        }
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

/* Replaces the decoder by the one for the CPU. False if there is none or
 * it will not open; then the object stays whole but has no decoder. */
static bool decoder_to_cpu(struct vxc *v)
{
    struct vdec_cfg dc = { .codec = v->cfg.in, .width = v->cfg.width, .height = v->cfg.height,
                           .gpu = v->cfg.gpu, .fit = v->cfg.fit };
    const struct codec_backend *b =
        codec_find(CODEC_VIDEO, CODEC_DECODER, v->cfg.in, WHERE_CPU, 0);
    char err[160] = "";

    v->db->vdec->close(v->dctx);
    v->dctx = b ? b->vdec->open(&dc, err, sizeof(err)) : NULL;
    if (!v->dctx) {
        log_err("PID 0x%04X: %s: %s; the stream is silent", v->cfg.pid,
                b ? b->name : "no decoder for the CPU", err);
        v->st->failed = true;
        return false;
    }
    v->db = b;
    v->cfg.decoder = WHERE_CPU;
    v->want_cuda = false;
    snprintf(v->st->decoder, sizeof(v->st->decoder), "%s", b->name);
    return true;
}

void vxc_unit(struct vxc *v, const struct es_unit *u)
{
    int off = pes_payload_offset(u->data, u->len);
    int rc;

    v->st->units_in++;
    if (off < 0 || !v->dctx)
        return;
    rc = v->db->vdec->push(v->dctx, u->data + off, u->len - (size_t)off, u->pts, u->dts);
    if (rc == VDEC_CANNOT) {
        if (v->db->where != WHERE_CPU && v->cfg.decoder == WHERE_AUTO &&
            codec_find(CODEC_VIDEO, CODEC_DECODER, v->cfg.in, WHERE_CPU, 0)) {
            /* The card will not decode this stream, and nobody insisted
             * on the card. The CPU decodes it, starting with this very
             * unit; the encoder stays where it is if it can. */
            log_warn("PID 0x%04X: %s cannot decode this stream; decoding on the CPU",
                     v->cfg.pid, v->db->name);
            if (!decoder_to_cpu(v))
                return;
            rc = v->db->vdec->push(v->dctx, u->data + off, u->len - (size_t)off, u->pts,
                                   u->dts);
        } else if (!v->dec_failed) {
            v->dec_failed = true;
            v->st->failed = true;
            log_err("PID 0x%04X: %s cannot decode this stream; the stream is silent",
                    v->cfg.pid, v->db->name);
        }
    } else if (v->dec_failed && rc == 0) {
        v->dec_failed = false;              /* the stream changed to one it takes */
        v->st->failed = v->enc_failed;
    }
    if (rc < 0)
        v->st->errors++;
    drain_decoder(v);

    if (v->fall_back) {
        /* The pictures are on a card whose encoder will not open (see
         * encoder_open). The pictures in this unit are gone with the old
         * decoder; the new one starts at the next key frame. */
        v->fall_back = false;
        if (!decoder_to_cpu(v))
            return;
        v->cfg.encoder = WHERE_CPU;
        v->enc_failed = false;
    }
}

bool vxc_last_out(const struct vxc *v, int64_t *dts90, stc_t *t1)
{
    if (v->last_dts90 == NOPTS)
        return false;
    *dts90 = v->last_dts90;
    *t1 = v->last_t1;
    return true;
}

void vxc_start_after(struct vxc *v, int64_t dts90, stc_t t1)
{
    v->last_dts90 = dts90;
    v->last_t1 = t1;
}

/* Input was thrown away: what the decoder holds refers to pictures it
 * will never get. */
void vxc_reset(struct vxc *v)
{
    if (v->dctx)
        v->db->vdec->reset(v->dctx);
}

void vxc_finish(struct vxc *v)
{
    if (v->dctx) {
        v->db->vdec->drain(v->dctx);
        drain_decoder(v);
    }
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
    v->gop = (stc_t)(cfg->gop_ms > 0 ? cfg->gop_ms : DEFAULT_GOP_MS) * STC_PER_MS;
    v->last_gop_n = INT64_MIN;
    v->want_cuda = eb->where == WHERE_NVIDIA;

    /* The decoder asked for, or the best that opens. */
    for (int skip = 0; !v->dctx; skip++) {
        /* H.264 goes out at 8 bits (see encoder_open). */
        struct vdec_cfg dc = { .codec = cfg->in, .width = cfg->width, .height = cfg->height,
                               .gpu = cfg->gpu, .fit = cfg->fit, .want_cuda = v->want_cuda,
                               .only_8bit = cfg->out == CODEC_H264 ||
                                            (cfg->profile && !strcmp(cfg->profile, "main")) };
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
    free(v->scaled);
    free(v);
}
