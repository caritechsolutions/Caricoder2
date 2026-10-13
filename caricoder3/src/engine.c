/*
 * caricoder3 - the pipeline: demux -> (codecs, later) -> mux, plus clocking
 * Copyright (c) 2026 CariTech Solutions
 *
 * Time lines
 * ----------
 * Input time  : the source's 27 MHz clock, as carried by its PCRs.
 * Output time : input time + ts_offset. ts_offset starts at zero, so in
 *               normal running the output carries the very same PTS/DTS
 *               values as the input. It only changes when the source
 *               restarts on a new clock, and then by exactly the amount
 *               that keeps the output clock running on without a step.
 *
 * The mux's "now" on the output time line is
 *
 *     live : pll(wall clock) - delay
 *     file : (latest input time + ts_offset) - delay
 *
 * so every unit sits in the mux for `delay` before it is due. That delay
 * is what lets whole PES packets be collected (and, later, frames be
 * decoded and re-encoded) and still leave on time.
 */
#include "engine.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "clock.h"
#include "log.h"
#include "xcode.h"

#define RESYNC_LIMIT    (STC_HZ / 5)    /* live: PCR this far off the model ... */
#define RESYNC_STABLE   (STC_HZ / 20)   /* ... without moving more than this   */
#define RESYNC_HOLD_NS  300000000LL     /* ... for this long ...               */
#define RESYNC_SAMPLES  3               /* ... over this many PCRs = new clock */
#define HOLD_MAX_NS     600000000LL     /* live: longest content is held meanwhile */
#define PCR_QUIET_NS    500000000LL     /* live: data but no PCR for this long */
#define ANCHOR_EVERY_NS 100000000LL
#define FLOW_GAP_NS     400000000LL     /* live: a pause at least this long restarts the run */
#define CATCHUP_MAX     (STC_HZ / 2)    /* live: further behind than this, skip */
#define FILE_SPLICE_MIN (STC_HZ / 10)   /* file: breaks smaller than this are left */
#define DRAIN_LIMIT     (30 * STC_HZ)   /* file: most output written after the end */
#define RUN_MAX_SLOTS   4096
#define SBUF_SIZE       (256 * 1024)
#define LOG_EVERY_NS    1000000000LL

struct engine {
    struct engine_cfg cfg;
    struct demux *dmx;
    struct mux *mux;
    stc_t delay;

    pthread_mutex_t lock;       /* guards everything below and the mux */
    struct pll pll;
    stc_t   ts_offset;
    stc_t   last_out_time;      /* newest PCR or unit time, output time line */
    bool    have_pcr;
    bool    have_prog;
    struct program prog;
    uint64_t discontinuities;
    struct demux_stats in_snap;
    uint64_t in_bytes, in_datagrams, rtp_datagrams;

    /* live: a PCR far from where the clock model puts it is not believed
     * until it has stayed there (see on_pcr) */
    bool    suspect;
    int64_t suspect_since;
    stc_t   suspect_err;
    int     suspect_n;
    stc_t   suspect_pcr;        /* input PCR that raised the question */
    bool    suspect_ahead;      /* ... and it read ahead of the clock model */
    /* Content that arrives while that question is open waits here, not yet
     * stamped for either clock, until the answer is known. */
    struct es_unit *held_head, *held_tail;
    int64_t held_since;         /* when the first of them arrived, ns */
    unsigned held_n;

    /* input thread only */
    int64_t rx_ns;              /* arrival time of the datagram in hand */
    int64_t last_pcr_rx;        /* arrival time of the last PCR */
    int64_t last_anchor_rx;     /* last time we stood in for a missing PCR */
    int64_t prev_rx;            /* arrival time of the previous datagram */
    int64_t flow_start;         /* start of the present unbroken run of input */
    int64_t dgram_interval;     /* smoothed time between datagrams, ns */

    /* Streams that are transcoded. xc_mode says what happens to a PID's
     * units: sent to the mux as they are, or handed to xc_by_pid. Both
     * guarded by the lock; the xcode objects themselves are created and
     * destroyed by the input thread only. */
    struct xcode *xc_by_pid[PID_COUNT];
    uint8_t xc_mode[PID_COUNT];         /* enum stream_mode */
    struct xcode *xc_list[MAX_ES];
    struct xcode_cfg xc_cfg[MAX_ES];
    int n_xc;
    uint64_t xc_orphans;                /* units for a transcoder that is gone */
    bool warned_no_backend;

    /* warnings about the clock: at most one line a second */
    int64_t warn_last_ns;
    unsigned warn_suppressed;
    int64_t prog_log_ns;
    unsigned prog_log_suppressed;

