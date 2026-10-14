/*
 * caricoder3 - transport stream multiplexer
 * Copyright (c) 2026 CariTech Solutions
 *
 * The output is a train of fixed-duration packet slots. Slot i starts at
 *
 *     STC(i) = STC(0) + i * 188 * 8 * 27e6 / muxrate
 *
 * computed with an integer remainder so it never drifts. A PCR written
 * into slot i is simply STC(i), so PCR accuracy by byte position is exact
 * by construction (under one 27 MHz tick) no matter how the packets are
 * later paced onto the wire.
 *
 * Each slot is filled, in priority order, by: a PCR that has fallen due,
 * a PSI packet, the queued packet with the earliest send time that has
 * been reached, or a null packet. In VBR mode the same slots run but the
 * null packets are simply not handed to the output.
 */
#include "mux.h"

#include <stdlib.h>
#include <string.h>

#include "log.h"

#define SLOT_NUM        (188LL * 8 * STC_HZ)
#define PSIQ_MAX        32
#define QUEUE_BYTES_MAX (256ull * 1024 * 1024)
#define MAX_EARLY       (15 * STC_HZ)   /* nothing is queued further ahead */
#define NO_STREAM       0xFF

struct mstream {
    bool     used;
    uint16_t pid;
    uint8_t  kind;
    struct es_unit *head, *tail;
    size_t   off;               /* bytes of head already sent */
    uint32_t k, npk;            /* packets sent / expected for head */
    uint32_t q_units;
    bool     orphan;            /* no longer in the service: only finishing
                                 * the unit that was already part-way out */
};

struct psi_table {
    bool     present;
    uint16_t pid;
    int      version;
    int      len;
    uint8_t  sec[PSI_MAX];
    int      ref_len;           /* same table built with version 0 ... */
    uint8_t  ref[PSI_MAX];      /* ... used to detect content changes */
};

struct mux {
    struct mux_cfg cfg;
    mux_out_fn out;
    void *opaque;

    bool    started;
    stc_t   stc;                /* start of the next slot */
    int64_t rem;                /* fractional part of stc, in 1/muxrate */
    int64_t slot_q, slot_r;

    bool    have_prog;
    struct program prog[MAX_PROGRAMS];
    int     n_prog;
    struct mstream st[MAX_ES];
    uint8_t pid_idx[PID_COUNT];
    /* Continuity counter of the next packet, per output PID. Kept by PID
     * and not by stream or table so that it stays unbroken even when a
     * PID changes hands (a stream replaced, a PMT moved). */
    uint8_t cc[PID_COUNT];
    uint8_t warned[PID_COUNT / 8];  /* PIDs already complained about */
    uint16_t pcr_pid;
    int     pcr_stream;         /* index into st[], or -1 */

    stc_t   pcr_interval, psi_interval, sdt_interval, max_late;
    stc_t   next_pcr, next_psi, next_sdt;

    struct psi_table pat, pmt[MAX_PROGRAMS], sdt;
    bool    sdt_active;         /* the service currently has an SDT entry */
    uint8_t psiq[PSIQ_MAX][TS_SIZE];
    int     psiq_head, psiq_n;
    bool    psi_urgent;         /* new tables go out ahead of everything */
    bool    pcr_disc;           /* next PCR carries discontinuity_indicator */

    uint64_t q_bytes;
    struct mux_stats stats;
};

static const uint8_t null_packet_head[4] = { TS_SYNC, 0x1F, 0xFF, 0x10 };

static void emit(struct mux *m, const uint8_t *pkt)
{
    m->stats.packets++;
    m->out(m->opaque, pkt);
}

/* ---- PSI ------------------------------------------------------------ */

typedef int (*psi_build_fn)(const struct program *list, int n, int version, uint8_t *sec);

static int build_pmt_one(const struct program *list, int n, int version, uint8_t *sec)
{
    (void)n;
    return psi_build_pmt(list, version, sec);
}

