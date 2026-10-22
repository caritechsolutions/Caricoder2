/*
 * caricoder3 - the IRDs (satellite receivers): their tuners, and whether
 * what they send matches what comes down
 * Copyright (c) 2026 CariTech Solutions
 *
 * Each IRD is asked for its tuner's state every minute (lock, C/N, Eb/N0,
 * BER, level, rates), from its own web interface; the readings are kept
 * in the history ("ird.NAME.cn", ...). The C/N it normally has is the
 * median of the last day's readings while locked.
 *
 * Every 10 minutes, when the IRD's raw tuner output (the whole transponder
 * as received) and its outputs (the remux our channels read) are known,
 * both are probed at the same time and compared service by service: the
 * same streams (PID and codec), nothing still scrambled, nothing without
 * data that the satellite has. A difference seen twice running is a
 * problem; a service that leaves an output is said once.
 *
 * Kinds of IRD understood: "upcom" (Upcom UC-IRD+, /goform/subBoard.cgi).
 * The supervisor turns the state into the watchdog's observations
 * (ird_watch) and gives an incident the readings of the IRD its channel
 * reads from (ird_for_url).
 */
#ifndef CC3_IRD_H
#define CC3_IRD_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define IRDS_MAX 16
#define IRD_OUTS 4

struct ird_conf {
    char name[48];
    char address[128];          /* its web interface: host or http://host:port */
    char kind[16];              /* "upcom" */
    char raw[256];              /* its raw tuner output (udp://...): "" none */
    char outputs[IRD_OUTS][256]; /* what our channels read from it */
    int n_out;
};

struct ird_opts {
    const char *binary;         /* this program, for the probes */
    int poll_s;                 /* the tuner, every (60) */
    int remux_s;                /* the comparison, every (600; 0: never) */
    int baseline_n;             /* readings before there is a normal C/N (30) */
    int probe_s;                /* each probe this long (5) */
};

struct ird_state {
    struct ird_conf c;
    bool answering;             /* the last poll had an answer */
    int fails;                  /* polls without one, in a row */
    int64_t last_try, last_ok;
    char error[160];
    bool have;                  /* tuner readings below */
    bool locked;
    double cn, ebn0, ber, strength, total_mbps, valid_mbps;
    double cn_normal;           /* < 0: not known yet */
    int n_samples;
    int low;                    /* readings in a row below the normal by the limit */
    /* the comparison */
    int64_t remux_t;            /* last done (0: never) */
    int remux_bad;              /* comparisons in a row with differences */
    int remux_services;         /* services compared */
    char remux_what[480];       /* the differences, in words ("" none) */
    char remux_error[160];      /* could not compare */
};

void ird_set(const struct ird_conf *list, int n, const struct ird_opts *o);
void ird_stop(void);
int ird_get(struct ird_state *out, int max);
/* Asks every IRD now, and compares now. */
void ird_check_now(void);

/* The watchdog's observations, from the last readings (every couple of
 * seconds, from the supervisor's loop). `cn_drop_db`: how far below its
 * normal the C/N must be. */
void ird_watch(int64_t t, int cn_drop_db);

/* The IRD whose outputs include `url` (the query ignored): its readings
 * as a JSON object; false when none. */
bool ird_for_url(FILE *f, const char *url);
/* [{...}, ...] every IRD, as ird_for_url writes one */
void ird_json(FILE *f);

/* The comparison itself (probe --brief JSON of the raw output and of an
 * output): how many differences; `what` says them. */
int ird_compare(const char *raw_json, const char *out_json, char *what, size_t len, int *services);

/* Parses an Upcom tuner answer ({"120": {"0": {...}}}); false if it is not one. */
bool ird_parse_upcom(const char *body, struct ird_state *st);

#endif
