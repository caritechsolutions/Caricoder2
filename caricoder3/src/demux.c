/*
 * caricoder3 - transport stream demultiplexer
 * Copyright (c) 2026 CariTech Solutions
 *
 * Takes 188-byte packets of a single- or multi-program transport stream,
 * follows PAT -> PMT (-> SDT) for one service and hands out complete PES
 * packets and sections, each stamped with the time it arrived.
 *
 * "The time it arrived" is taken from the stream itself, not from the
 * machine's clock: every packet gets a running index, PCR packets pin
 * index -> 27 MHz time, and everything in between is interpolated by
 * position. That makes the timing immune to network jitter and makes the
 * whole demux a pure function of its input, which is what lets the test
 * suite compare file runs bit for bit.
 */
#include "demux.h"

#include <stdlib.h>
#include <string.h>

#define PES_MAX         (8u * 1024 * 1024)
#define PCR_HIST        512
#define PCR_GAP_MAX     (1 * STC_HZ)        /* longer than this is a break */
#define PCR_WAIT_MIN    20000               /* packets; see hist_add() */
#define PCR_WAIT_MAX    2000000
#define SLOPE_MAX       (STC_HZ / 20)       /* 50 ms per packet = 30 kb/s */
#define TS_RANGE_BACK   (5 * STC_HZ)        /* DTS this far behind arrival */
#define TS_RANGE_FWD    (30 * STC_HZ)       /* ... or ahead: not believable */
#define NO_ES           0xFF

enum pid_role { ROLE_NONE = 0, ROLE_PAT, ROLE_PMT, ROLE_SDT, ROLE_ES };

struct es_state {
    uint16_t pid;
    uint8_t  kind;
    bool     used;

    /* PES assembly */
    uint8_t *buf;
    size_t   len, cap;
    size_t   want;              /* 6 + PES_packet_length, 0 if unbounded */
    bool     active;
    bool     rai;
    int64_t  first_idx, last_idx;

    /* section assembly */
    struct section_asm *sasm;
    int64_t  sec_start_idx;

    uint8_t  last_pkt[TS_SIZE]; /* to tell a true duplicate from a loss */
};

struct pcr_pt {
    int64_t idx;
    stc_t   pcr;
};

struct demux {
    struct demux_cb cb;
    void *opaque;
    int want_program;

    int64_t idx;                /* index of the packet being processed */

    uint8_t role[PID_COUNT];
    int8_t  last_cc[PID_COUNT];
    uint8_t dup_seen[PID_COUNT];
    uint8_t es_idx[PID_COUNT];

    struct section_asm pat_asm, pmt_asm, sdt_asm;
    bool have_pat, have_pmt;
    uint8_t last_pmt[PSI_MAX];
    int last_pmt_len;

    struct program cur;         /* what the callback was last told */
    struct program next;        /* being assembled from PAT/PMT/SDT */
    struct program scratch;     /* work area for parsing */
    bool published;

    struct es_state es[MAX_ES];

    /* PCR tracking */
    bool    have_pcr;
    int64_t pcr_raw;            /* last PCR as carried (wrapped) */
    stc_t   pcr_unw;            /* same, unwrapped */
    struct pcr_pt hist[PCR_HIST];
    int     hist_n, hist_head;  /* ring: hist_head is the oldest entry */
    bool    anchored;           /* newest history came from an anchor */
    int64_t slope_num, slope_den; /* ticks per packet of the last segment */
    int64_t peak_pps;           /* highest packet rate seen between two PCRs */
    int64_t pcr_wait_pkts;

    struct es_unit *pend_head, *pend_tail;

    struct demux_stats stats;
};

/* ---- index -> time -------------------------------------------------- */

static const struct pcr_pt *hist_at(const struct demux *d, int i)
{
    return &d->hist[(d->hist_head + i) % PCR_HIST];
}

static void hist_clear(struct demux *d)
{
    d->hist_n = 0;
    d->hist_head = 0;
}

