/*
 * caricoder3 - the watchdog: rules over what the channels and the server
 * report, and the incidents they open
 * Copyright (c) 2026 CariTech Solutions
 *
 * The supervisor tells it, every couple of seconds, which conditions hold
 * (no input, CC errors above the limit, a black picture, ...) for each
 * channel and for the server. A condition that lasts `min_s` opens an
 * incident (kept in the history, history.h); it closes when the condition
 * has been gone for `clear_s`, dated when it was last seen. Shorter ones
 * stay plain events. When channels that read the same source have the
 * same upstream problem at once, one incident for the source is opened
 * as well, and theirs belong to it: the clue that the cause is before
 * the transcoder.
 *
 * Some are kept as records instead (history_record), for the weekly
 * summary and as context, and never asked about: CC errors (they come and
 * go; what they damage is an incident of its own, which names them as its
 * cause), and dropped pictures the source caused that are over within
 * `short_s`.
 */
#ifndef CC3_WATCHDOG_H
#define CC3_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

enum wd_rule {
    WD_NO_INPUT, WD_CC, WD_LOSS, WD_DROPPING, WD_OUT_STALLED, WD_RESTARTS, WD_FAILED,
    WD_BACKUP, WD_CLOCK, WD_BLACK, WD_FROZEN, WD_SILENT, WD_NO_DATA, WD_PIDS,
    WD_CPU, WD_GPU, WD_NIC_DROP, WD_DISK_LOW, WD_DISK_CRITICAL, WD_DISK_FILLING, WD_AV_SHIFT,
    WD_CARD_OWN, WD_CARD_ENDED, WD_MEM_GROWING,
    WD_IRD_DOWN, WD_IRD_UNLOCKED, WD_IRD_CN, WD_REMUX, WD_SAT_CHANGED, WD_LANGUAGE, WD_IRD_RESTART, WD_IRD_HOT,
    WD_RULES
};

struct wd_limits {
    int min_s;              /* a problem this long is an incident */
    int content_s;          /* ... a black or frozen picture, silent audio: this long (programmes have short ones) */
    int clear_s;            /* gone this long: the incident is over */
    int group_s;            /* channels of one source within this of each other: one source incident */
    int no_input_s;
    int cc_per_min;
    int dropped_per_min;
    int restarts;           /* this many restarts ... */
    int restart_window_s;   /* ... in this long */
    int cpu_pct, gpu_pct;   /* load above this ... */
    int load_s;             /* ... for this long */
    int disk_warn_pct;      /* free space below this: low */
    int disk_crit_pct;      /* ... below this: critical (the history makes room) */
    int disk_full_h;        /* full within this many hours at the last hour's rate */
    int av_shift_ms;        /* the source's A/V timing moved this far from its baseline ... */
    int av_shift_s;         /* ... for this long */
    int mem_window_s;       /* a process's memory going up all through this long ... */
    int mem_growth_mb;      /* ... by at least this much ... */
    int mem_growth_pct;     /* ... and this share of where it was: a leak */
    int warmup_s;           /* a channel's first this long after it starts: dropped pictures,
                             * streams without data and CC errors are not judged */
    int ird_cn_drop_db;     /* an IRD's C/N this far below its normal: low */
    int ird_temp_max_c;     /* an IRD hotter than this: too hot (0: judged by its normal instead) */
    int ird_temp_rise_c;    /* ... this much above its normal: too hot */
    int merge_s;            /* the same problem again on the same channel (or source) within this long of
                             * the last one ending: that incident again, not a new one (0: never) */
    int short_s;            /* dropped pictures the source caused (CC errors, no data, no input within a
                             * minute), over within this long: a record, not an incident */
    bool off[WD_RULES];     /* rules turned off */
};

void wd_defaults(struct wd_limits *l);
const char *wd_rule_id(enum wd_rule r);         /* "no_input", ... */
const char *wd_rule_name(enum wd_rule r);       /* "no input", ... */
int wd_rule_by_id(const char *id);              /* -1: none */

void wd_set_limits(const struct wd_limits *l);
/* The limits as a JSON object, and back (keys missing: left as they are). */
struct json_doc;
void wd_limits_write(FILE *f, const struct wd_limits *l);
void wd_limits_read(const struct json_doc *d, int obj, struct wd_limits *l);
void wd_get_limits(struct wd_limits *l);

/* What a new incident should carry as evidence: a JSON object written to
 * `f` for `chan` (NULL: the server). Called while opening one. */
void wd_set_snapshot(void (*fn)(FILE *f, const char *chan, void *op), void *op);

/* Told when an incident opens (`chan` NULL: the server's or a source's,
 * `group` the source), so evidence can be gathered while it lasts. Called
 * from wd_tick; it must not call back into the watchdog. */
void wd_set_on_open(void (*fn)(int64_t id, const char *chan, const char *group, enum wd_rule r, void *op), void *op);

/*
 * Whether rule `r` holds for `subject` (a channel's name, or "server",
 * "server/gpu0", ...) now (`t`, wall clock seconds). `chan` is the channel
 * (NULL for the server), `group` its source (for grouping; "" none),
 * `what` a few words for the summary ("audio ENG silent"), `began` when
 * it began if known before now (0: now).
 */
void wd_observe(int64_t t, const char *subject, const char *chan, const char *group, enum wd_rule r,
                bool on, const char *what, int64_t began);

/* Something that happens at one moment (the source's PIDs changed): an
 * incident that opens and closes at `t`. */
void wd_instant(int64_t t, const char *chan, const char *group, enum wd_rule r, const char *what);
/* ... of the server, not of a channel */
void wd_instant_server(int64_t t, const char *subject, enum wd_rule r, const char *what);

/* The channel stopped or went: what it had open closes now. */
void wd_forget(int64_t t, const char *subject);

/* Opens, closes and groups. Every couple of seconds. */
void wd_tick(int64_t t);

/* Incidents open now (a source's counts once, not its channels). */
int wd_open_count(void);

/* Problems being watched now: {"open": n, "pending": n, "items": [...]} */
void wd_state_json(FILE *f, int64_t t);

#endif