    /* byte-stream input */
    uint8_t *sbuf;
    size_t  sbuf_len;
    bool    locked;
};

static stc_t round300(stc_t v)
{
    stc_t r = v % STC_PER_90K;

    v -= r;
    if (r >= STC_PER_90K / 2)
        v += STC_PER_90K;
    else if (r < -STC_PER_90K / 2)
        v -= STC_PER_90K;
    return v;
}

/* ---- transcoded streams --------------------------------------------- */

/*
 * A unit that has been given its place on the output time line goes to
 * the mux, or to the transcoder of its stream. Called with the lock held.
 */
static void deliver(struct engine *e, struct es_unit *u)
{
    if (e->xc_mode[u->pid] == STREAM_COPY) {
        mux_push(e->mux, u);
    } else if (e->xc_by_pid[u->pid]) {
        xcode_push(e->xc_by_pid[u->pid], u);
    } else {
        e->xc_orphans++;
        free(u);
    }
}

/* What a transcoder produced. Comes from its thread in a live run, and
 * from inside deliver() (lock already held) in a file run. */
static void on_xcode_out(void *opaque, struct es_unit *u)
{
    struct engine *e = opaque;
    bool live = e->cfg.live;
    struct xcode *x;
    stc_t now = NOPTS;

    if (live)
        pthread_mutex_lock(&e->lock);
    if (!live)
        now = e->last_out_time - e->delay;
    else if (e->pll.valid)
        now = pll_eval(&e->pll, clock_now_ns()) - e->delay;
    x = e->xc_by_pid[u->pid];
    if (x && now != NOPTS)
        xcode_note_slack(x, u->t0 - now);
    mux_push(e->mux, u);
    if (live)
        pthread_mutex_unlock(&e->lock);
}

static const struct xcode_spec *spec_for(const struct xcode_spec *list, int n, int pid)
{
    const struct xcode_spec *all = NULL;

    for (int i = 0; i < n; i++) {
        if (list[i].pid == pid)
            return &list[i];
        if (list[i].pid == 0)
            all = &list[i];
    }
    return all;
}

/* The transcoding wanted for stream `idx` of the input, if any. */
static bool plan_stream(struct engine *e, const struct program *p, int idx,
                        struct xcode_cfg *cfg)
{
    enum codec_id in = xcode_input_codec(p, idx);
    const struct xcode_spec *s;
    bool video = codec_is_video(in);
    bool is_audio_type, is_video_type;
    int st = p->es[idx].stream_type;

