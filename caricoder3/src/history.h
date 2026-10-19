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

/* The disk is nearly full: keep the record to `bytes` for now (the oldest
 * goes, as for the cap); 0: back to the configured cap. */
void history_squeeze(int64_t bytes);
int64_t history_bytes(void);

/* JSON for the API. `chan` NULL: the server's metrics (`metric` a name or a
 * prefix ending in '*'). Resolution is chosen from the span. */
bool history_query_chan(FILE *f, const char *chan, int64_t from, int64_t to);
bool history_query_metrics(FILE *f, const char *metric, int64_t from, int64_t to);
bool history_query_events(FILE *f, const char *chan, int64_t from, int64_t to, int limit);
/* Incidents: a problem from its start to its end. Opening one returns its
 * id (0 when no record is kept); `parent` (0 none) is the source's
 * incident a channel's belongs to. `detail` is a JSON object. An update
 * sets `closed` (0: still open) and `parent`; a NULL summary or detail is
 * left as it was. */
int64_t history_incident_open(int64_t t, const char *chan, const char *rule, const char *level,
                              const char *grp, const char *summary, const char *detail, int64_t parent);
void history_incident_update(int64_t id, int64_t closed, const char *summary, const char *detail, int64_t parent);
/* {"incidents": [...]}: overlapping the span, open ones first; for a
 * channel, its own and the source's it belongs to */
bool history_query_incidents(FILE *f, const char *chan, int64_t from, int64_t to, bool open_only, int limit);
/* Evidence a tool gathered: `args` and `result` JSON objects, `file` a
 * file it kept ("" none); `incident` 0 when it belongs to none. */
int64_t history_evidence_add(int64_t t, int64_t incident, const char *chan, const char *tool, const char *args,
                             const char *result, const char *file);
/* {"evidence": [...]}, newest first; chan NULL / incident 0: any */
bool history_query_evidence(FILE *f, const char *chan, int64_t incident, int limit);
/* A line of the conversation with a peer: `dir` "out" (said here) or "in"
 * (heard), `data` a JSON object. JSON array of the latest, newest first. */
void history_message(int64_t t, const char *peer, const char *dir, const char *text, const char *data,
                     int64_t incident);
bool history_query_messages(FILE *f, int64_t incident, int limit);
/* The technician's answer about incident `id` (a JSON object). */
void history_incident_diagnosis(int64_t id, int64_t t, const char *json);
/* As history_query_incident, briefer: the members without their snapshot,
 * at most 80 events (what the technician is given). */
bool history_report_incident(FILE *f, int64_t id, int64_t now);
/* Up to `limit` earlier incidents like `id` (the same rule on the same
 * channel or source), those a person gave a verdict on first: a JSON array. */
bool history_similar(FILE *f, int64_t id, int limit);
/* The file kept with evidence `id`. */
bool history_evidence_file(int64_t id, char *path, size_t len);
/* {"incident": ..., "members": [...], "evidence": [...], "events": [...]} */
bool history_query_incident(FILE *f, int64_t id, int64_t now);

/* {"path": ..., "bytes": ..., "cap_bytes": ..., "oldest": ...} */
void history_info(FILE *f);

#endif