/*
 * Rebuilds a table. The version number only moves when the content
 * really changed, and it always moves then: a downstream device that
 * caches tables by version must never see new content under an old
 * number. Returns true when the table changed.
 */
static bool table_update(struct psi_table *t, psi_build_fn build,
                         const struct program *p, int n, uint16_t pid, int in_version)
{
    uint8_t ref[PSI_MAX];
    int ref_len = build(p, n, 0, ref);

    if (ref_len < 0) {
        log_err("mux: table on PID 0x%04X does not fit in one section", pid);
        return false;
    }
    if (t->present && t->pid == pid && t->ref_len == ref_len &&
        memcmp(t->ref, ref, (size_t)ref_len) == 0)
        return false;

    if (!t->present)
        t->version = in_version & 0x1F;
    else if ((in_version & 0x1F) != t->version)
        t->version = in_version & 0x1F;
    else
        t->version = (t->version + 1) & 0x1F;

    t->present = true;
    t->pid = pid;
    t->ref_len = ref_len;
    memcpy(t->ref, ref, (size_t)ref_len);
    t->len = build(p, n, t->version, t->sec);
    return true;
}

static void psi_enqueue(struct mux *m, struct psi_table *t)
{
    int off = 0;

    if (!t->present)
        return;
    /* Whole table or nothing: if the queue has not drained since last
     * time (output rate too low for the repetition asked for), this
     * repetition is skipped rather than sent cut short. */
    if (m->psiq_n + (t->len + 1 + 183) / 184 > PSIQ_MAX)
        return;
    while (off < t->len) {
        uint8_t *pkt = m->psiq[(m->psiq_head + m->psiq_n) % PSIQ_MAX];
        int o = 4;

        pkt[0] = TS_SYNC;
        pkt[1] = (uint8_t)((off == 0 ? 0x40 : 0) | (t->pid >> 8));
        pkt[2] = (uint8_t)t->pid;
        pkt[3] = 0x10;                      /* counter is set in psi_pop() */
        if (off == 0)
            pkt[o++] = 0x00;                /* pointer_field */

        int n = t->len - off;
        if (n > TS_SIZE - o)
            n = TS_SIZE - o;
        memcpy(pkt + o, t->sec + off, (size_t)n);
        memset(pkt + o + n, 0xFF, (size_t)(TS_SIZE - o - n));
        off += n;
        m->psiq_n++;
    }
}

static void psi_pop(struct mux *m)
{
    uint8_t *pkt = m->psiq[m->psiq_head];
    int pid = ts_pid(pkt);

    /* Stamped at the moment of sending, like every other packet, so the
     * count is right even if something else shares this PID meanwhile. */
    pkt[3] = (uint8_t)(0x10 | m->cc[pid]);
    m->cc[pid] = (m->cc[pid] + 1) & 0x0F;
    m->stats.psi++;
    emit(m, pkt);
    m->psiq_head = (m->psiq_head + 1) % PSIQ_MAX;
    m->psiq_n--;
}

/* ---- streams -------------------------------------------------------- */

static size_t unit_wire_len(const struct mstream *s, const struct es_unit *u)
{
    return u->len + (s->kind == ES_SECTIONS ? 1 : 0);   /* + pointer_field */
}

static void head_begin(struct mstream *s)
{
    s->off = 0;
    s->k = 0;
    s->npk = s->head ? (uint32_t)((unit_wire_len(s, s->head) + 183) / 184) : 0;
    if (s->head && s->npk == 0)
        s->npk = 1;
}

/* Send time of the next packet of the unit at the head of the queue:
 * the unit's packets are spread evenly over its window. */
static stc_t next_sched(const struct mstream *s)
{
    const struct es_unit *u = s->head;
    uint32_t k = s->k < s->npk ? s->k : s->npk - 1;

    return u->t0 + (u->t1 - u->t0) * (int64_t)k / (int64_t)s->npk;
}

