/*
 * caricoder3 - audio transcoding
 * Copyright (c) 2026 CariTech Solutions
 *
 * PES packets -> access units -> samples -> access units -> PES packets.
 *
 * Time is kept by counting samples, not by trusting each time stamp on
 * its own. The first block of samples fixes an origin; from then on a
 * block is expected where the samples before it end. A stamp that agrees
 * to within a few milliseconds changes nothing, so rounding in the source
 * cannot make the output jitter. One that is later means audio is missing
 * (a lost packet): the hole is filled with silence, so the output frames
 * keep their spacing. One that is earlier means overlap: the surplus is
 * cut. Only a large jump either way starts a new origin.
 *
 * Output frames are timed from the same count: frame k of the encoder
 * begins at sample k x frame_size, less the delay the codec itself adds,
 * so that what a decoder plays at a given time is what the source had at
 * that time.
 */
#include <stdlib.h>
#include <string.h>

#include "aframe.h"
#include "log.h"
#include "xcode.h"

#define TOLERANCE       (STC_HZ / 100)      /* stamps this close are "on time" */
#define MEND_MAX        STC_HZ              /* holes and overlaps up to this are mended */
#define PES_BYTES_MAX   32768
/*
 * A standard decoder has 3584 bytes of buffer for an audio stream. What
 * sits in it is the audio between "has arrived" and "is played": a packet
 * is sent over as long as it plays, and has all arrived `lead` before its
 * first sample is due, so that is (packet + lead) of audio at the bit
 * rate. Both are sized, per encoder, to stay inside TSTD_BUDGET bytes
 * (the buffer less room for packet headers and a late transport packet),
 * and never more than LEAD_TOTAL_MAX however low the bit rate.
 */
#define TSTD_BUDGET     3000
#define LEAD_TOTAL_MAX  (STC_HZ / 5)
#define LEAD_MIN        (STC_HZ / 50)
#define ENC_OUT_MAX     16384
#define SEGMENTS        4

struct segment {
    int64_t from;               /* first sample (count since the encoder opened) ... */
    stc_t   origin;             /* ... that is timed from this: time of sample 0 */
};

struct axc {
    struct xcode_cfg cfg;
    xcode_emit_fn emit;
    void *opaque;
    struct xcode_stats *st;

    /* input */
    struct aframer *fr;
    const struct adec_ops *dec;
    void *dctx;
    stc_t next_pts;             /* time of the next unit if it brings none */
    stc_t au_dur;
    int16_t pcm[AUDIO_MAX_FRAME * AUDIO_MAX_CH];

    /* encoder */
    const struct aenc_ops *enc;
    void *ectx;
    int rate, ch, frame, delay;
    int16_t *fifo;              /* less than one frame of samples */
    int fifo_n;
    int64_t fed;                /* samples given to the fifo since the encoder opened */
    int64_t frames_out;         /* access units received from the encoder */
    struct segment seg[SEGMENTS];
    int n_seg;
    struct aframer *ofr;
    uint8_t obuf[ENC_OUT_MAX];
    bool enc_failed;            /* this format cannot be encoded: say so once */
    int fail_rate, fail_ch;

    /* PES packet being filled */
    uint8_t *pes;
    size_t pes_len;
    stc_t pes_pts, pes_dur;
    int pes_n;                  /* frames in it */
    int pes_frames;             /* frames to a packet */
    stc_t lead;                 /* a packet has all left this long before its
                                 * first sample is due */
    stc_t last_t1;
};

/* samples -> ticks without overflowing or losing the remainder */
static stc_t samples_to_stc(int64_t n, int rate)
{
    int64_t q = n / rate, r = n % rate;

    return q * STC_HZ + r * STC_HZ / rate;
}

static int64_t stc_to_samples(stc_t t, int rate)
{
    return (t / STC_HZ) * rate + (t % STC_HZ) * rate / STC_HZ;
}

/* ---- output: PES packets -------------------------------------------- */

