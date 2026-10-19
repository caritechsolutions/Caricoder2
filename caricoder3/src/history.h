/*
 * caricoder3 - what happened, kept on disk: channel and server history, events
 * Copyright (c) 2026 CariTech Solutions
 *
 * The supervisor's record (SQLite, one file): every 10 s each channel's
 * rates and error counts and the server's load; summed up per minute and
 * per hour; and an event log (starts, stops, failures, switches, warnings
 * the channels wrote, changes made in the GUI). Detail is kept 3 days,
 * minutes 30 days, hours and events a year, and the file never grows past
 * its cap: the oldest goes first. Writes happen in a thread of its own;
 * reads open the file themselves (WAL: they never wait for the writer).
 */
#ifndef CC3_HISTORY_H
#define CC3_HISTORY_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* One channel's 10 s: rates averaged over it, counters as increments. */
struct hist_chan {
    char name[64];
    int state;                  /* 0 stopped, 1 running, 2 failed */
    double in_bps, out_bps;
    int64_t cc, overflow, dropped, late, lost, switches, restarts;
    int health;                 /* 0 fine, 1 trouble, 2 failed */
};

/* Turns the record on: `path` the file, `max_mb` its cap. False (and the
 * record stays off) when the file cannot be opened or was built without. */
bool history_open(const char *path, int max_mb, char *err, size_t errlen);
void history_close(void);
bool history_enabled(void);

/* Queued, written by the thread. `t` is the wall clock (seconds). */
void history_chan(int64_t t, const struct hist_chan *h);
void history_metric(int64_t t, const char *name, double v);
/* level: "info", "warn", "error"; chan may be NULL (the server) */
void history_event(int64_t t, const char *chan, const char *level, const char *kind,
                   const char *text);

/* Rows were written for the past (a test, an import): minutes and hours
 * are made again from `since` on, next time the writer looks. */
void history_resummarise(int64_t since);

/* JSON for the API. `chan` NULL: the server's metrics (`metric` a name or a
 * prefix ending in '*'). Resolution is chosen from the span. */
bool history_query_chan(FILE *f, const char *chan, int64_t from, int64_t to);
bool history_query_metrics(FILE *f, const char *metric, int64_t from, int64_t to);
bool history_query_events(FILE *f, const char *chan, int64_t from, int64_t to, int limit);
/* {"path": ..., "bytes": ..., "cap_bytes": ..., "oldest": ...} */
void history_info(FILE *f);

#endif