static void head_pop(struct mux *m, struct mstream *s)
{
    struct es_unit *u = s->head;

    s->head = u->next;
    if (!s->head)
        s->tail = NULL;
    s->q_units--;
    m->stats.queued_units--;
    m->q_bytes -= u->len + sizeof(*u);
    free(u);
    head_begin(s);
    if (s->orphan && !s->head)
        memset(s, 0, sizeof(*s));           /* its last unit is out: gone */
}

static void stream_clear(struct mux *m, struct mstream *s)
{
    while (s->head) {
        head_pop(m, s);
        m->stats.dropped_units++;
    }
}

/* One packet of the head unit of stream s into the slot at `now`. */
static void stream_packet(struct mux *m, struct mstream *s, stc_t now,
                          bool with_pcr)
{
    struct es_unit *u = s->head;
    size_t total = unit_wire_len(s, u);
    size_t remaining = total - s->off;
    bool first = s->off == 0;
    uint8_t pkt[TS_SIZE];
    int o;

    int64_t late = now - next_sched(s);
    if (late > m->stats.max_late)
        m->stats.max_late = late;

    pkt[0] = TS_SYNC;
    pkt[1] = (uint8_t)((first ? 0x40 : 0) | (s->pid >> 8));
    pkt[2] = (uint8_t)s->pid;

    if (s->kind == ES_SECTIONS) {
        /* Sections pad with 0xFF in the payload; no adaptation field. */
        size_t n = remaining < 184 ? remaining : 184;
        const uint8_t *src = u->data + (first ? 0 : s->off - 1);
        size_t copy = n;

        pkt[3] = (uint8_t)(0x10 | m->cc[s->pid]);
        o = 4;
        if (first) {
            pkt[o++] = 0x00;                /* pointer_field */
            copy = n - 1;
        }
        memcpy(pkt + o, src, copy);
        memset(pkt + o + copy, 0xFF, (size_t)TS_SIZE - (size_t)o - copy);
        s->off += n;
    } else {
        /* PES pads with adaptation field stuffing. */
        bool rai = first && u->rai;
        bool need_af = with_pcr || rai;
        int af_body = need_af ? 1 + (with_pcr ? 6 : 0) : 0;
        int room = TS_SIZE - 4 - (need_af ? 1 + af_body : 0);
        int n = remaining < (size_t)room ? (int)remaining : room;
        int stuff = room - n;

        if (!need_af && stuff == 0) {
            pkt[3] = (uint8_t)(0x10 | m->cc[s->pid]);
            o = 4;
        } else {
            int af_len = need_af ? af_body + stuff : stuff - 1;

            pkt[3] = (uint8_t)(0x30 | m->cc[s->pid]);
            pkt[4] = (uint8_t)af_len;
            o = 5;
            if (af_len > 0) {
                pkt[o++] = (uint8_t)((rai ? 0x40 : 0) | (with_pcr ? 0x10 : 0) |
                                     (with_pcr && m->pcr_disc ? 0x80 : 0));
                if (with_pcr) {
                    ts_write_pcr(pkt + o, now);
                    o += 6;
                    m->pcr_disc = false;
                }
                while (o < 5 + af_len)
                    pkt[o++] = 0xFF;
            }
        }
        memcpy(pkt + o, u->data + s->off, (size_t)n);
        s->off += (size_t)n;
    }

    m->cc[s->pid] = (m->cc[s->pid] + 1) & 0x0F;
    s->k++;
    m->stats.data++;
    emit(m, pkt);

    if (s->off >= total) {
        if (u->dts != NOPTS && now > u->dts * STC_PER_90K)
            m->stats.late_units++;
        m->stats.units++;
        head_pop(m, s);
    }
}

/* Adaptation-field-only packet: keeps the PCR flowing when the PCR PID
 * has nothing to send. It has no payload, so the continuity counter is
 * the one of the previous packet on that PID. */