    /* Which list applies is a matter of what the stream is, also when it
     * is in a coding we cannot read. */
    is_video_type = st == 0x01 || st == 0x02 || st == 0x10 || st == 0x1B || st == 0x24;
    is_audio_type = st == 0x03 || st == 0x04 || st == 0x0F || st == 0x11 ||
                    st == 0x81 || st == 0x87 ||
                    (st == 0x06 && (program_es_has_desc(p, idx, 0x6A, NULL) ||
                                    program_es_has_desc(p, idx, 0x7A, NULL)));
    if (!is_video_type && !is_audio_type)
        return false;
    s = is_video_type ? spec_for(e->cfg.video, e->cfg.n_video, p->es[idx].pid)
                      : spec_for(e->cfg.audio, e->cfg.n_audio, p->es[idx].pid);
    if (!s || s->codec == CODEC_NONE)
        return false;
    if (in == CODEC_NONE) {
        log_warn("PID 0x%04X: %s cannot be decoded here, the stream is copied",
                 p->es[idx].pid, stream_type_name(st));
        return false;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->pid = p->es[idx].pid;
    cfg->in = in;
    cfg->out = s->codec;
    cfg->bitrate = s->bitrate;
    if (video) {
        cfg->decoder = e->cfg.decoder;
        cfg->encoder = e->cfg.encoder;
        cfg->gpu = e->cfg.gpu;
        cfg->vbv_ms = e->cfg.vbv_ms;
        cfg->gop_ms = e->cfg.gop_ms;
        cfg->preset = e->cfg.preset;
    }
    return true;
}

/* How the transcoded stream is announced in the PMT. */
static void announce(struct program *np, int idx, enum codec_id out)
{
    /* MPEG audio at 16, 22.05 or 24 kHz is type 0x04, at the full rates
     * 0x03. The sample rate is kept, so the type of an MPEG audio input
     * tells; from another coding the rate is not known this early and
     * 0x03 is written (receivers decode either under either type). */
    int mpa_type = np->es[idx].stream_type == 0x04 ? 0x04 : 0x03;

    static const uint8_t keep[] = { 0x0A, 0x52 };       /* language, stream id */
    static const uint8_t hevc_reg[] = { 0x05, 0x04, 'H', 'E', 'V', 'C' };

    switch (out) {
    case CODEC_AAC:  program_set_es(np, idx, 0x0F, keep, 2, NULL, 0); break;
    case CODEC_MP2:
    case CODEC_MP3:  program_set_es(np, idx, mpa_type, keep, 2, NULL, 0); break;
    case CODEC_H264: program_set_es(np, idx, 0x1B, keep, 2, NULL, 0); break;
    case CODEC_HEVC: program_set_es(np, idx, 0x24, keep, 2, hevc_reg, 6); break;
    default: break;
    }
}

/*
 * Brings the set of transcoders in line with the service as it now is,
 * and rewrites `np` (the description the mux will send) accordingly.
 * Runs in the input thread, outside the lock: opening a codec takes time,
 * and stopping one means waiting for its thread.
 */
static void update_xcodes(struct engine *e, const struct program *p, struct program *np)
{
    struct xcode *gone[MAX_ES], *keep[MAX_ES];
    int gone_pid[MAX_ES];
    int64_t gone_dts[MAX_ES];
    stc_t gone_t1[MAX_ES];
    bool gone_out[MAX_ES];
    struct xcode_cfg want[MAX_ES], keep_cfg[MAX_ES];
    bool wanted[MAX_ES] = { false };
    int n_gone = 0, n_keep = 0;

    for (int i = 0; i < p->n_es; i++)
        wanted[i] = plan_stream(e, p, i, &want[i]);

    /* Those no longer wanted, or wanted differently, come out first. */
    pthread_mutex_lock(&e->lock);
    for (int k = 0; k < e->n_xc; k++) {
        bool still = false;

        for (int i = 0; i < p->n_es; i++)
            if (wanted[i] && !memcmp(&want[i], &e->xc_cfg[k], sizeof(want[i])))
                still = true;
        if (still) {
            keep[n_keep] = e->xc_list[k];
            keep_cfg[n_keep++] = e->xc_cfg[k];
        } else {
            gone_pid[n_gone] = e->xc_cfg[k].pid;
            gone[n_gone++] = e->xc_list[k];
            e->xc_by_pid[e->xc_cfg[k].pid] = NULL;
        }
    }
    memcpy(e->xc_list, keep, sizeof(keep[0]) * (size_t)n_keep);
    memcpy(e->xc_cfg, keep_cfg, sizeof(keep_cfg[0]) * (size_t)n_keep);
    e->n_xc = n_keep;
    pthread_mutex_unlock(&e->lock);

    /* What they still hold is the end of the old stream: it goes out. */
    for (int k = 0; k < n_gone; k++) {
        xcode_finish(gone[k]);
        gone_out[k] = xcode_last_out(gone[k], &gone_dts[k], &gone_t1[k]);
        xcode_free(gone[k]);
    }

    for (int i = 0; i < p->n_es; i++) {
        struct xcode *x = NULL;
        char err[200] = "";
        bool have = false;

        if (!wanted[i])
            continue;
        for (int k = 0; k < e->n_xc; k++)
            if (!memcmp(&want[i], &e->xc_cfg[k], sizeof(want[i])))
                have = true;
        if (!have) {
            x = xcode_new(&want[i], e->cfg.live, on_xcode_out, e, err, sizeof(err));
            if (!x) {
                log_err("PID 0x%04X: cannot transcode %s to %s (%s); the stream is copied",
                        want[i].pid, codec_name(want[i].in), codec_name(want[i].out), err);
                wanted[i] = false;
                continue;
            }
            for (int k = 0; k < n_gone; k++)
                if (gone_pid[k] == want[i].pid && gone_out[k])
                    xcode_start_after(x, gone_dts[k], gone_t1[k]);
            pthread_mutex_lock(&e->lock);
            e->xc_list[e->n_xc] = x;
            e->xc_cfg[e->n_xc++] = want[i];
            e->xc_by_pid[want[i].pid] = x;
            pthread_mutex_unlock(&e->lock);
        }
        announce(np, i, want[i].out);
    }

    pthread_mutex_lock(&e->lock);
    memset(e->xc_mode, STREAM_COPY, sizeof(e->xc_mode));
    for (int k = 0; k < e->n_xc; k++)
        e->xc_mode[e->xc_cfg[k].pid] = STREAM_TRANSCODE;
    pthread_mutex_unlock(&e->lock);
}

/* ---- demux callbacks ------------------------------------------------ */

static void on_program(void *opaque, const struct program *p)
{
    struct engine *e = opaque;
    struct program np = *p;

    program_set_service(&np, e->cfg.service_provider, e->cfg.service_name);
    if (e->cfg.n_audio || e->cfg.n_video)
        update_xcodes(e, p, &np);

    bool same_streams;

    pthread_mutex_lock(&e->lock);
    same_streams = e->have_prog && e->prog.pmt_pid == np.pmt_pid &&
                   e->prog.pcr_pid == np.pcr_pid && e->prog.n_es == np.n_es &&
                   memcmp(e->prog.es, np.es, sizeof(np.es)) == 0;
    mux_set_program(e->mux, &np);
    e->prog = np;
    e->have_prog = true;
    pthread_mutex_unlock(&e->lock);

    /* A source whose tables flap must not flood the log. */
    int64_t now = clock_now_ns();

    if (now - e->prog_log_ns < LOG_EVERY_NS && e->prog_log_ns) {
        e->prog_log_suppressed++;
        return;
    }
    e->prog_log_ns = now;
    if (e->prog_log_suppressed) {
        log_info("service: %u further table change(s) not shown",
                 e->prog_log_suppressed);
        e->prog_log_suppressed = 0;
    }
    if (same_streams) {
        log_info("service %u: tables updated (%s)", np.number,
                 np.have_sdt ? "SDT present" : "no SDT");
        return;
    }
    log_info("service %u: PMT PID 0x%04X, PCR PID 0x%04X, %d stream(s)",
             np.number, np.pmt_pid, np.pcr_pid, np.n_es);
    for (int i = 0; i < np.n_es; i++) {
        bool xc = e->xc_mode[np.es[i].pid] == STREAM_TRANSCODE;

        if (xc)
            log_info("  PID 0x%04X  type 0x%02X  %s  [transcoded from %s]", np.es[i].pid,
                     np.es[i].stream_type, stream_type_name(np.es[i].stream_type),
                     stream_type_name(p->es[i].stream_type));
        else
            log_info("  PID 0x%04X  type 0x%02X  %s  [copy]", np.es[i].pid,
                     np.es[i].stream_type, stream_type_name(np.es[i].stream_type));
    }
    if (np.pcr_pid == PID_NULL)
        log_warn("service has no PCR: it cannot be timed and will not be output");
}

/* Clock warnings can come once per PCR on a broken input. Called without
 * the engine lock so a slow log sink never holds up the output. */
static void warn_time_base(struct engine *e, stc_t moved)
{
    int64_t now = clock_now_ns();

    if (now - e->warn_last_ns < LOG_EVERY_NS) {
        e->warn_suppressed++;
        return;
    }
    if (e->warn_suppressed)
        log_warn("clock: input time base moved by %+.3f s, output time line "
                 "kept continuous (%u similar in the last second not shown)",
                 (double)moved / STC_HZ, e->warn_suppressed);
    else
        log_warn("clock: input time base moved by %+.3f s, output time line "
                 "kept continuous", (double)moved / STC_HZ);
    e->warn_last_ns = now;
    e->warn_suppressed = 0;
}

/* Moves a unit from the input time line onto the output one. */
static void stamp(const struct engine *e, struct es_unit *u)
{
    int64_t off90 = e->ts_offset / STC_PER_90K;

    if (!e->ts_offset)
        return;
    u->t0 += e->ts_offset;
    u->t1 += e->ts_offset;
    if (u->pts != NOPTS) {
        u->pts += off90;
        if (u->pts_off >= 0)
            pes_patch_ts(u->data + u->pts_off, u->pts);
    }
    if (u->dts != NOPTS) {
        u->dts += off90;
        if (u->dts_off >= 0)
            pes_patch_ts(u->data + u->dts_off, u->dts);
    }
}

/*
 * Sends on everything that was held, stamped with the offset in force now.
 * `tentative` marks it as stamped without knowing yet whether the clock
 * has stepped; if it turns out it has, the mux moves what is still
 * waiting (mux_restamp_tentative). Called with the lock held.
 */
static void release_held(struct engine *e, bool tentative)
{
    struct es_unit *u = e->held_head;

    e->held_head = e->held_tail = NULL;
    e->held_n = 0;
    while (u) {
        struct es_unit *next = u->next;

        u->next = NULL;
        u->tentative = tentative;
        stamp(e, u);
        deliver(e, u);
        u = next;
    }
}

/* The same for units that carry their final stamps already. */
static void release_held_stamped(struct engine *e)
{
    struct es_unit *u = e->held_head;

    e->held_head = e->held_tail = NULL;
    e->held_n = 0;
    while (u) {
        struct es_unit *next = u->next;

        u->next = NULL;
        deliver(e, u);
        u = next;
    }
}

static void on_unit(void *opaque, struct es_unit *u)
{
    struct engine *e = opaque;

    pthread_mutex_lock(&e->lock);
    if (!e->cfg.live) {
        stc_t t1;

        /* A file run starts its output clock at the first thing to send,
         * and from then on the clock follows the data: normally the PCR
         * (on_pcr), but units keep it moving if the PCR has gone. */
        stamp(e, u);
        t1 = u->t1;
        if (!mux_started(e->mux)) {
            /* New video is sent from one encoder buffer before it is due
             * (xc_video.c), which for the first pictures is earlier than
             * anything in the input: the clock starts that much sooner. */
            stc_t early = 0;

            if (e->cfg.n_video)
                early = (stc_t)((e->cfg.vbv_ms > 0 ? e->cfg.vbv_ms : 1000) + 100) * STC_PER_MS;
            if (early > u->t0)              /* but not before time 0: that */
                early = u->t0 > 0 ? u->t0 : 0;  /* would start in a wrap */
            mux_start(e->mux, u->t0 - early);
        }
        deliver(e, u);
        if (t1 > e->last_out_time) {
            e->last_out_time = t1;
            mux_run(e->mux, t1 - e->delay, 0);
        }
    } else if (!e->suspect) {
        stamp(e, u);
        deliver(e, u);
    } else if (u->wall) {
        /*
         * The PCR has gone quiet with the question still open, and this
         * was timed from our own clock in its place (demux_time_anchor).
         * Its send time is therefore already on the time line we know and
         * must never be moved. Whether its timestamps belong to a new
         * clock cannot be known without a PCR; it goes out as it is.
         *
         * What is held, or queued under doubt, is older content of the
         * same streams. Left where the doubtful clock put it, it would
         * sit in front of this for seconds, or be long overdue. It goes
         * out now, ahead of this, on the same terms.
         */
        stamp(e, u);
        mux_send_tentative_now(e->mux, u->t0);
        for (struct es_unit *h = e->held_head; h; h = h->next) {
            stamp(e, h);
            h->t0 = u->t0;
            h->t1 = u->t0 + 1;
        }
        release_held_stamped(e);
        deliver(e, u);
    } else if (e->suspect_ahead && !e->held_head && u->t1 <= e->suspect_pcr) {
        /* The clock may have jumped forward, but this was complete before
         * the PCR that says so: it belongs to the clock we know. */
        stamp(e, u);
        deliver(e, u);
    } else if (e->held_head ? e->rx_ns - e->held_since <= HOLD_MAX_NS
                            : e->rx_ns - e->suspect_since <= HOLD_MAX_NS) {
        /* Which clock this belongs to is not known yet (see on_pcr). */
        if (!e->held_head)
            e->held_since = e->rx_ns;
        u->next = NULL;
        if (e->held_tail)
            e->held_tail->next = u;
        else
            e->held_head = u;
        e->held_tail = u;
        e->held_n++;
    } else {
        /* The question has stayed open too long to keep content back
         * (the PCR stopped coming, or comes too rarely). Send it on the
         * clock we know, marked so that it can still be moved. */
        release_held(e, true);
        u->tentative = true;
        stamp(e, u);
        deliver(e, u);
    }
    pthread_mutex_unlock(&e->lock);
}

static void on_pcr(void *opaque, stc_t pcr, stc_t expected, bool jump,
                   stc_t since)
{
    struct engine *e = opaque;
    bool first = false, moved = false;
    stc_t moved_by = 0;

    pthread_mutex_lock(&e->lock);
    if (e->cfg.live) {
        stc_t obs = pcr + e->ts_offset;

        e->last_pcr_rx = e->rx_ns;
        if (!e->pll.valid) {
            pll_init(&e->pll, e->rx_ns, obs);
            first = true;
        } else {
            stc_t err = obs - pll_eval(&e->pll, e->rx_ns);

            if (err <= RESYNC_LIMIT && err >= -RESYNC_LIMIT) {
                if (e->suspect) {
                    e->suspect = false;     /* false alarm: same clock */
                    mux_clear_tentative(e->mux);
                    release_held(e, false);
                }
                pll_update(&e->pll, e->rx_ns, obs);
            } else if (!e->suspect) {
                /* The question opens (explained just below). */
                e->suspect = true;
                e->suspect_since = e->rx_ns;
                e->suspect_err = err;
                e->suspect_n = 1;
                /* What was complete before the clock first showed its
                 * new reading still belongs to the clock we know. */
                e->suspect_pcr = since;
                e->suspect_ahead = err > 0;
            } else if (err - e->suspect_err > RESYNC_STABLE ||
                       err - e->suspect_err < -RESYNC_STABLE) {
                /*
                 * Far from where the source clock should be. Either data
                 * that was held up and is now being delivered faster than
                 * real time (the error shrinks as it catches up with the
                 * wall clock), or a source whose clock really has stepped
                 * (the error stays put). Only time tells them apart, so
                 * wait for the error to hold still before acting, keep
                 * the outliers away from the PLL, and hold the content
                 * that arrives meanwhile (on_unit). Each time the error
                 * moves, the wait starts again. An error that moves is
                 * most likely a backlog draining, so what was held goes
                 * out on the clock we know. It is marked tentative all
                 * the same: a real step can also begin with a moving
                 * error, and then it is put right when the step is
                 * confirmed.
                 */
                release_held(e, true);
                e->suspect_since = e->rx_ns;
                e->suspect_err = err;
                e->suspect_n = 1;
                e->suspect_ahead = false;
            } else if (++e->suspect_n >= RESYNC_SAMPLES &&
                       e->rx_ns - e->suspect_since >= RESYNC_HOLD_NS) {
                /* It stayed: the clock has stepped. Splice the new one
                 * onto ours so that the output clock runs on. Content
                 * already queued under doubt is moved onto the new clock,
                 * and what was held is sent stamped for it. */
                stc_t adj = round300(err);

                e->ts_offset -= adj;
                e->discontinuities++;
                e->suspect = false;
                mux_restamp_tentative(e->mux, -adj);
                release_held(e, false);
                moved = true;
                moved_by = err;
            }
        }
        if (pcr + e->ts_offset > e->last_out_time)
            e->last_out_time = pcr + e->ts_offset;
    } else {
        if (jump) {
            /* No wall clock in a file run, so a hole in the recording is
             * closed up and a restart is spliced on: either way the
             * output clock carries straight on. */
            stc_t diff = pcr - expected;

            if (diff < -FILE_SPLICE_MIN || diff > FILE_SPLICE_MIN) {
                e->ts_offset -= round300(diff);
                e->discontinuities++;
                moved = true;
                moved_by = diff;
            }
        }
        if (pcr + e->ts_offset > e->last_out_time)
            e->last_out_time = pcr + e->ts_offset;
        if (mux_started(e->mux))
            mux_run(e->mux, e->last_out_time - e->delay, 0);
    }
    e->have_pcr = true;
    pthread_mutex_unlock(&e->lock);

    if (first)
        log_info("clock: locked to input PCR");
    if (moved)
        warn_time_base(e, moved_by);
}

/* ---- input ---------------------------------------------------------- */

static void snapshot_input(struct engine *e, size_t bytes, bool datagram, bool rtp)
{
    pthread_mutex_lock(&e->lock);
    e->in_snap = *demux_get_stats(e->dmx);
    e->in_bytes += bytes;
    if (datagram)
        e->in_datagrams++;
    if (rtp)
        e->rtp_datagrams++;
    pthread_mutex_unlock(&e->lock);
}

void engine_push_datagram(struct engine *e, const uint8_t *buf, size_t len,
                          int64_t rx_ns)
{
    const uint8_t *p = buf;
    size_t n = len;
    bool rtp = false;

    /* RTP (RFC 2250): version 2 in the top bits; raw TS starts with 0x47. */
    if (n >= 12 + TS_SIZE && (p[0] & 0xC0) == 0x80) {
        size_t hdr = 12 + 4u * (p[0] & 0x0F);

        if ((p[0] & 0x10) && n >= hdr + 4)
            hdr += 4 + 4u * (((size_t)p[hdr + 2] << 8) | p[hdr + 3]);
        if ((p[0] & 0x20) && n > hdr && p[n - 1] <= n - hdr)
            n -= p[n - 1];
        if (hdr < n && p[hdr] == TS_SYNC) {
            p += hdr;
            n -= hdr;
            rtp = true;
        }
    }

    e->rx_ns = rx_ns;
    /* Where the present uninterrupted run of input began. What counts
     * as an interruption depends on the stream: a 64 kb/s radio service
     * sends a datagram only every 160 ms. */
    if (e->flow_start == 0) {
        e->flow_start = rx_ns;
    } else {
        int64_t gap = rx_ns - e->prev_rx;
        int64_t limit = 5 * e->dgram_interval;

        if (limit < FLOW_GAP_NS)
            limit = FLOW_GAP_NS;
        if (gap > limit)
            e->flow_start = rx_ns;
        else
            e->dgram_interval += (gap - e->dgram_interval) / 16;
    }
    e->prev_rx = rx_ns;
    while (n >= TS_SIZE) {
        if (p[0] == TS_SYNC) {
            demux_packet(e->dmx, p);
            p += TS_SIZE;
            n -= TS_SIZE;
        } else {
            demux_sync_error(e->dmx);
            while (n >= TS_SIZE && p[0] != TS_SYNC) {
                p++;
                n--;
            }
        }
    }
    /*
     * Data has been arriving steadily for half a second and no PCR with
     * it. The clock recovered while the PCR was there is still running,
     * so use it in the PCR's place: every 100 ms tell the demux what time
     * it is. "Steadily" matters: after an outage the first datagrams are
     * old data whose own PCR is about to follow, and the wall clock says
     * nothing true about them.
     */
    if (e->cfg.live &&
        rx_ns - (e->last_pcr_rx > e->flow_start ? e->last_pcr_rx : e->flow_start)
            > PCR_QUIET_NS &&
        rx_ns - e->last_anchor_rx > ANCHOR_EVERY_NS) {
        stc_t now_stc = 0;
        bool valid, doubt;

        pthread_mutex_lock(&e->lock);
        valid = e->pll.valid;
        doubt = e->suspect;
        if (valid)
            now_stc = pll_eval(&e->pll, rx_ns) - e->ts_offset;
        pthread_mutex_unlock(&e->lock);

        e->last_anchor_rx = rx_ns;
        if (valid)
            demux_time_anchor(e->dmx, now_stc, doubt);
        else
            demux_resolve_now(e->dmx);      /* never had a clock: discard */
    }
    snapshot_input(e, len, true, rtp);
}

void engine_push_stream(struct engine *e, const uint8_t *buf, size_t len)
{
    size_t total = len;

    while (len > 0) {
        size_t take = SBUF_SIZE - e->sbuf_len;
        size_t pos = 0;

        if (take > len)
            take = len;
        memcpy(e->sbuf + e->sbuf_len, buf, take);
        e->sbuf_len += take;
        buf += take;
        len -= take;

        while (e->sbuf_len - pos >= TS_SIZE) {
            const uint8_t *p = e->sbuf + pos;

            if (e->locked && p[0] == TS_SYNC) {
                /* A packet is only trusted once the next one is seen to
                 * start where it should: if alignment was just lost, this
                 * packet's tail already belongs to something else. */
                if (e->sbuf_len - pos < TS_SIZE + 1)
                    break;
                if (p[TS_SIZE] == TS_SYNC) {
                    demux_packet(e->dmx, p);
                    pos += TS_SIZE;
                    continue;
                }
            }
            /* Hunt: a sync byte followed by two more at packet spacing. */
            if (e->locked) {
                e->locked = false;
                demux_sync_error(e->dmx);
                pos++;
                continue;
            }
            if (e->sbuf_len - pos < 2 * TS_SIZE + 1)
                break;
            if (p[0] == TS_SYNC && p[TS_SIZE] == TS_SYNC && p[2 * TS_SIZE] == TS_SYNC)
                e->locked = true;
            else
                pos++;
        }
        memmove(e->sbuf, e->sbuf + pos, e->sbuf_len - pos);
        e->sbuf_len -= pos;
    }
    snapshot_input(e, total, false, false);
}

void engine_finish(struct engine *e)
{
    /* Whatever is left in the sync buffer: whole packets only. */
    for (size_t pos = 0; e->locked && e->sbuf_len - pos >= TS_SIZE; pos += TS_SIZE)
        if (e->sbuf[pos] == TS_SYNC)
            demux_packet(e->dmx, e->sbuf + pos);
    e->sbuf_len = 0;

    demux_flush(e->dmx);
    if (!e->cfg.live)                       /* a live run has no end to tidy */
        for (int k = 0; k < e->n_xc; k++)
            xcode_finish(e->xc_list[k]);

    pthread_mutex_lock(&e->lock);
    while (mux_started(e->mux) && !mux_idle(e->mux) &&
           mux_next_stc(e->mux) < e->last_out_time + DRAIN_LIMIT)
        mux_run(e->mux, mux_next_stc(e->mux), 1);
    mux_drop_all(e->mux);                   /* nothing, unless the limit hit */
    e->in_snap = *demux_get_stats(e->dmx);
    pthread_mutex_unlock(&e->lock);
}

/* ---- live output ---------------------------------------------------- */

int64_t engine_run(struct engine *e, int64_t now_ns, int dgram, int fill)
{
    int64_t wake;
    stc_t next;
    bool started_now = false;

    pthread_mutex_lock(&e->lock);
    if (!mux_started(e->mux)) {
        if (!e->have_prog || !e->pll.valid || e->prog.pcr_pid == PID_NULL) {
            pthread_mutex_unlock(&e->lock);
            return now_ns + 5 * 1000000;
        }
        mux_start(e->mux, pll_eval(&e->pll, now_ns) - e->delay);
        started_now = true;
    }

    /* Content held over a clock question goes out after HOLD_MAX_NS even
     * if no further input arrives to prompt it. */
    if (e->held_head && now_ns - e->held_since > HOLD_MAX_NS)
        release_held(e, true);

    stc_t target = pll_eval(&e->pll, now_ns) - e->delay;
    stc_t skipped = 0;

    /*
     * If this process was not running for a while (stopped, starved, the
     * virtual machine paused), the time it missed is gone. Sending it now
     * in one burst would help nobody downstream, so the output clock steps
     * forward instead, the step is flagged in the stream, and the content
     * that is still current goes out on time.
     */
    if (target - mux_next_stc(e->mux) > CATCHUP_MAX) {
        skipped = target - mux_next_stc(e->mux);
        mux_skip_to(e->mux, target);
    }

    int n = mux_run(e->mux, target, RUN_MAX_SLOTS);

    if (n >= RUN_MAX_SLOTS) {
        wake = now_ns;                      /* behind: come straight back */
    } else {
        if (e->cfg.mux.vbr) {
            next = mux_next_event(e->mux);
            if (next > mux_next_stc(e->mux) + STC_HZ)
                next = mux_next_stc(e->mux) + STC_HZ;
        } else {
            int need = dgram - ((fill + n) % dgram);

            next = mux_stc_after(e->mux, need - 1);
        }
        wake = pll_inverse(&e->pll, next + e->delay);
    }
    pthread_mutex_unlock(&e->lock);

    if (started_now)
        log_info("output: started");
    if (skipped)
        log_warn("output: fell %.2f s behind (process not scheduled?), "
                 "skipped ahead", (double)skipped / STC_HZ);
    return wake;
}

/* ---- housekeeping --------------------------------------------------- */

void engine_get_stats(struct engine *e, struct engine_stats *out)
{
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&e->lock);
    out->in = e->in_snap;
    mux_get_stats(e->mux, &out->out);
    out->in_bytes = e->in_bytes;
    out->in_datagrams = e->in_datagrams;
    out->rtp_datagrams = e->rtp_datagrams;
    out->discontinuities = e->discontinuities;
    out->held_units = e->held_n;
    out->ts_offset = e->ts_offset;
    out->have_program = e->have_prog;
    out->clock_locked = e->cfg.live ? e->pll.valid : e->have_pcr;
    out->started = mux_started(e->mux);
    if (e->pll.valid) {
        out->clock_ppm = pll_ppm(&e->pll);
        out->clock_jitter_us = e->pll.jitter / 27.0;
    }
    if (e->have_prog)
        out->prog = e->prog;
    out->n_xc = e->n_xc;
    for (int k = 0; k < e->n_xc; k++)
        xcode_get_stats(e->xc_list[k], &out->xc[k]);
    out->xc_orphans = e->xc_orphans;
    pthread_mutex_unlock(&e->lock);
}