static void pes_flush(struct axc *a)
{
    struct es_unit *u;
    size_t off;
    int64_t pts90;

    if (a->pes_len == 0)
        return;
    pts90 = (a->pes_pts + STC_PER_90K / 2) / STC_PER_90K;
    u = pes_unit_new(a->cfg.pid, 0xC0, a->pes_len, pts90, NOPTS, &off);
    if (u) {
        memcpy(u->data + off, a->pes, a->pes_len);
        u->t1 = a->pes_pts - a->lead;
        u->t0 = u->t1 - a->pes_dur;
        if (u->t0 < a->last_t1 && a->last_t1 - u->t0 < STC_HZ)
            u->t0 = a->last_t1;             /* never two at once */
        if (u->t1 <= u->t0)
            u->t1 = u->t0 + 1;
        a->last_t1 = u->t1;
        a->st->units_out++;
        a->emit(a->opaque, u);
    }
    a->pes_len = 0;
    a->pes_dur = 0;
    a->pes_n = 0;
}

static void pes_add(struct axc *a, const uint8_t *au, size_t len, stc_t pts, stc_t dur)
{
    /* A packet holds frames that follow on from one another. */
    if (a->pes_len && (pts - (a->pes_pts + a->pes_dur) > TOLERANCE ||
                       pts - (a->pes_pts + a->pes_dur) < -TOLERANCE))
        pes_flush(a);
    if (a->pes_len + len > PES_BYTES_MAX)
        pes_flush(a);
    if (len > PES_BYTES_MAX)
        return;
    if (a->pes_len == 0)
        a->pes_pts = pts;
    memcpy(a->pes + a->pes_len, au, len);
    a->pes_len += len;
    a->pes_dur += dur;
    a->st->frames_out++;
    if (++a->pes_n >= a->pes_frames)
        pes_flush(a);
}

/* ---- encoder -------------------------------------------------------- */

static stc_t frame_pts(const struct axc *a, int64_t k)
{
    int64_t first = k * a->frame;
    stc_t origin = a->seg[0].origin;

    for (int i = a->n_seg - 1; i >= 0; i--)
        if (a->seg[i].from <= first) {
            origin = a->seg[i].origin;
            break;
        }
    return origin + samples_to_stc(first, a->rate) - samples_to_stc(a->delay, a->rate);
}

static void encode_frame(struct axc *a, const int16_t *pcm)
{
    const uint8_t *au;
    size_t len;
    stc_t dummy;
    int n = a->enc->encode(a->ectx, pcm, a->obuf, sizeof(a->obuf));

    if (n <= 0)
        return;
    aframer_push(a->ofr, a->obuf, (size_t)n, NOPTS);
    while (aframer_next(a->ofr, &au, &len, &dummy)) {
        pes_add(a, au, len, frame_pts(a, a->frames_out), samples_to_stc(a->frame, a->rate));
        a->frames_out++;
    }
}

static void fifo_add(struct axc *a, const int16_t *pcm, int n)
{
    while (n > 0) {
        int take = a->frame - a->fifo_n;

        if (take > n)
            take = n;
        if (pcm) {
            memcpy(a->fifo + (size_t)a->fifo_n * a->ch, pcm,
                   (size_t)take * a->ch * sizeof(int16_t));
            pcm += (size_t)take * a->ch;
        } else {
            memset(a->fifo + (size_t)a->fifo_n * a->ch, 0,
                   (size_t)take * a->ch * sizeof(int16_t));
        }
        a->fifo_n += take;
        a->fed += take;
        n -= take;
        if (a->fifo_n == a->frame) {
            encode_frame(a, a->fifo);
            a->fifo_n = 0;
        }
    }
}

/* Completes the frame in hand with silence, so that what follows starts
 * on a frame boundary. */
static void fifo_pad(struct axc *a)
{
    if (a->fifo_n > 0)
        fifo_add(a, NULL, a->frame - a->fifo_n);
}

static void encoder_close(struct axc *a)
{
    if (!a->ectx)
        return;
    fifo_pad(a);
    fifo_add(a, NULL, a->frame);            /* push out what the codec holds back */
    pes_flush(a);
    a->enc->close(a->ectx);
    a->ectx = NULL;
    free(a->fifo);
    a->fifo = NULL;
    aframer_reset(a->ofr);
}