static void pcr_only_packet(struct mux *m, stc_t now)
{
    uint8_t pkt[TS_SIZE];
    int cc = (m->cc[m->pcr_pid] - 1) & 0x0F;

    pkt[0] = TS_SYNC;
    pkt[1] = (uint8_t)(m->pcr_pid >> 8);
    pkt[2] = (uint8_t)m->pcr_pid;
    pkt[3] = (uint8_t)(0x20 | cc);
    pkt[4] = 183;
    pkt[5] = (uint8_t)(0x10 | (m->pcr_disc ? 0x80 : 0));
    m->pcr_disc = false;
    ts_write_pcr(pkt + 6, now);
    memset(pkt + 12, 0xFF, TS_SIZE - 12);
    m->stats.pcr_only++;
    emit(m, pkt);
}

/* Hopelessly late units go, and so does anything timed absurdly far
 * ahead (it would block its stream until then). Never one that is
 * already half sent. */
static void stream_prune(struct mux *m, struct mstream *s, stc_t now)
{
    while (s->head && s->off == 0 &&
           (now - s->head->t1 > m->max_late || s->head->t0 - now > MAX_EARLY)) {
        head_pop(m, s);
        m->stats.dropped_units++;
    }
}

static struct mstream *pick_stream(struct mux *m, stc_t now)
{
    struct mstream *best = NULL;
    stc_t best_t = 0;

    for (int i = 0; i < MAX_ES; i++) {
        struct mstream *s = &m->st[i];

        if (!s->used)
            continue;
        stream_prune(m, s, now);
        if (!s->head)
            continue;

        stc_t t = next_sched(s);
        if (t <= now && (!best || t < best_t)) {
            best = s;
            best_t = t;
        }
    }
    return best;
}

static void slot(struct mux *m)
{
    stc_t now = m->stc;

    if (m->have_prog) {
        if (now >= m->next_psi) {
            psi_enqueue(m, &m->pat);
            for (int i = 0; i < m->n_prog; i++)
                psi_enqueue(m, &m->pmt[i]);
            m->next_psi = now - m->next_psi > m->psi_interval
                        ? now + m->psi_interval
                        : m->next_psi + m->psi_interval;
        }
        if (m->sdt_active && now >= m->next_sdt) {
            psi_enqueue(m, &m->sdt);
            m->next_sdt = now - m->next_sdt > m->sdt_interval
                        ? now + m->sdt_interval
                        : m->next_sdt + m->sdt_interval;
        }

        /* At start-up and after a change of service the tables lead, so
         * no receiver sees a stream before the PMT that announces it. */
        if (m->psi_urgent) {
            if (m->psiq_n) {
                psi_pop(m);
                return;
            }
            m->psi_urgent = false;
        }

        /*
         * PCR. From the nominal interval on, the next packet of the PCR
         * stream carries it for free. Only if that stream stays silent
         * for a further quarter interval is a packet spent on a PCR alone.
         */
        if (m->pcr_pid != PID_NULL && now >= m->next_pcr) {
            struct mstream *ps = m->pcr_stream >= 0 ? &m->st[m->pcr_stream] : NULL;

            if (ps)
                stream_prune(m, ps, now);
            if (ps && ps->kind == ES_PES && ps->head && next_sched(ps) <= now) {
                m->next_pcr = now + m->pcr_interval;
                stream_packet(m, ps, now, true);
                return;
            }
            if (now >= m->next_pcr + m->pcr_interval / 4) {
                m->next_pcr = now + m->pcr_interval;
                pcr_only_packet(m, now);
                return;
            }
        }

        if (m->psiq_n) {
            psi_pop(m);
            return;
        }

        struct mstream *s = pick_stream(m, now);
        if (s) {
            stream_packet(m, s, now, false);
            return;
        }
    }

    if (!m->cfg.vbr) {
        uint8_t pkt[TS_SIZE];

        memcpy(pkt, null_packet_head, 4);
        memset(pkt + 4, 0xFF, TS_SIZE - 4);
        m->stats.nulls++;
        emit(m, pkt);
    }
}

