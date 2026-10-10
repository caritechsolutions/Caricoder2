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

#define RESYNC_LIMIT    (STC_HZ / 5)    /* live: PCR this far off the model ... */
#define RESYNC_STABLE   (STC_HZ / 20)   /* ... without moving more than this   */
#define RESYNC_HOLD_NS  300000000LL     /* ... for this long ...               */
#define RESYNC_SAMPLES  5               /* ... over this many PCRs = new clock */
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

    /* input thread only */
    int64_t rx_ns;              /* arrival time of the datagram in hand */
    int64_t last_pcr_rx;        /* arrival time of the last PCR */
    int64_t last_anchor_rx;     /* last time we stood in for a missing PCR */
    int64_t prev_rx;            /* arrival time of the previous datagram */
    int64_t flow_start;         /* start of the present unbroken run of input */
    int64_t dgram_interval;     /* smoothed time between datagrams, ns */

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

/* ---- demux callbacks ------------------------------------------------ */

static void on_program(void *opaque, const struct program *p)
{
    struct engine *e = opaque;
    struct program np = *p;

    program_set_service(&np, e->cfg.service_provider, e->cfg.service_name);

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
    for (int i = 0; i < np.n_es; i++)
        log_info("  PID 0x%04X  type 0x%02X  %s  [copy]", np.es[i].pid,
                 np.es[i].stream_type, stream_type_name(np.es[i].stream_type));
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

static void on_unit(void *opaque, struct es_unit *u)
{
    struct engine *e = opaque;

    pthread_mutex_lock(&e->lock);
    if (e->ts_offset) {
        int64_t off90 = e->ts_offset / STC_PER_90K;

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
    u->tentative = e->suspect;

    if (!e->cfg.live) {
        stc_t t1 = u->t1;

        /* A file run starts its output clock at the first thing to send,
         * and from then on the clock follows the data: normally the PCR
         * (on_pcr), but units keep it moving if the PCR has gone. */
        if (!mux_started(e->mux))
            mux_start(e->mux, u->t0);
        mux_push(e->mux, u);
        if (t1 > e->last_out_time) {
            e->last_out_time = t1;
            mux_run(e->mux, t1 - e->delay, 0);
        }
    } else {
        mux_push(e->mux, u);
    }
    pthread_mutex_unlock(&e->lock);
}

static void on_pcr(void *opaque, stc_t pcr, stc_t expected, bool jump)
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
                    e->suspect = false;
                    mux_clear_tentative(e->mux);
                }
                pll_update(&e->pll, e->rx_ns, obs);
            } else if (!e->suspect || err - e->suspect_err > RESYNC_STABLE ||
                       err - e->suspect_err < -RESYNC_STABLE) {
                /*
                 * Far from where the source clock should be. Either data
                 * that was held up and is now being delivered faster than
                 * real time (the error shrinks as it catches up with the
                 * wall clock), or a source that restarted on a new clock
                 * (the error stays put). Only time tells them apart, so
                 * wait for the error to hold still before acting, and
                 * keep the outliers away from the PLL meanwhile. Each time
                 * the error moves, the wait starts again.
                 */
                e->suspect = true;
                e->suspect_since = e->rx_ns;
                e->suspect_err = err;
                e->suspect_n = 1;
            } else if (++e->suspect_n >= RESYNC_SAMPLES &&
                       e->rx_ns - e->suspect_since >= RESYNC_HOLD_NS) {
                /* It stayed: a new clock. Splice it onto ours so that the
                 * output clock runs on, and discard what was queued with
                 * the old offset while we were making sure. */
                e->ts_offset -= round300(err);
                e->discontinuities++;
                e->suspect = false;
                mux_drop_tentative(e->mux);
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
        bool valid;

        pthread_mutex_lock(&e->lock);
        valid = e->pll.valid;
        if (valid)
            now_stc = pll_eval(&e->pll, rx_ns) - e->ts_offset;
        pthread_mutex_unlock(&e->lock);

        e->last_anchor_rx = rx_ns;
        if (valid)
            demux_time_anchor(e->dmx, now_stc);
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
    return e;
}

void engine_free(struct engine *e)
{
    if (!e)
        return;
    demux_free(e->dmx);
    mux_free(e->mux);
    free(e->sbuf);
    pthread_mutex_destroy(&e->lock);
    free(e);
}
