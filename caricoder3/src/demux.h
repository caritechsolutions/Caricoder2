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
     *
     * An unusual PCR is only reported once the next one has borne it out
     * (see handle_pcr), and it is that next one which is reported. `since`
     * is then the reading of the earlier one: content timed after `since`
     * arrived after the clock was first seen at its new reading. For an
     * ordinary PCR `since` equals `pcr`.
     */
    void (*pcr)(void *opaque, stc_t pcr, stc_t expected, bool jump,
                stc_t since);
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
    uint64_t pcr_glitches;      /* single PCRs the next one contradicted: ignored */
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
 * was there. `doubt`: the caller did not believe the last PCRs it was
 * given (they read far from its clock), so its time and theirs are not
 * on one line; the stand-ins then replace them rather than follow on. */
void demux_time_anchor(struct demux *d, stc_t input_stc, bool doubt);

/*
 * Order of the callbacks when an ordinary PCR arrives. Off (the default):
 * the units it makes timeable are delivered first, then the PCR is
 * reported. On: the PCR is reported first. A live pipeline wants it on,
 * so that it has judged the PCR against its clock before it receives the
 * content that arrived with it.
 */
void demux_set_pcr_first(struct demux *d, bool on);

/* A byte that should have been a sync byte was not. */
void demux_sync_error(struct demux *d);

/* End of input: deliver whatever is still being assembled. */
void demux_flush(struct demux *d);

const struct demux_stats *demux_get_stats(const struct demux *d);

#endif