static void hist_add(struct demux *d, int64_t idx, stc_t pcr)
{
    if (d->hist_n > 0) {
        const struct pcr_pt *last = hist_at(d, d->hist_n - 1);

        /* Intervals under a millisecond say nothing reliable about the
         * rate (two PCRs back to back, or a damaged one). */
        if (idx > last->idx && pcr - last->pcr >= STC_PER_MS) {
            int64_t pps;

            d->slope_num = pcr - last->pcr;
            d->slope_den = idx - last->idx;

            /*
             * How many packets may pass without a PCR before we stop
             * waiting for one: two seconds' worth at the highest packet
             * rate this stream has shown, and never fewer than a fixed
             * floor. It must not be judged from the last interval alone:
             * on a null-stripped (VBR) input a quiet interval is routinely
             * followed by a key frame thirty times the size.
             */
            pps = d->slope_den * STC_HZ / d->slope_num;
            d->peak_pps -= d->peak_pps / 256;   /* forget old peaks slowly */
            if (pps > d->peak_pps)
                d->peak_pps = pps;
            d->pcr_wait_pkts = 2 * d->peak_pps;
            if (d->pcr_wait_pkts < PCR_WAIT_MIN)
                d->pcr_wait_pkts = PCR_WAIT_MIN;
            if (d->pcr_wait_pkts > PCR_WAIT_MAX)
                d->pcr_wait_pkts = PCR_WAIT_MAX;
        }
    }
    if (d->hist_n == PCR_HIST) {
        d->hist_head = (d->hist_head + 1) % PCR_HIST;
        d->hist_n--;
    }
    d->hist[(d->hist_head + d->hist_n) % PCR_HIST] = (struct pcr_pt){ idx, pcr };
    d->hist_n++;
}

/*
 * Time of a packet outside the PCRs we hold, carrying the last measured
 * rate onward (or backward). The rate is capped at what a real multiplex
 * can have, and the reach backward at one PCR gap, so a stream that lies
 * about its clock cannot place data arbitrarily far away in time.
 */
static stc_t extrapolate(const struct demux *d, const struct pcr_pt *from,
                         int64_t idx)
{
    int64_t n = idx - from->idx;
    int64_t num = d->slope_num, den = d->slope_den;
    stc_t dt;

    if (den <= 0)
        return from->pcr;
    /* Prefer the rate over everything we remember (some seconds) to the
     * rate of the last interval alone: steadier, and for a VBR stream it
     * is the average rather than whatever the last 30 ms happened to be. */
    if (d->hist_n >= 2) {
        const struct pcr_pt *a = hist_at(d, 0), *b = hist_at(d, d->hist_n - 1);

        if (b->idx > a->idx && b->pcr - a->pcr >= STC_PER_MS) {
            num = b->pcr - a->pcr;
            den = b->idx - a->idx;
        }
    }
    if (num > SLOPE_MAX * den)
        dt = n * SLOPE_MAX;
    else
        dt = n * num / den;
    if (dt < -PCR_GAP_MAX)
        dt = -PCR_GAP_MAX;
    return from->pcr + dt;
}

/* Arrival time of packet `idx` on the input clock. Needs hist_n >= 1. */
static stc_t idx_to_stc(const struct demux *d, int64_t idx)
{
    const struct pcr_pt *newest = hist_at(d, d->hist_n - 1);
    const struct pcr_pt *oldest = hist_at(d, 0);

    if (idx >= newest->idx)
        return extrapolate(d, newest, idx);
    if (idx <= oldest->idx)
        return extrapolate(d, oldest, idx);
    for (int i = d->hist_n - 1; i > 0; i--) {
        const struct pcr_pt *a = hist_at(d, i - 1);
        const struct pcr_pt *b = hist_at(d, i);

        if (idx >= a->idx)
            return a->pcr + (idx - a->idx) * (b->pcr - a->pcr) / (b->idx - a->idx);
    }
    return oldest->pcr;
}

/* ---- delivering units ----------------------------------------------- */

static int64_t unwrap33(int64_t raw, int64_t near)
{
    int64_t m = near % PTS_MOD;

    if (m < 0)
        m += PTS_MOD;
    return near + mod_diff(raw, m, PTS_MOD);
}

/* While a unit waits for its timing, t0/t1 hold packet indices and
 * pts/dts hold the raw 33-bit values. */