/* ---- public API ----------------------------------------------------- */

int mux_run(struct mux *m, stc_t until, int max_slots)
{
    int n = 0;

    if (!m->started)
        return 0;
    while (m->stc <= until && (max_slots <= 0 || n < max_slots)) {
        slot(m);
        m->stc += m->slot_q;
        m->rem += m->slot_r;
        if (m->rem >= m->cfg.muxrate) {
            m->rem -= m->cfg.muxrate;
            m->stc++;
        }
        m->stats.slots++;
        n++;
    }
    return n;
}

stc_t mux_next_stc(const struct mux *m)
{
    return m->stc;
}

stc_t mux_stc_after(const struct mux *m, int n)
{
    return m->stc + n * m->slot_q + (m->rem + n * m->slot_r) / m->cfg.muxrate;
}

stc_t mux_next_event(const struct mux *m)
{
    stc_t t = INT64_MAX;

    if (m->psiq_n)
        return m->stc;
    if (m->have_prog) {
        t = m->next_psi;
        if (m->sdt_active && m->next_sdt < t)
            t = m->next_sdt;
        if (m->pcr_pid != PID_NULL && m->next_pcr < t)
            t = m->next_pcr;
        for (int i = 0; i < MAX_ES; i++) {
            const struct mstream *s = &m->st[i];

            if (s->used && s->head) {
                stc_t ts = next_sched(s);
                if (ts < t)
                    t = ts;
            }
        }
    }
    return t < m->stc ? m->stc : t;
}

bool mux_idle(const struct mux *m)
{
    return m->stats.queued_units == 0 && m->psiq_n == 0;
}

void mux_start(struct mux *m, stc_t stc0)
{
    m->stc = stc0;
    m->rem = 0;
    m->started = true;
    m->next_pcr = m->next_psi = m->next_sdt = stc0;
    m->psi_urgent = true;
}

bool mux_started(const struct mux *m)
{
    return m->started;
}

void mux_skip_to(struct mux *m, stc_t stc)
{
    if (!m->started || stc <= m->stc)
        return;
    /* A unit that was part-way out when time stopped is still finished,
     * late as it is: cutting it short would put a malformed PES packet on
     * the wire, and a late one is merely ignored by a decoder. */
    m->stc = stc;
    m->rem = 0;
    m->pcr_disc = true;                     /* the PCR steps: say so */
    m->next_pcr = stc;
    if (m->next_psi < stc)
        m->next_psi = stc;
    if (m->next_sdt < stc)
        m->next_sdt = stc;
    m->stats.skips++;
}

void mux_restamp_tentative(struct mux *m, stc_t delta)
{
    int64_t d90 = delta / STC_PER_90K;

    for (int i = 0; i < MAX_ES; i++) {
        struct mstream *s = &m->st[i];

        for (struct es_unit *u = s->head; u; u = u->next) {
            if (!u->tentative)
                continue;
            u->tentative = false;
            if (u == s->head && s->off > 0)
                continue;                   /* part of it is already out */
            u->t0 += delta;
            u->t1 += delta;
            if (u->pts != NOPTS) {
                u->pts += d90;
                if (u->pts_off >= 0)
                    pes_patch_ts(u->data + u->pts_off, u->pts);
            }
            if (u->dts != NOPTS) {
                u->dts += d90;
                if (u->dts_off >= 0)
                    pes_patch_ts(u->data + u->dts_off, u->dts);
            }
        }
    }
}

