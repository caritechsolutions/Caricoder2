/*
 * caricoder3 - transport stream demultiplexer
 * Copyright (c) 2026 CariTech Solutions
 */
#ifndef CC3_DEMUX_H
#define CC3_DEMUX_H

#include "cc3.h"
#include "psi.h"

struct demux;

struct demux_cb {
    /* The selected service appeared or changed (PMT / SDT). */
    void (*program)(void *opaque, const struct program *p);

    /* One complete PES packet or section, timed on the input clock.
     * Ownership of the unit passes to the callee (free() it). */
    void (*unit)(void *opaque, struct es_unit *u);

    /*
     * A PCR of the selected service was received. `pcr` is unwrapped.
     * `jump` is set when it does not follow on from the previous one
     * (went backwards, leapt forward, or the stream flagged it);
     * `expected` is then where the old time line had got to at this
     * packet, so the caller can decide how to splice.
     */
    void (*pcr)(void *opaque, stc_t pcr, stc_t expected, bool jump);
};

struct demux_stats {
    uint64_t packets;           /* every TS packet seen, all PIDs */
    uint64_t sync_errors;
    uint64_t tei;               /* transport_error_indicator set */
    uint64_t cc_errors;         /* on PIDs of the selected service */
    uint64_t cc_errors_other;   /* on any other PID of the multiplex */
    uint64_t duplicates;
    uint64_t scrambled;
    uint64_t pes_units;
    uint64_t pes_dropped;       /* partial PES thrown away after a loss */
    uint64_t pes_invalid;       /* no start code, oversize, bad header */
    uint64_t pes_len_mismatch;
    uint64_t sections;
    uint64_t psi_errors;        /* bad CRC or malformed table */
    uint64_t pcr_count;
    uint64_t pcr_jumps;
    uint64_t pcr_missing;       /* times the PCR had to be done without */
    uint64_t ts_out_of_range;   /* PTS/DTS nowhere near the PCR: dropped */
    uint64_t program_changes;
};

/* program_number 0 selects the first service listed in the PAT. */
struct demux *demux_new(int program_number, const struct demux_cb *cb,
                        void *opaque);
void demux_free(struct demux *d);

/* Feed exactly one 188-byte packet. */
void demux_packet(struct demux *d, const uint8_t *pkt);

/* Time and deliver everything that is waiting for its next PCR, now,
 * by carrying the last known rate forward. For a live input whose PCR
 * has gone silent while data keeps arriving. */
void demux_resolve_now(struct demux *d);

/* Stand-in for a PCR: "the packet that arrives next does so at this
 * time on the input clock". A live input whose PCR has gone silent is
 * kept timed this way, from the clock that was recovered while the PCR
 * was there. */
void demux_time_anchor(struct demux *d, stc_t input_stc);

/* A byte that should have been a sync byte was not. */
void demux_sync_error(struct demux *d);

/* End of input: deliver whatever is still being assembled. */
void demux_flush(struct demux *d);

const struct demux_stats *demux_get_stats(const struct demux *d);

#endif