static void unit_finalize(struct demux *d, struct es_unit *u)
{
    if (d->hist_n == 0) {                   /* never saw a PCR: cannot time it */
        d->stats.pcr_missing++;
        free(u);
        return;
    }

    stc_t t0 = idx_to_stc(d, u->t0);
    stc_t t1 = idx_to_stc(d, u->t1 + 1);

    if (t1 <= t0)
        t1 = t0 + 1;
    u->t0 = t0;
    u->t1 = t1;

    if (u->kind == ES_PES) {
        int64_t near = t1 / STC_PER_90K;

        if (u->pts != NOPTS)
            u->pts = unwrap33(u->pts, near);
        if (u->dts != NOPTS)
            u->dts = unwrap33(u->dts, near);
        else
            u->dts = u->pts;

        if (u->dts != NOPTS) {
            int64_t ahead = u->dts * STC_PER_90K - t1;

            if (ahead < -TS_RANGE_BACK || ahead > TS_RANGE_FWD) {
                d->stats.ts_out_of_range++;
                free(u);
                return;
            }
        }
        d->stats.pes_units++;
    } else {
        d->stats.sections++;
    }
    d->cb.unit(d->opaque, u);
}

static void resolve_pending(struct demux *d, bool force)
{
    while (d->pend_head) {
        struct es_unit *u = d->pend_head;

        if (!force) {
            if (d->hist_n == 0 || d->slope_den <= 0)
                return;
            if (u->t1 + 1 > hist_at(d, d->hist_n - 1)->idx)
                return;                     /* not bracketed by a PCR yet */
        }
        d->pend_head = u->next;
        if (!d->pend_head)
            d->pend_tail = NULL;
        u->next = NULL;
        unit_finalize(d, u);
    }
}

static void unit_queue(struct demux *d, struct es_unit *u)
{
    u->next = NULL;
    if (d->pend_tail)
        d->pend_tail->next = u;
    else
        d->pend_head = u;
    d->pend_tail = u;
    resolve_pending(d, false);
}

static struct es_unit *unit_alloc(const struct es_state *es, const uint8_t *data,
                                  size_t len, int64_t first_idx, int64_t last_idx)
{
    struct es_unit *u = malloc(sizeof(*u) + len);

    if (!u)
        return NULL;
    memset(u, 0, sizeof(*u));
    u->pid = es->pid;
    u->kind = es->kind;
    u->pts = u->dts = NOPTS;
    u->pts_off = u->dts_off = -1;
    u->t0 = first_idx;
    u->t1 = last_idx;
    u->len = len;
    memcpy(u->data, data, len);
    return u;
}

/* ---- PES ------------------------------------------------------------ */

static bool stream_id_has_header(int sid)
{
    switch (sid) {
    case 0xBC: case 0xBE: case 0xBF: case 0xF0: case 0xF1:
    case 0xF2: case 0xF8: case 0xFF:
        return false;
    default:
        return true;
    }
}

static void pes_complete(struct demux *d, struct es_state *es)
{
    const uint8_t *b = es->buf;
    size_t len = es->len;
    int64_t pts = NOPTS, dts = NOPTS;
    int pts_off = -1, dts_off = -1;

    es->active = false;

    if (es->want && len > es->want)
        len = es->want;                     /* bytes past the declared end */

    if (len < 6 || b[0] != 0 || b[1] != 0 || b[2] != 1) {
        d->stats.pes_invalid++;
        return;
    }
    if (stream_id_has_header(b[3])) {
        if (len < 9 || (b[6] & 0xC0) != 0x80 || 9u + b[8] > len) {
            d->stats.pes_invalid++;
            return;
        }
        int flags = b[7] >> 6;
        int hdl = b[8];

        if ((flags & 2) && hdl >= 5) {
            pts_off = 9;
            pts = pes_read_ts(b + 9);
            if (flags == 3 && hdl >= 10) {
                dts_off = 14;
                dts = pes_read_ts(b + 14);
            }
        }
    }
    size_t plen = ((size_t)b[4] << 8) | b[5];
    if (plen && 6 + plen != len)
        d->stats.pes_len_mismatch++;

    struct es_unit *u = unit_alloc(es, b, len, es->first_idx, es->last_idx);
    if (!u)
        return;
    u->rai = es->rai;
    u->pts = pts;
    u->dts = dts;
    u->pts_off = pts_off;
    u->dts_off = dts_off;
    unit_queue(d, u);
}

static void pes_drop(struct demux *d, struct es_state *es)
{
    if (es->active) {
        es->active = false;
        d->stats.pes_dropped++;
    }
    if (es->sasm)
        section_asm_reset(es->sasm);
}