void mux_send_tentative_now(struct mux *m, stc_t stc)
{
    for (int i = 0; i < MAX_ES; i++) {
        struct mstream *s = &m->st[i];

        for (struct es_unit *u = s->head; u; u = u->next) {
            if (!u->tentative)
                continue;
            u->tentative = false;
            if (u == s->head && s->off > 0)
                continue;                   /* part of it is already out */
            u->t0 = stc;
            u->t1 = stc + 1;
        }
    }
}

void mux_clear_tentative(struct mux *m)
{
    for (int i = 0; i < MAX_ES; i++)
        for (struct es_unit *u = m->st[i].head; u; u = u->next)
            u->tentative = false;
}

void mux_drop_all(struct mux *m)
{
    for (int i = 0; i < MAX_ES; i++)
        if (m->st[i].used)
            stream_clear(m, &m->st[i]);
}

void mux_push(struct mux *m, struct es_unit *u)
{
    int idx = m->pid_idx[u->pid & 0x1FFF];
    struct mstream *s;

    if (idx == NO_STREAM || u->len == 0 ||
        m->q_bytes + u->len + sizeof(*u) > QUEUE_BYTES_MAX) {
        m->stats.dropped_units++;
        free(u);
        return;
    }
    s = &m->st[idx];
    u->next = NULL;
    if (s->tail)
        s->tail->next = u;
    else
        s->head = u;
    s->tail = u;
    s->q_units++;
    m->stats.queued_units++;
    m->q_bytes += u->len + sizeof(*u);
    if (s->head == u)
        head_begin(s);
}

void mux_set_program(struct mux *m, const struct program *p)
{
    mux_set_programs(m, p, 1);
}