static bool encoder_open(struct axc *a, int rate, int ch)
{
    const struct codec_backend *b;
    char err[160] = "";
    int bitrate = a->cfg.bitrate;

    if (a->enc_failed && a->fail_rate == rate && a->fail_ch == ch)
        return false;
    if (bitrate <= 0) {
        bitrate = a->cfg.out == CODEC_MP2 ? (ch == 1 ? 96000 : 192000)
                                          : (ch == 1 ? 64000 : 128000);
        if (rate < 32000)
            bitrate /= 2;                   /* half-rate audio needs less */
    }

    b = codec_find(CODEC_AUDIO, CODEC_ENCODER, a->cfg.out, WHERE_CPU, 0);
    if (b)
        a->ectx = b->aenc->open(a->cfg.out, rate, ch, &bitrate, &a->frame, &a->delay,
                                err, sizeof(err));
    if (!b || !a->ectx || a->frame <= 0 || a->frame > AUDIO_MAX_FRAME) {
        log_err("PID 0x%04X: cannot encode %s: %s", a->cfg.pid, codec_name(a->cfg.out),
                b ? err : "no encoder built in");
        if (a->ectx)
            b->aenc->close(a->ectx);
        a->ectx = NULL;
        a->enc_failed = true;
        a->fail_rate = rate;
        a->fail_ch = ch;
        a->st->failed = true;
        return false;
    }
    a->fifo = malloc((size_t)a->frame * (size_t)ch * sizeof(int16_t));
    if (!a->fifo) {
        b->aenc->close(a->ectx);
        a->ectx = NULL;
        return false;
    }
    a->enc = b->aenc;
    a->enc_failed = false;
    a->st->failed = false;
    snprintf(a->st->encoder, sizeof(a->st->encoder), "%s", b->name);
    a->rate = rate;
    a->ch = ch;
    {
        stc_t frame_dur = samples_to_stc(a->frame, rate);
        stc_t total = (stc_t)TSTD_BUDGET * 8 * STC_HZ / (bitrate > 0 ? bitrate : 1);

        if (total > LEAD_TOTAL_MAX)
            total = LEAD_TOTAL_MAX;
        a->pes_frames = (int)(total / 2 / (frame_dur > 0 ? frame_dur : 1));
        if (a->pes_frames < 1)
            a->pes_frames = 1;
        a->lead = total - a->pes_frames * frame_dur;
        if (a->lead < LEAD_MIN)
            a->lead = LEAD_MIN;
    }
    a->fifo_n = 0;
    a->fed = 0;
    a->frames_out = 0;
    a->n_seg = 0;
    log_info("PID 0x%04X: audio %s -> %s, %d Hz, %s, %d kb/s (%s -> %s)",
             a->cfg.pid, codec_name(a->cfg.in), codec_name(a->cfg.out), rate,
             ch == 1 ? "mono" : "stereo", bitrate / 1000, a->st->decoder, b->name);
    return true;
}

/* ---- samples -------------------------------------------------------- */

static void new_origin(struct axc *a, stc_t pts)
{
    fifo_pad(a);
    if (a->n_seg == SEGMENTS) {
        memmove(a->seg, a->seg + 1, sizeof(a->seg[0]) * (SEGMENTS - 1));
        a->n_seg--;
    }
    a->seg[a->n_seg].from = a->fed;
    a->seg[a->n_seg].origin = pts - samples_to_stc(a->fed, a->rate);
    a->n_seg++;
}