static void pes_packet(struct demux *d, struct es_state *es, const uint8_t *pkt)
{
    int off = ts_payload_offset(pkt);
    size_t n = (size_t)(TS_SIZE - off);

    if (ts_pusi(pkt)) {
        if (es->active)
            pes_complete(d, es);
        es->active = true;
        es->len = 0;
        es->want = 0;
        es->first_idx = d->idx;
        es->last_idx = d->idx;
        es->rai = ts_has_af_flags(pkt) && (pkt[5] & 0x40);
    }
    if (!es->active || n == 0)
        return;

    if (es->len + n > PES_MAX) {
        es->active = false;
        d->stats.pes_invalid++;
        return;
    }
    if (es->len + n > es->cap) {
        size_t cap = es->cap ? es->cap * 2 : 65536;
        uint8_t *nb;

        while (cap < es->len + n)
            cap *= 2;
        nb = realloc(es->buf, cap);
        if (!nb) {
            es->active = false;
            return;
        }
        es->buf = nb;
        es->cap = cap;
    }
    memcpy(es->buf + es->len, pkt + off, n);
    es->len += n;
    es->last_idx = d->idx;

    if (!es->want && es->len >= 6) {
        size_t plen = ((size_t)es->buf[4] << 8) | es->buf[5];

        if (plen)
            es->want = 6 + plen;
    }
    if (es->want && es->len >= es->want)
        pes_complete(d, es);                /* no need to wait for the next PUSI */
}

/* ---- section-carried elementary streams ----------------------------- */

struct sec_ctx {
    struct demux *d;
    struct es_state *es;
};

static void es_section(void *opaque, const uint8_t *sec, int len)
{
    struct sec_ctx *c = opaque;
    struct demux *d = c->d;
    struct es_state *es = c->es;
    struct es_unit *u = unit_alloc(es, sec, (size_t)len, es->sec_start_idx, d->idx);

    es->sec_start_idx = d->idx;             /* a following one starts here */
    if (u)
        unit_queue(d, u);
}

static void section_packet(struct demux *d, struct es_state *es,
                           const uint8_t *pkt)
{
    int off = ts_payload_offset(pkt);
    struct sec_ctx c = { d, es };

    if (off >= TS_SIZE || !es->sasm)
        return;
    if (es->sasm->len == 0)
        es->sec_start_idx = d->idx;
    section_asm_push(es->sasm, pkt + off, TS_SIZE - off, ts_pusi(pkt),
                     es_section, &c);
}

/* ---- PSI ------------------------------------------------------------ */

static void es_release(struct es_state *es)
{
    free(es->buf);
    free(es->sasm);
    memset(es, 0, sizeof(*es));
}

/* Rebuilds the PID role table and the ES states from d->cur, keeping the
 * assembly state of streams that are still there. */
static void apply_program(struct demux *d)
{
    struct es_state old[MAX_ES];
    const struct program *p = &d->cur;

    memcpy(old, d->es, sizeof(old));
    memset(d->es, 0, sizeof(d->es));
    memset(d->role, ROLE_NONE, sizeof(d->role));
    memset(d->es_idx, NO_ES, sizeof(d->es_idx));

    for (int i = 0; i < p->n_es; i++) {
        const struct es_info *e = &p->es[i];
        struct es_state *es = &d->es[i];

        if (e->pid == PID_NULL || d->role[e->pid] == ROLE_ES)
            continue;                       /* unusable or listed twice */
        for (int k = 0; k < MAX_ES; k++) {
            if (old[k].used && old[k].pid == e->pid && old[k].kind == e->kind) {
                *es = old[k];
                old[k].used = false;
                break;
            }
        }
        if (!es->used) {
            es->used = true;
            es->pid = e->pid;
            es->kind = e->kind;
            if (es->kind == ES_SECTIONS)
                es->sasm = calloc(1, sizeof(*es->sasm));
        }
        d->role[e->pid] = ROLE_ES;
        d->es_idx[e->pid] = (uint8_t)i;
    }
    for (int k = 0; k < MAX_ES; k++) {
        if (old[k].used) {
            if (old[k].active)
                d->stats.pes_dropped++;
            es_release(&old[k]);
        }
    }

    /* PSI PIDs win if a broken PMT lists one of them as a stream. */
    d->role[PID_PAT] = ROLE_PAT;
    d->role[PID_SDT] = ROLE_SDT;
    if (d->have_pat && d->next.pmt_pid != PID_NULL)
        d->role[d->next.pmt_pid] = ROLE_PMT;
}