void mux_set_programs(struct mux *m, const struct program *list, int n)
{
    struct mstream old[MAX_ES];
    bool changed, any_sdt = false;
    const struct program *p = &list[0];     /* the first: PCR, ts id, versions */
    int n_st = 0;

    if (n < 1)
        return;
    if (n > MAX_PROGRAMS)
        n = MAX_PROGRAMS;
    memcpy(old, m->st, sizeof(old));
    memset(m->st, 0, sizeof(m->st));
    memset(m->pid_idx, NO_STREAM, sizeof(m->pid_idx));

    for (int k = 0; k < n; k++)
    for (int i = 0; i < list[k].n_es; i++) {
        const struct es_info *e = &list[k].es[i];
        struct mstream *s;
        bool own_pmt = false;

        for (int j = 0; j < n; j++)
            own_pmt |= e->pid == list[j].pmt_pid;
        /* The same PID in two programs is one stream, carried once. */
        if (m->pid_idx[e->pid] != NO_STREAM && k > 0 &&
            m->st[m->pid_idx[e->pid]].kind == e->kind)
            continue;
        /* A stream may not sit on a PID the mux uses for its own tables. */
        if (e->pid == PID_NULL || e->pid == PID_PAT || e->pid == PID_SDT ||
            own_pmt || m->pid_idx[e->pid] != NO_STREAM || n_st == MAX_ES) {
            if (!(m->warned[e->pid >> 3] & (1 << (e->pid & 7)))) {
                m->warned[e->pid >> 3] |= (uint8_t)(1 << (e->pid & 7));
                log_warn("mux: stream on PID 0x%04X cannot be carried", e->pid);
            }
            continue;
        }
        s = &m->st[n_st];
        for (int j = 0; j < MAX_ES; j++) {
            if (old[j].used && old[j].pid == e->pid && old[j].kind == e->kind) {
                *s = old[j];
                s->orphan = false;
                old[j].used = false;
                break;
            }
        }
        if (!s->used) {
            s->used = true;
            s->pid = e->pid;
            s->kind = e->kind;
        }
        m->pid_idx[e->pid] = (uint8_t)n_st++;
    }
    /*
     * Streams that have left the service. Whatever they had queued goes,
     * with one exception: a unit that is already part-way out is finished
     * first, because half a PES packet on the wire is a malformed one.
     * The stream lingers in a spare slot just long enough for that.
     */
    for (int k = 0; k < MAX_ES; k++) {
        struct mstream *o = &old[k];
        int slot = -1;

        if (!o->used)
            continue;
        if (o->head && o->off > 0 && m->pid_idx[o->pid] == NO_STREAM &&
            o->pid != PID_PAT && o->pid != PID_SDT) {
            bool own_pmt = false;

            for (int j = 0; j < n; j++)
                own_pmt |= o->pid == list[j].pmt_pid;
            for (int j = MAX_ES - 1; j >= n_st && slot < 0 && !own_pmt; j--)
                if (!m->st[j].used)
                    slot = j;
        }
        if (slot < 0) {
            stream_clear(m, o);
            continue;
        }
        while (o->head->next) {
            struct es_unit *u = o->head->next;

            o->head->next = u->next;
            o->q_units--;
            m->stats.queued_units--;
            m->stats.dropped_units++;
            m->q_bytes -= u->len + sizeof(*u);
            free(u);
        }
        o->tail = o->head;
        o->orphan = true;
        m->st[slot] = *o;
    }

    memcpy(m->prog, list, sizeof(list[0]) * (size_t)n);
    m->n_prog = n;
    m->pcr_pid = p->pcr_pid;
    m->pcr_stream = m->pid_idx[p->pcr_pid & 0x1FFF] == NO_STREAM
                  ? -1 : m->pid_idx[p->pcr_pid & 0x1FFF];

    changed = table_update(&m->pat, psi_build_pat, list, n, PID_PAT, p->pat_version);
    for (int k = 0; k < n; k++)
        changed |= table_update(&m->pmt[k], build_pmt_one, &list[k], 1, list[k].pmt_pid,
                                list[k].pmt_version);
    for (int k = 0; k < n; k++)
        any_sdt |= list[k].have_sdt;
    /* When the service has no SDT entry the table stops being sent, but
     * its last version number is remembered (sdt.present stays set), so
     * that whatever comes next is never mistaken for the old content. */
    if (any_sdt) {
        if (table_update(&m->sdt, psi_build_sdt, list, n, PID_SDT, p->sdt_version) ||
            !m->sdt_active)
            m->next_sdt = m->stc;
        m->sdt_active = true;
    } else {
        m->sdt_active = false;
    }
    if (changed || !m->have_prog) {
        m->next_psi = m->stc;               /* announce it straight away */
        m->psi_urgent = true;
    }
    if (!m->have_prog)
        m->next_pcr = m->stc;
    m->have_prog = true;
}

void mux_get_stats(struct mux *m, struct mux_stats *out)
{
    m->stats.queued_bytes = m->q_bytes;
    *out = m->stats;
    m->stats.max_late = 0;
}

struct mux *mux_new(const struct mux_cfg *cfg, mux_out_fn out, void *opaque)
{
    struct mux *m;

    if (cfg->muxrate <= 0)
        return NULL;
    m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->cfg = *cfg;
    m->out = out;
    m->opaque = opaque;
    m->slot_q = SLOT_NUM / cfg->muxrate;
    m->slot_r = SLOT_NUM % cfg->muxrate;
    m->pcr_interval = cfg->pcr_interval_ms * STC_PER_MS;
    m->psi_interval = cfg->psi_interval_ms * STC_PER_MS;
    m->sdt_interval = cfg->sdt_interval_ms * STC_PER_MS;
    m->max_late = cfg->max_late_ms * STC_PER_MS;
    m->pcr_pid = PID_NULL;
    m->pcr_stream = -1;
    memset(m->pid_idx, NO_STREAM, sizeof(m->pid_idx));
    return m;
}

void mux_free(struct mux *m)
{
    if (!m)
        return;
    for (int i = 0; i < MAX_ES; i++)
        if (m->st[i].used)
            stream_clear(m, &m->st[i]);
    free(m);
}
