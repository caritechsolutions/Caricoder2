/*
 * caricoder3 - transport stream multiplexer
 * Copyright (c) 2026 CariTech Solutions
 */
#ifndef CC3_MUX_H
#define CC3_MUX_H

#include "cc3.h"
#include "psi.h"

struct mux;

struct mux_cfg {
    int64_t muxrate;        /* bits per second of the (virtual) output */
    bool    vbr;            /* true: null packets are not emitted */
    int     pcr_interval_ms;
    int     psi_interval_ms;    /* PAT + PMT */
    int     sdt_interval_ms;
    int     max_late_ms;    /* queued units older than this are discarded */
};

struct mux_stats {
    uint64_t packets;       /* packets handed to the output */
    uint64_t slots;         /* packet slots elapsed (== packets when CBR) */
    uint64_t nulls;
    uint64_t psi;
    uint64_t pcr_only;      /* adaptation-only packets carrying just a PCR */
    uint64_t data;
    uint64_t units;
    uint64_t dropped_units; /* too late or too early to send, queue full,
                             * or their stream was removed */
    uint64_t late_units;    /* finished after their own DTS */
    uint64_t skips;         /* output clock jumped forward (process stalled) */
    int64_t  max_late;      /* worst packet lateness since last read, ticks */
    uint64_t queued_bytes;
    uint32_t queued_units;
};

typedef void (*mux_out_fn)(void *opaque, const uint8_t *pkt);

struct mux *mux_new(const struct mux_cfg *cfg, mux_out_fn out, void *opaque);
void mux_free(struct mux *m);

/* Sets or replaces the service description. Streams that keep their PID
 * keep their continuity counter and queue. */
void mux_set_program(struct mux *m, const struct program *p);
/* Several programs in one output (one per rendition of a channel): one
 * PAT and SDT for all, a PMT each. Elementary streams are shared by PID:
 * an audio PID listed in every PMT is carried once. The PCR is that of
 * the first program. */
void mux_set_programs(struct mux *m, const struct program *list, int n);

/* Fixes the STC of the first packet slot. */
void mux_start(struct mux *m, stc_t stc0);
bool mux_started(const struct mux *m);

/* Moves the output clock forward to `stc` without emitting the slots in
 * between, and flags the step on the next PCR. For when the process was
 * not running for a while and the missed time cannot usefully be sent. */
void mux_skip_to(struct mux *m, stc_t stc);

/* Units flagged tentative were stamped before it was known whether the
 * input clock had stepped. It had: move those still waiting by `delta`
 * (time stamps included), so they belong to the new clock like the rest. */
void mux_restamp_tentative(struct mux *m, stc_t delta);

/* It had not: they are right as they are. */
void mux_clear_tentative(struct mux *m);

/* Nobody will ever know: the PCR has gone, and content timed from the
 * wall clock is about to be queued behind them. They are due at `stc`,
 * at once, so that they neither block what follows nor are left to rot. */
void mux_send_tentative_now(struct mux *m, stc_t stc);

/* Discards everything queued (counted as dropped). */
void mux_drop_all(struct mux *m);

/* Queues a unit (ownership passes to the mux). */
void mux_push(struct mux *m, struct es_unit *u);

/* Emits every packet slot whose STC is <= until, at most max_slots of
 * them (0 = no limit). Returns the number of slots consumed. */
int mux_run(struct mux *m, stc_t until, int max_slots);

/* STC of the next slot to be emitted. */
stc_t mux_next_stc(const struct mux *m);

/* STC of the slot `n` slots after the next one. */
stc_t mux_stc_after(const struct mux *m, int n);

/* Earliest STC at which something other than a null packet is due. */
stc_t mux_next_event(const struct mux *m);

/* True when no unit is queued. */
bool mux_idle(const struct mux *m);

/* Copies the counters; max_late restarts from zero. */
void mux_get_stats(struct mux *m, struct mux_stats *out);

#endif