static void publish(struct demux *d)
{
    if (!d->have_pmt)
        return;
    if (d->published && memcmp(&d->cur, &d->next, sizeof(d->cur)) == 0)
        return;
    d->cur = d->next;
    d->published = true;
    d->stats.program_changes++;
    apply_program(d);
    d->cb.program(d->opaque, &d->cur);
}

static void pat_section(void *opaque, const uint8_t *sec, int len)
{
    struct demux *d = opaque;
    uint16_t ts_id, number, pmt_pid;
    uint8_t version;

    if (sec[0] != 0x00)
        return;
    if (!psi_section_ok(sec, len, 0x00)) {
        d->stats.psi_errors++;
        return;
    }
    if (!psi_pat_find(sec, len, d->want_program, &ts_id, &version, &number,
                      &pmt_pid))
        return;

    if (!d->have_pat || d->next.pmt_pid != pmt_pid || d->next.number != number) {
        /* New or moved PMT: forget the old one until the new one arrives. */
        if (d->have_pat && d->role[d->next.pmt_pid] == ROLE_PMT)
            d->role[d->next.pmt_pid] = ROLE_NONE;
        d->have_pmt = false;
        d->last_pmt_len = 0;
        section_asm_reset(&d->pmt_asm);
        if (d->next.number != number) {     /* a different service: its SDT */
            d->next.have_sdt = false;       /* entry has yet to be seen     */
            d->next.onid = 0;
            d->next.running_status = 0;
            d->next.free_ca = false;
            d->next.sdt_version = 0;
            d->next.sdt_desc_len = 0;
            memset(d->next.sdt_desc, 0, sizeof(d->next.sdt_desc));
        }
        if (pmt_pid != PID_PAT && pmt_pid != PID_SDT) {
            if (d->role[pmt_pid] == ROLE_ES)
                pes_drop(d, &d->es[d->es_idx[pmt_pid]]);
            d->role[pmt_pid] = ROLE_PMT;
        }
    }
    d->have_pat = true;
    d->next.ts_id = ts_id;
    d->next.number = number;
    d->next.pmt_pid = pmt_pid;
    d->next.pat_version = version;
    publish(d);
}

static void pmt_section(void *opaque, const uint8_t *sec, int len)
{
    struct demux *d = opaque;

    if (sec[0] != 0x02 || len < 5)
        return;
    if (((sec[3] << 8) | sec[4]) != d->next.number)
        return;                             /* another service on a shared PID */
    if (len == d->last_pmt_len && memcmp(sec, d->last_pmt, (size_t)len) == 0)
        return;                             /* the usual case: nothing new */
    if (!psi_section_ok(sec, len, 0x02)) {
        d->stats.psi_errors++;
        return;
    }
    /* Parse into a copy: a table that passes its CRC but is malformed
     * inside must leave the last good one in force, untouched. */
    d->scratch = d->next;
    if (!psi_parse_pmt(sec, len, &d->scratch)) {
        d->stats.psi_errors++;
        return;
    }
    d->next = d->scratch;
    memcpy(d->last_pmt, sec, (size_t)len);
    d->last_pmt_len = len;
    d->have_pmt = true;
    publish(d);
}

static void sdt_section(void *opaque, const uint8_t *sec, int len)
{
    struct demux *d = opaque;

    if (sec[0] != 0x42 || !d->have_pat)
        return;
    if (!psi_section_ok(sec, len, 0x42)) {
        d->stats.psi_errors++;
        return;
    }
    if (psi_parse_sdt(sec, len, &d->next))
        publish(d);
}

/* ---- PCR ------------------------------------------------------------ */

static void drop_partials(struct demux *d)
{
    for (int i = 0; i < MAX_ES; i++)
        if (d->es[i].used)
            pes_drop(d, &d->es[i]);
}