struct engine *engine_new(const struct engine_cfg *cfg, mux_out_fn out,
                          void *opaque)
{
    static const struct demux_cb cb = { on_program, on_unit, on_pcr };
    struct engine *e = calloc(1, sizeof(*e));

    if (!e)
        return NULL;
    e->cfg = *cfg;
    e->delay = cfg->delay_ms * STC_PER_MS;
    pthread_mutex_init(&e->lock, NULL);
    e->sbuf = malloc(SBUF_SIZE);
    e->mux = mux_new(&cfg->mux, out, opaque);
    e->dmx = demux_new(cfg->program, &cb, e);
    if (!e->sbuf || !e->mux || !e->dmx) {
        engine_free(e);
        return NULL;
    }
    demux_set_pcr_first(e->dmx, cfg->live);
    return e;
}

void engine_free(struct engine *e)
{
    if (!e)
        return;
    while (e->held_head) {
        struct es_unit *u = e->held_head;

        e->held_head = u->next;
        free(u);
    }
    /* The transcoders first: their threads still hand units to the mux. */
    for (int k = 0; k < e->n_xc; k++)
        xcode_free(e->xc_list[k]);
    demux_free(e->dmx);
    mux_free(e->mux);
    free(e->sbuf);
    pthread_mutex_destroy(&e->lock);
    free(e);
}