static void samples_in(struct axc *a, stc_t pts, const int16_t *pcm, int n,
                       int rate, int ch)
{
    stc_t diff;

    if (!a->ectx || rate != a->rate || ch != a->ch) {
        encoder_close(a);
        if (!encoder_open(a, rate, ch))
            return;
    }
    if (a->n_seg == 0) {
        new_origin(a, pts);
    } else {
        diff = pts - (a->seg[a->n_seg - 1].origin + samples_to_stc(a->fed, a->rate));
        if (diff > TOLERANCE && diff <= MEND_MAX) {
            int64_t gap = stc_to_samples(diff, a->rate);

            a->st->silence_ms += (uint64_t)(diff / STC_PER_MS);
            while (gap > 0) {
                int step = gap > a->frame ? a->frame : (int)gap;

                fifo_add(a, NULL, step);
                gap -= step;
            }
        } else if (diff < -TOLERANCE && diff >= -MEND_MAX) {
            int64_t cut = stc_to_samples(-diff, a->rate);

            if (cut > n)
                cut = n;
            a->st->cut_ms += (uint64_t)(samples_to_stc(cut, a->rate) / STC_PER_MS);
            pcm += cut * ch;
            n -= (int)cut;
        } else if (diff > MEND_MAX || diff < -MEND_MAX) {
            a->st->restarts++;
            new_origin(a, pts);
        }
    }
    if (n > 0)
        fifo_add(a, pcm, n);
}

/* ---- input ---------------------------------------------------------- */

void axc_unit(struct axc *a, const struct es_unit *u)
{
    const uint8_t *au;
    size_t len;
    stc_t pts;
    int off = pes_payload_offset(u->data, u->len);

    a->st->units_in++;
    if (off < 0)
        return;
    aframer_push(a->fr, u->data + off, u->len - (size_t)off,
                 u->pts == NOPTS ? NOPTS : u->pts * STC_PER_90K);

    while (aframer_next(a->fr, &au, &len, &pts)) {
        int rate = 0, ch = 0, delay = 0, n;

        if (pts == NOPTS)
            pts = a->next_pts;
        if (pts == NOPTS)
            continue;                       /* nothing to time it by yet */
        n = a->dec->decode(a->dctx, au, len, a->pcm, &rate, &ch, &delay);
        if (n < 0) {
            a->st->errors++;
            a->next_pts = a->au_dur ? pts + a->au_dur : NOPTS;
            continue;
        }
        a->st->frames_in++;
        if (n == 0) {
            a->next_pts = a->au_dur ? pts + a->au_dur : NOPTS;
            continue;
        }
        a->au_dur = samples_to_stc(n, rate);
        a->next_pts = pts + a->au_dur;
        samples_in(a, pts - samples_to_stc(delay, rate), a->pcm, n, rate, ch);
    }
    a->st->skipped_bytes = aframer_skipped(a->fr);
}

void axc_idle(struct axc *a)
{
    pes_flush(a);
}

void axc_finish(struct axc *a)
{
    encoder_close(a);
}

struct axc *axc_new(const struct xcode_cfg *cfg, xcode_emit_fn emit, void *opaque,
                    struct xcode_stats *st, char *err, size_t errlen)
{
    const struct codec_backend *db, *eb;
    struct axc *a;

    db = codec_find(CODEC_AUDIO, CODEC_DECODER, cfg->in, WHERE_CPU, 0);
    eb = codec_find(CODEC_AUDIO, CODEC_ENCODER, cfg->out, WHERE_CPU, 0);
    if (!db || !eb) {
        snprintf(err, errlen, "no %s for %s is built in", db ? "encoder" : "decoder",
                 codec_name(db ? cfg->out : cfg->in));
        return NULL;
    }
    a = calloc(1, sizeof(*a));
    if (!a)
        return NULL;
    a->cfg = *cfg;
    a->emit = emit;
    a->opaque = opaque;
    a->st = st;
    a->next_pts = NOPTS;
    a->fr = aframer_new(cfg->in);
    a->ofr = aframer_new(cfg->out);
    a->pes = malloc(PES_BYTES_MAX);
    a->dec = db->adec;
    a->dctx = a->fr && a->ofr && a->pes ? a->dec->open(cfg->in, err, errlen) : NULL;
    if (!a->dctx) {
        axc_free(a);
        return NULL;
    }
    snprintf(st->decoder, sizeof(st->decoder), "%s", db->name);
    snprintf(st->encoder, sizeof(st->encoder), "%s", eb->name);
    return a;
}

void axc_free(struct axc *a)
{
    if (!a)
        return;
    if (a->ectx)
        a->enc->close(a->ectx);
    if (a->dctx)
        a->dec->close(a->dctx);
    aframer_free(a->fr);
    aframer_free(a->ofr);
    free(a->fifo);
    free(a->pes);
    free(a);
}