static void handle_pcr(struct demux *d, const uint8_t *pkt)
{
    int64_t raw = ts_pcr(pkt);

    d->stats.pcr_count++;

    if (!d->have_pcr) {
        d->have_pcr = true;
        d->pcr_raw = raw;
        d->pcr_unw = raw;
        hist_clear(d);
        hist_add(d, d->idx, d->pcr_unw);
        d->cb.pcr(d->opaque, d->pcr_unw, d->pcr_unw, false);
        return;
    }

    int64_t delta = mod_diff(raw, d->pcr_raw, PCR_MOD);
    bool jump = (pkt[5] & 0x80) || delta <= 0 || delta > PCR_GAP_MAX;

    /*
     * The PCR is back after anchors stood in for it (demux_time_anchor).
     * If it reads about what the anchors said, the gap since the last
     * real PCR is no break at all: the other streams never stopped, and
     * what they have half assembled must not be thrown away.
     */
    if (jump && d->anchored && !(pkt[5] & 0x80) && delta > 0) {
        stc_t off = d->pcr_unw + delta - idx_to_stc(d, d->idx);

        if (off > -PCR_GAP_MAX && off < PCR_GAP_MAX)
            jump = false;
    }
    d->anchored = false;
    d->pcr_raw = raw;
    if (jump) {
        stc_t expected = idx_to_stc(d, d->idx);

        /* Everything already received belongs to the old time line. */
        resolve_pending(d, true);
        drop_partials(d);
        d->pcr_unw += delta;
        hist_clear(d);
        hist_add(d, d->idx, d->pcr_unw);
        d->stats.pcr_jumps++;
        d->cb.pcr(d->opaque, d->pcr_unw, expected, true);
    } else {
        d->pcr_unw += delta;
        /* A real PCR outranks the stand-in anchors that covered for it
         * (demux_time_anchor): if it does not sit after them, start the
         * interpolation afresh from it rather than run time backwards. */
        if (d->hist_n && d->pcr_unw <= hist_at(d, d->hist_n - 1)->pcr) {
            resolve_pending(d, true);
            hist_clear(d);
        }
        hist_add(d, d->idx, d->pcr_unw);
        resolve_pending(d, false);
        d->cb.pcr(d->opaque, d->pcr_unw, d->pcr_unw, false);
    }
}

/* ---- per-packet entry point ----------------------------------------- */

enum { CC_OK, CC_LOST, CC_DUP };

static int cc_check(struct demux *d, const uint8_t *pkt, int pid)
{
    int cc = ts_cc(pkt);
    int last = d->last_cc[pid];
    bool has_payload = ts_afc(pkt) & 1;

    d->last_cc[pid] = (int8_t)cc;

    if (last < 0 || (ts_has_af_flags(pkt) && (pkt[5] & 0x80))) {
        d->dup_seen[pid] = 0;
        return CC_OK;
    }
    if (!has_payload)
        return cc == last ? CC_OK : CC_LOST;
    if (cc == ((last + 1) & 0x0F)) {
        d->dup_seen[pid] = 0;
        return CC_OK;
    }
    if (cc == last && !d->dup_seen[pid]) {
        d->dup_seen[pid] = 1;               /* one repeat is legal */
        return CC_DUP;
    }
    d->dup_seen[pid] = 0;
    return CC_LOST;
}

static void process_packet(struct demux *d, const uint8_t *pkt)
{
    if (pkt[0] != TS_SYNC) {
        d->stats.sync_errors++;
        return;
    }
    if (ts_tei(pkt)) {                      /* nothing in it can be trusted */
        d->stats.tei++;
        return;
    }

    int pid = ts_pid(pkt);
    if (pid == PID_NULL)
        return;

    int role = d->role[pid];
    bool is_pcr_pid = d->published && pid == d->cur.pcr_pid;
    bool ours = role != ROLE_NONE || is_pcr_pid;
    int cc = cc_check(d, pkt, pid);

    if (cc == CC_DUP && role == ROLE_ES) {
        /* Same counter twice is a legal repeat only if the packet really
         * is the same one. Otherwise 15 packets went missing in between. */
        if (memcmp(pkt, d->es[d->es_idx[pid]].last_pkt, TS_SIZE) != 0)
            cc = CC_LOST;
    }
    if (cc == CC_DUP) {
        d->stats.duplicates++;
        return;
    }
    if (cc == CC_LOST) {
        if (ours)
            d->stats.cc_errors++;
        else
            d->stats.cc_errors_other++;
        switch (role) {
        case ROLE_PAT: section_asm_reset(&d->pat_asm); break;
        case ROLE_PMT: section_asm_reset(&d->pmt_asm); break;
        case ROLE_SDT: section_asm_reset(&d->sdt_asm); break;
        case ROLE_ES:  pes_drop(d, &d->es[d->es_idx[pid]]); break;
        default: break;
        }
    }

    if (is_pcr_pid && ts_has_pcr(pkt))
        handle_pcr(d, pkt);

    int off = ts_payload_offset(pkt);

    switch (role) {
    case ROLE_PAT:
        if (off < TS_SIZE)
            section_asm_push(&d->pat_asm, pkt + off, TS_SIZE - off,
                             ts_pusi(pkt), pat_section, d);
        break;
    case ROLE_PMT:
        if (off < TS_SIZE)
            section_asm_push(&d->pmt_asm, pkt + off, TS_SIZE - off,
                             ts_pusi(pkt), pmt_section, d);
        break;
    case ROLE_SDT:
        if (off < TS_SIZE)
            section_asm_push(&d->sdt_asm, pkt + off, TS_SIZE - off,
                             ts_pusi(pkt), sdt_section, d);
        break;
    case ROLE_ES: {
        struct es_state *es = &d->es[d->es_idx[pid]];

        memcpy(es->last_pkt, pkt, TS_SIZE);
        if (ts_tsc(pkt)) {                  /* scrambled: cannot be parsed */
            d->stats.scrambled++;
            pes_drop(d, es);
            break;
        }
        if (es->kind == ES_SECTIONS)
            section_packet(d, es, pkt);
        else
            pes_packet(d, es, pkt);
        break;
    }
    default:
        break;
    }
}

