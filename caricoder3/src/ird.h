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
 * Every few hours (3 by default), when the IRD's raw tuner output (the whole transponder
 * as received) and its outputs (the remux our channels read) are known,
 * both are probed at the same time and compared service by service: the
 * same streams (PID and codec), nothing still scrambled, nothing without
 * data that the satellite has. A difference seen twice running is a
 * problem; a service that leaves an output is said once.
 *
 * Every poll an Upcom is also asked for its own tables (ird_upcom.h): the
 * PSI of each of its inputs and of its remux. Each service of the remux is
 * compared with the same service from the input it comes from (the same
 * streams; language labels only noted), and each input with what it was a
 * minute before: a service whose streams or labels change on the
 * satellite is said at once.
 *
 * Kinds of IRD understood: "upcom" (Upcom UC-IRD+, /goform/subBoard.cgi) and
 * "mcr" (the Multi-Channel CI-Receiver: /cgi.php, HTTP basic authentication,
 * two tuners; its uptime and temperature too: a restart is said at once).
 * An IRD with no raw output given has each output's probe compared with the
 * one before: a service whose streams change, or that turns scrambled, is
 * said.
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
#define IRD_TUNERS 4

struct ird_conf {
    char name[48];
    char address[128];          /* its web interface: host[:port], or KIND://[user:password@]host[:port] */
    char kind[16];              /* "upcom", "mcr" */
    char raw[256];              /* its raw tuner output (udp://...): "" none */
    char outputs[IRD_OUTS][256]; /* what our channels read from it */
    int n_out;
};

struct ird_opts {
    const char *binary;         /* this program, for the probes */
    int poll_s;                 /* the tuner, every (60) */
    int remux_s;                /* the probe comparison, every (3 h; 0: never) */
    int baseline_n;             /* readings before there is a normal C/N (30) */
    int probe_s;                /* each probe this long (5) */
};

struct ird_state {
    struct ird_conf c;
    bool answering;             /* the last poll had an answer */
    int fails;                  /* polls without one, in a row */
    int64_t last_try, last_ok;
    char error[160];
    int n_tuners;               /* an IRD may have several, each on its own transponder */
    struct ird_tuner {
        bool have;              /* readings below */
        bool locked;
        double cn, ebn0, ber, strength, quality, total_mbps, valid_mbps;     /* NAN: not given */
        double cn_normal;       /* < 0: not known yet */
        int n_samples;
        int low;                /* readings in a row below the normal by the limit */
    } t[IRD_TUNERS];
    /* how the box itself is, when it says (the "mcr" kind) */
    bool have_health;
    int64_t uptime_s;           /* -1: not known */
    double temp_c, temp_normal; /* NAN, -1: not known */
    int temp_high;              /* readings in a row above the limit */
    int64_t restarted_t;        /* when it was last seen to have restarted (0: not) */
    /* the comparison */
    int64_t remux_t;            /* last done (0: never) */
    int remux_bad;              /* comparisons in a row with differences */
    int remux_services;         /* services compared */
    char remux_what[480];       /* the differences, in words ("" none) */
    char remux_notes[480];      /* the same in streams none of our channels take: noted only */
    char remux_error[160];      /* could not compare */
    /* the IRD's own tables (an Upcom's: ird_upcom.h), every poll */
    int64_t tables_t;           /* last read (0: never) */
    int tables_services;        /* services of the remux compared */
    int tables_bad;             /* reads in a row with differences */
    char tables_what[480];      /* how the remux differs from the satellite ("" not) */
    char tables_labels[480];    /* audio language labels that differ (a note only: labels are not to be trusted) */
    char tables_error[160];
};

void ird_set(const struct ird_conf *list, int n, const struct ird_opts *o);
void ird_stop(void);
int ird_get(struct ird_state *out, int max);
/* Whether one of our channels takes PID `pid` of program `program` from
 * `url`: a difference in a stream nobody takes is said as a note, not a
 * problem. Called from the IRD thread. */
void ird_set_uses(bool (*fn)(const char *url, int program, int pid, void *op), void *op);
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
int ird_compare(const char *raw_json, const char *out_json, const char *url, char *what, size_t len, char *notes,
                size_t nlen, int *services);

/* Parses an Upcom tuner answer ({"120": {"0": {...}}}) into st->t[0];
 * false if it is not one. */
bool ird_parse_upcom(const char *body, struct ird_state *st);
/* The Multi-Channel CI-Receiver's: its tuners (proctype=tuner), and its
 * uptime and temperature (proctype=runstatus). */
bool ird_parse_mcr_tuners(const char *body, struct ird_state *st);
bool ird_parse_mcr_status(const char *body, int64_t *uptime_s, double *temp_c);

#endif
