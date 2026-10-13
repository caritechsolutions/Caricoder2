/*
 * caricoder3 - the pipeline: demux -> (codecs, later) -> mux, plus clocking
 * Copyright (c) 2026 CariTech Solutions
 */
#ifndef CC3_ENGINE_H
#define CC3_ENGINE_H

#include "cc3.h"
#include "demux.h"
#include "mux.h"

struct engine;

struct engine_cfg {
    int  program;               /* 0 = first service in the PAT */
    bool live;                  /* paced by the wall clock; false = file run */
    int  delay_ms;              /* buffer between input and output */
    struct mux_cfg mux;
    const char *service_name;       /* NULL = keep the input's */
    const char *service_provider;
};

struct engine_stats {
    struct demux_stats in;
    struct mux_stats out;
    uint64_t in_bytes;
    uint64_t in_datagrams;
    uint64_t rtp_datagrams;
    uint64_t discontinuities;   /* output time line re-based */
    unsigned held_units;        /* waiting on a clock question right now */
    int64_t  ts_offset;         /* 27 MHz ticks added to input timestamps */
    bool     have_program;
    bool     clock_locked;
    bool     started;
    double   clock_ppm;
    double   clock_jitter_us;
    struct program prog;
};

struct engine *engine_new(const struct engine_cfg *cfg, mux_out_fn out,
                          void *opaque);
void engine_free(struct engine *e);

/* One UDP datagram: raw TS or RTP-wrapped TS. */
void engine_push_datagram(struct engine *e, const uint8_t *buf, size_t len,
                          int64_t rx_ns);

/* A piece of a byte stream (file): finds and keeps packet sync itself. */
void engine_push_stream(struct engine *e, const uint8_t *buf, size_t len);

/* File mode only: end of input, write out everything still queued. */
void engine_finish(struct engine *e);

/*
 * Live mode only, called from the output thread: emits every packet that
 * is due at now_ns and returns the time the next one is due. `fill` is
 * the number of packets waiting in a partly built datagram of `dgram`
 * packets, so the wake-up can be aimed at the packet that completes it.
 */
int64_t engine_run(struct engine *e, int64_t now_ns, int dgram, int fill);

void engine_get_stats(struct engine *e, struct engine_stats *out);

#endif