void demux_packet(struct demux *d, const uint8_t *pkt)
{
    d->stats.packets++;
    process_packet(d, pkt);

    /* The PCR went quiet while data kept coming: stop waiting for it and
     * time what is queued by carrying the last known rate forward. */
    if (d->pend_head) {
        int64_t since = d->hist_n ? d->idx - hist_at(d, d->hist_n - 1)->idx
                                  : d->idx - d->pend_head->t0;

        if (since > d->pcr_wait_pkts)
            demux_resolve_now(d);
    }
    d->idx++;
}

void demux_resolve_now(struct demux *d)
{
    if (!d->pend_head)
        return;
    if (d->hist_n)
        for (struct es_unit *u = d->pend_head; u; u = u->next)
            d->stats.pcr_missing++;
    resolve_pending(d, true);
}

void demux_time_anchor(struct demux *d, stc_t input_stc)
{
    if (!d->have_pcr || d->hist_n == 0)
        return;
    if (input_stc <= hist_at(d, d->hist_n - 1)->pcr)
        return;                             /* time must move forward */
    hist_add(d, d->idx, input_stc);
    d->anchored = true;
    d->stats.pcr_missing++;
    resolve_pending(d, false);
}

void demux_sync_error(struct demux *d)
{
    d->stats.sync_errors++;
}

void demux_flush(struct demux *d)
{
    for (int i = 0; i < MAX_ES; i++) {
        struct es_state *es = &d->es[i];

        if (!es->used || es->kind != ES_PES || !es->active)
            continue;
        /* A PES packet of unbounded length ends where the input ends. One
         * that declared its length and stopped short is simply cut off. */
        if (es->want)
            pes_drop(d, es);
        else
            pes_complete(d, es);
    }
    resolve_pending(d, true);
}

const struct demux_stats *demux_get_stats(const struct demux *d)
{
    return &d->stats;
}

struct demux *demux_new(int program_number, const struct demux_cb *cb,
                        void *opaque)
{
    struct demux *d = calloc(1, sizeof(*d));

    if (!d)
        return NULL;
    d->cb = *cb;
    d->opaque = opaque;
    d->want_program = program_number;
    d->pcr_wait_pkts = PCR_WAIT_MIN;
    memset(d->last_cc, -1, sizeof(d->last_cc));
    memset(d->es_idx, NO_ES, sizeof(d->es_idx));
    d->role[PID_PAT] = ROLE_PAT;
    d->role[PID_SDT] = ROLE_SDT;
    d->next.pmt_pid = PID_NULL;
    return d;
}

void demux_free(struct demux *d)
{
    if (!d)
        return;
    for (int i = 0; i < MAX_ES; i++)
        if (d->es[i].used)
            es_release(&d->es[i]);
    while (d->pend_head) {
        struct es_unit *u = d->pend_head;

        d->pend_head = u->next;
        free(u);
    }
    free(d);
}
