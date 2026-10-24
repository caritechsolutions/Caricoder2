/*
 * caricoder3 - what happened, kept on disk (see history.h)
 * Copyright (c) 2026 CariTech Solutions
 */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "history.h"
#include "log.h"

#ifndef HAVE_SQLITE

bool history_open(const char *path, int max_mb, char *err, size_t errlen)
{
    (void)path; (void)max_mb;
    snprintf(err, errlen, "this build has no SQLite: no history is kept");
    return false;
}
void history_close(void) {}
bool history_enabled(void) { return false; }
void history_chan(int64_t t, const struct hist_chan *h) { (void)t; (void)h; }
void history_metric(int64_t t, const char *name, double v) { (void)t; (void)name; (void)v; }
void history_event(int64_t t, const char *chan, const char *level, const char *kind, const char *text)
{
    (void)t; (void)chan; (void)level; (void)kind; (void)text;
}
bool history_query_chan(FILE *f, const char *chan, int64_t from, int64_t to)
{
    (void)f; (void)chan; (void)from; (void)to;
    return false;
}
bool history_query_metrics(FILE *f, const char *metric, int64_t from, int64_t to)
{
    (void)f; (void)metric; (void)from; (void)to;
    return false;
}
bool history_query_events(FILE *f, const char *chan, int64_t from, int64_t to, int limit)
{
    (void)f; (void)chan; (void)from; (void)to; (void)limit;
    return false;
}
void history_info(FILE *f) { fprintf(f, "null"); }
void history_record(int64_t t, int64_t closed, const char *chan, const char *rule, const char *grp,
                    const char *summary, const char *cause)
{
    (void)t; (void)closed; (void)chan; (void)rule; (void)grp; (void)summary; (void)cause;
}
bool history_query_records(FILE *f, const char *chan, int64_t from, int64_t to, int limit)
{
    (void)chan; (void)from; (void)to; (void)limit;
    fprintf(f, "{\"records\": []}");
    return false;
}
bool history_summary(FILE *f, int64_t from, int64_t to)
{
    (void)from; (void)to;
    fprintf(f, "null");
    return false;
}
int64_t history_incident_open(int64_t t, const char *chan, const char *rule, const char *level,
                              const char *grp, const char *summary, const char *detail, int64_t parent)
{
    (void)t; (void)chan; (void)rule; (void)level; (void)grp; (void)summary; (void)detail; (void)parent;
    return 0;
}
void history_incident_update(int64_t id, int64_t closed, const char *summary, const char *detail, int64_t parent)
{
    (void)id; (void)closed; (void)summary; (void)detail; (void)parent;
}
bool history_query_incidents(FILE *f, const char *chan, int64_t from, int64_t to, bool open_only, int limit)
{
    (void)f; (void)chan; (void)from; (void)to; (void)open_only; (void)limit;
    return false;
}
bool history_query_incident(FILE *f, int64_t id, int64_t now)
{
    (void)f; (void)id; (void)now;
    return false;
}
void history_resummarise(int64_t since) { (void)since; }
void history_squeeze(int64_t bytes) { (void)bytes; }
int64_t history_evidence_add(int64_t t, int64_t incident, const char *chan, const char *tool, const char *args,
                             const char *result, const char *file)
{
    (void)t; (void)incident; (void)chan; (void)tool; (void)args; (void)result; (void)file;
    return 0;
}
bool history_query_evidence(FILE *f, const char *chan, int64_t incident, int limit)
{
    (void)f; (void)chan; (void)incident; (void)limit;
    return false;
}
bool history_evidence_file(int64_t id, char *path, size_t len)
{
    (void)id; (void)path; (void)len;
    return false;
}
void history_message(int64_t t, const char *peer, const char *dir, const char *text, const char *data,
                     int64_t incident)
{
    (void)t; (void)peer; (void)dir; (void)text; (void)data; (void)incident;
}
bool history_query_messages(FILE *f, int64_t incident, int limit)
{
    (void)incident; (void)limit;
    fprintf(f, "[]");
    return true;
}
void history_incident_diagnosis(int64_t id, int64_t t, const char *json) { (void)id; (void)t; (void)json; }
void history_incident_verdict(int64_t id, int64_t t, const char *verdict, const char *note, const char *actual)
{
    (void)id; (void)t; (void)verdict; (void)note; (void)actual;
}
int history_verdict_ids(int64_t *ids, int max) { (void)ids; (void)max; return 0; }
int history_eval_cases(struct hist_case *out, int max) { (void)out; (void)max; return 0; }
bool history_open_readonly(const char *path, char *err, size_t errlen)
{
    (void)path;
    snprintf(err, errlen, "this build has no SQLite: no history is kept");
    return false;
}
bool history_query_incidents_brief(FILE *f, int64_t from, int64_t to, bool open_only, int limit)
{
    (void)from; (void)to; (void)open_only; (void)limit;
    fprintf(f, "{\"incidents\": []}");
    return true;
}
bool history_incident_answer(int64_t id, char **diagnosis, char **verdict, char **note, char **actual)
{
    (void)id;
    *diagnosis = *verdict = *note = *actual = NULL;
    return false;
}
void history_verdict_counts(FILE *f) { fprintf(f, "null"); }
bool history_report_incident(FILE *f, int64_t id, int64_t now) { (void)f; (void)id; (void)now; return false; }
bool history_similar(FILE *f, int64_t id, int limit) { (void)id; (void)limit; fprintf(f, "[]"); return true; }
int64_t history_bytes(void) { return 0; }

#else

#include <sqlite3.h>

#define KEEP_S10      (3 * 86400)
#define KEEP_S60      (30 * 86400)
#define KEEP_S3600    (366 * 86400)
#define KEEP_EVENTS   (366 * 86400)
#define FLUSH_S       2
#define TIDY_S        600             /* retention and the cap, this often */
#define NAMES_MAX     1024

enum rec_type { R_CHAN, R_METRIC, R_EVENT, R_INC_OPEN, R_INC_UPDATE, R_EVIDENCE, R_MESSAGE, R_INC_DIAG, R_INC_VERDICT,
                R_RECORD };

struct rec {
    enum rec_type type;
    int64_t t;
    struct hist_chan c;             /* R_CHAN */
    char name[96];                  /* R_METRIC: metric; R_EVENT: channel ("" = server) */
    double v;
    char level[8], kind[32];
    char *text;                     /* R_EVENT; R_INC_*: the summary. malloc'd */
    /* incidents: name = channel, kind = rule, text = summary */
    int64_t id, parent, closed;
    char *detail, *grp;             /* malloc'd */
};

static struct {
    bool on;
    char path[512];
    int64_t cap;
    sqlite3 *db;                    /* the writer's */
    pthread_t th;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool stop;
    struct rec *q;
    int qn, qcap;
    int dropped;
    int64_t resum_since;            /* > 0: history_resummarise() asked */
    /* name -> id caches, writer thread only */
    char chan_names[NAMES_MAX][64];
    int chan_ids[NAMES_MAX], n_chan;
    char met_names[NAMES_MAX][96];
    int met_ids[NAMES_MAX], n_met;
    int64_t next_incident, next_evidence;
    int64_t cap_cfg;                /* as configured; cap is lower while squeezed */
} H = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

static const char *const SCHEMA =
    "PRAGMA journal_mode=WAL;"
    "CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v INTEGER);"
    "CREATE TABLE IF NOT EXISTS chans (id INTEGER PRIMARY KEY, name TEXT UNIQUE);"
    "CREATE TABLE IF NOT EXISTS metrics (id INTEGER PRIMARY KEY, name TEXT UNIQUE);"
    /* every 10 s: state, health, kb/s in and out, increments of the counters */
    "CREATE TABLE IF NOT EXISTS s10 (t INTEGER, cid INTEGER, st INTEGER, hl INTEGER, inb INTEGER, outb INTEGER,"
    " cc INTEGER, ovf INTEGER, drp INTEGER, late INTEGER, lost INTEGER, sw INTEGER, rs INTEGER, PRIMARY KEY (t, cid))"
    " WITHOUT ROWID;"
    "CREATE INDEX IF NOT EXISTS s10_c ON s10 (cid, t);"
    /* per minute and per hour: seconds running, worst health, rates average/min/max, counters summed */
    "CREATE TABLE IF NOT EXISTS s60 (t INTEGER, cid INTEGER, up INTEGER, hl INTEGER, in_avg INTEGER, in_min INTEGER,"
    " in_max INTEGER, out_avg INTEGER, out_min INTEGER, out_max INTEGER, cc INTEGER, ovf INTEGER, drp INTEGER,"
    " late INTEGER, lost INTEGER, sw INTEGER, rs INTEGER, PRIMARY KEY (t, cid)) WITHOUT ROWID;"
    "CREATE INDEX IF NOT EXISTS s60_c ON s60 (cid, t);"
    "CREATE TABLE IF NOT EXISTS s3600 (t INTEGER, cid INTEGER, up INTEGER, hl INTEGER, in_avg INTEGER, in_min INTEGER,"
    " in_max INTEGER, out_avg INTEGER, out_min INTEGER, out_max INTEGER, cc INTEGER, ovf INTEGER, drp INTEGER,"
    " late INTEGER, lost INTEGER, sw INTEGER, rs INTEGER, PRIMARY KEY (t, cid)) WITHOUT ROWID;"
    "CREATE INDEX IF NOT EXISTS s3600_c ON s3600 (cid, t);"
    /* the server: one value per metric */
    "CREATE TABLE IF NOT EXISTS m10 (t INTEGER, mid INTEGER, v REAL, PRIMARY KEY (t, mid)) WITHOUT ROWID;"
    "CREATE INDEX IF NOT EXISTS m10_m ON m10 (mid, t);"
    "CREATE TABLE IF NOT EXISTS m60 (t INTEGER, mid INTEGER, v REAL, vmin REAL, vmax REAL, PRIMARY KEY (t, mid))"
    " WITHOUT ROWID;"
    "CREATE INDEX IF NOT EXISTS m60_m ON m60 (mid, t);"
    "CREATE TABLE IF NOT EXISTS m3600 (t INTEGER, mid INTEGER, v REAL, vmin REAL, vmax REAL, PRIMARY KEY (t, mid))"
    " WITHOUT ROWID;"
    "CREATE INDEX IF NOT EXISTS m3600_m ON m3600 (mid, t);"
    "CREATE TABLE IF NOT EXISTS events (id INTEGER PRIMARY KEY, t INTEGER, cid INTEGER, level TEXT, kind TEXT,"
    " text TEXT);"
    "CREATE INDEX IF NOT EXISTS events_t ON events (t);"
    "CREATE INDEX IF NOT EXISTS events_c ON events (cid, t);"
    /* problems, from when they began (t) to when they ended (closed; NULL while open);
     * a channel's may belong to one of the source's (parent); the verdict is a person's */
    "CREATE TABLE IF NOT EXISTS incidents (id INTEGER PRIMARY KEY, t INTEGER, closed INTEGER, chan TEXT,"
    " rule TEXT, level TEXT, grp TEXT, summary TEXT, detail TEXT, parent INTEGER, verdict TEXT, note TEXT,"
    " verdict_t INTEGER);"
    "CREATE INDEX IF NOT EXISTS incidents_t ON incidents (t);"
    "CREATE INDEX IF NOT EXISTS incidents_c ON incidents (chan, t);"
    "CREATE INDEX IF NOT EXISTS incidents_p ON incidents (parent);"
    /* what the tools found or kept: a probe's JSON, a recording, a picture */
    "CREATE TABLE IF NOT EXISTS evidence (id INTEGER PRIMARY KEY, t INTEGER, incident INTEGER, chan TEXT, tool TEXT,"
    " args TEXT, result TEXT, file TEXT);"
    "CREATE INDEX IF NOT EXISTS evidence_i ON evidence (incident);"
    "CREATE INDEX IF NOT EXISTS evidence_t ON evidence (t);"
    /* what this server and its peers said to each other ("out": said here, "in": heard) */
    "CREATE TABLE IF NOT EXISTS messages (id INTEGER PRIMARY KEY, t INTEGER, peer TEXT, dir TEXT, text TEXT,"
    " data TEXT, incident INTEGER);"
    "CREATE INDEX IF NOT EXISTS messages_t ON messages (t);"
    /* what came and went without an effect worth a person's time (CC errors,
     * short damage caused at the source): for the weekly summary and the
     * technician, not for a verdict */
    "CREATE TABLE IF NOT EXISTS records (id INTEGER PRIMARY KEY, t INTEGER, closed INTEGER, chan TEXT, rule TEXT,"
    " grp TEXT, summary TEXT, cause TEXT);"
    "CREATE INDEX IF NOT EXISTS records_t ON records (t);"
    "CREATE INDEX IF NOT EXISTS records_c ON records (chan, t);";

static bool exec(sqlite3 *db, const char *sql)
{
    char *e = NULL;

    if (sqlite3_exec(db, sql, NULL, NULL, &e) != SQLITE_OK) {
        log_err("history: %s (%.80s)", e ? e : "?", sql);
        sqlite3_free(e);
        return false;
    }
    return true;
}

static int64_t meta_get(const char *k)
{
    sqlite3_stmt *st;
    int64_t v = 0;

    if (sqlite3_prepare_v2(H.db, "SELECT v FROM meta WHERE k = ?", -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, k, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

static void meta_set(const char *k, int64_t v)
{
    sqlite3_stmt *st;

    if (sqlite3_prepare_v2(H.db, "INSERT OR REPLACE INTO meta (k, v) VALUES (?, ?)", -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(st, 1, k, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, v);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* The id of a name in `table` (chans or metrics), made when new. */
static int name_id(const char *table, const char *name, char (*names)[96], int *ids, int *n, size_t width)
{
    sqlite3_stmt *st;
    char sql[128];
    int id = -1;

    for (int i = 0; i < *n; i++)
        if (!strcmp((const char *)names + (size_t)i * width, name))
            return ids[i];
    snprintf(sql, sizeof(sql), "INSERT OR IGNORE INTO %s (name) VALUES (?)", table);
    if (sqlite3_prepare_v2(H.db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    snprintf(sql, sizeof(sql), "SELECT id FROM %s WHERE name = ?", table);
    if (sqlite3_prepare_v2(H.db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW)
            id = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    if (id >= 0 && *n < NAMES_MAX) {
        snprintf((char *)names + (size_t)*n * width, width, "%s", name);
        ids[(*n)++] = id;
    }
    return id;
}

static int chan_id(const char *name)
{
    return name_id("chans", name, (char (*)[96])H.chan_names, H.chan_ids, &H.n_chan, sizeof(H.chan_names[0]));
}

static int metric_id(const char *name)
{
    return name_id("metrics", name, H.met_names, H.met_ids, &H.n_met, sizeof(H.met_names[0]));
}

static void write_batch(struct rec *q, int n)
{
    sqlite3_stmt *sc = NULL, *sm = NULL, *se = NULL, *si = NULL, *su = NULL, *sv = NULL, *sg = NULL, *sd = NULL;
    sqlite3_stmt *sx = NULL, *sr = NULL;

    exec(H.db, "BEGIN");
    sqlite3_prepare_v2(H.db, "INSERT INTO records (t, closed, chan, rule, grp, summary, cause) VALUES (?,?,?,?,?,?,?)",
                       -1, &sr, NULL);
    sqlite3_prepare_v2(H.db, "INSERT OR REPLACE INTO s10 VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)", -1, &sc, NULL);
    sqlite3_prepare_v2(H.db, "INSERT OR REPLACE INTO m10 VALUES (?,?,?)", -1, &sm, NULL);
    sqlite3_prepare_v2(H.db, "INSERT INTO events (t, cid, level, kind, text) VALUES (?,?,?,?,?)", -1, &se, NULL);
    sqlite3_prepare_v2(H.db, "INSERT OR REPLACE INTO incidents (id, t, closed, chan, rule, level, grp, summary, detail,"
                       " parent) VALUES (?,?,?,?,?,?,?,?,?,?)", -1, &si, NULL);
    sqlite3_prepare_v2(H.db, "INSERT INTO messages (t, peer, dir, text, data, incident) VALUES (?,?,?,?,?,?)", -1, &sg,
                       NULL);
    sqlite3_prepare_v2(H.db, "UPDATE incidents SET diagnosis = ?, diag_t = ? WHERE id = ?", -1, &sd, NULL);
    sqlite3_prepare_v2(H.db, "UPDATE incidents SET verdict = ?, note = ?, actual = ?, verdict_t = ? WHERE id = ?", -1,
                       &sx, NULL);
    sqlite3_prepare_v2(H.db, "INSERT OR REPLACE INTO evidence (id, t, incident, chan, tool, args, result, file)"
                       " VALUES (?,?,?,?,?,?,?,?)", -1, &sv, NULL);
    sqlite3_prepare_v2(H.db, "UPDATE incidents SET closed = ?, summary = COALESCE(?, summary),"
                       " detail = COALESCE(?, detail), parent = ? WHERE id = ?", -1, &su, NULL);
    for (int i = 0; i < n; i++) {
        struct rec *r = &q[i];

        if (r->type == R_CHAN && sc) {
            int cid = chan_id(r->c.name);
            int64_t v[] = { r->t, cid, r->c.state, r->c.health, (int64_t)(r->c.in_bps / 1000 + 0.5),
                            (int64_t)(r->c.out_bps / 1000 + 0.5), r->c.cc, r->c.overflow, r->c.dropped,
                            r->c.late, r->c.lost, r->c.switches, r->c.restarts };

            if (cid < 0)
                continue;
            for (int k = 0; k < 13; k++)
                sqlite3_bind_int64(sc, k + 1, v[k]);
            sqlite3_step(sc);
            sqlite3_reset(sc);
        } else if (r->type == R_METRIC && sm) {
            int mid = metric_id(r->name);

            if (mid < 0)
                continue;
            sqlite3_bind_int64(sm, 1, r->t);
            sqlite3_bind_int(sm, 2, mid);
            sqlite3_bind_double(sm, 3, r->v);
            sqlite3_step(sm);
            sqlite3_reset(sm);
        } else if (r->type == R_EVENT && se) {
            sqlite3_bind_int64(se, 1, r->t);
            if (r->name[0])
                sqlite3_bind_int(se, 2, chan_id(r->name));
            else
                sqlite3_bind_null(se, 2);
            sqlite3_bind_text(se, 3, r->level, -1, SQLITE_STATIC);
            sqlite3_bind_text(se, 4, r->kind, -1, SQLITE_STATIC);
            sqlite3_bind_text(se, 5, r->text ? r->text : "", -1, SQLITE_STATIC);
            sqlite3_step(se);
            sqlite3_reset(se);
        } else if (r->type == R_INC_OPEN && si) {
            sqlite3_bind_int64(si, 1, r->id);
            sqlite3_bind_int64(si, 2, r->t);
            if (r->closed)
                sqlite3_bind_int64(si, 3, r->closed);
            else
                sqlite3_bind_null(si, 3);
            if (r->name[0])
                sqlite3_bind_text(si, 4, r->name, -1, SQLITE_STATIC);
            else
                sqlite3_bind_null(si, 4);
            sqlite3_bind_text(si, 5, r->kind, -1, SQLITE_STATIC);
            sqlite3_bind_text(si, 6, r->level, -1, SQLITE_STATIC);
            sqlite3_bind_text(si, 7, r->grp ? r->grp : "", -1, SQLITE_STATIC);
            sqlite3_bind_text(si, 8, r->text ? r->text : "", -1, SQLITE_STATIC);
            sqlite3_bind_text(si, 9, r->detail ? r->detail : "{}", -1, SQLITE_STATIC);
            if (r->parent)
                sqlite3_bind_int64(si, 10, r->parent);
            else
                sqlite3_bind_null(si, 10);
            sqlite3_step(si);
            sqlite3_reset(si);
        } else if (r->type == R_INC_VERDICT && sx) {
            if (!strcmp(r->kind, "none"))      /* the verdict taken back */
                sqlite3_bind_null(sx, 1);
            else
                sqlite3_bind_text(sx, 1, r->kind, -1, SQLITE_STATIC);
            sqlite3_bind_text(sx, 2, r->text ? r->text : "", -1, SQLITE_STATIC);
            sqlite3_bind_text(sx, 3, r->detail ? r->detail : "{}", -1, SQLITE_STATIC);
            sqlite3_bind_int64(sx, 4, r->t);
            sqlite3_bind_int64(sx, 5, r->id);
            sqlite3_step(sx);
            sqlite3_reset(sx);
        } else if (r->type == R_INC_DIAG && sd) {
            sqlite3_bind_text(sd, 1, r->detail ? r->detail : "{}", -1, SQLITE_STATIC);
            sqlite3_bind_int64(sd, 2, r->t);
            sqlite3_bind_int64(sd, 3, r->id);
            sqlite3_step(sd);
            sqlite3_reset(sd);
        } else if (r->type == R_MESSAGE && sg) {
            sqlite3_bind_int64(sg, 1, r->t);
            sqlite3_bind_text(sg, 2, r->name, -1, SQLITE_STATIC);
            sqlite3_bind_text(sg, 3, r->kind, -1, SQLITE_STATIC);
            sqlite3_bind_text(sg, 4, r->text ? r->text : "", -1, SQLITE_STATIC);
            sqlite3_bind_text(sg, 5, r->detail ? r->detail : "{}", -1, SQLITE_STATIC);
            if (r->parent)
                sqlite3_bind_int64(sg, 6, r->parent);
            else
                sqlite3_bind_null(sg, 6);
            sqlite3_step(sg);
            sqlite3_reset(sg);
        } else if (r->type == R_EVIDENCE && sv) {
            sqlite3_bind_int64(sv, 1, r->id);
            sqlite3_bind_int64(sv, 2, r->t);
            if (r->parent)
                sqlite3_bind_int64(sv, 3, r->parent);
            else
                sqlite3_bind_null(sv, 3);
            if (r->name[0])
                sqlite3_bind_text(sv, 4, r->name, -1, SQLITE_STATIC);
            else
                sqlite3_bind_null(sv, 4);
            sqlite3_bind_text(sv, 5, r->kind, -1, SQLITE_STATIC);
            sqlite3_bind_text(sv, 6, r->grp ? r->grp : "{}", -1, SQLITE_STATIC);
            sqlite3_bind_text(sv, 7, r->detail ? r->detail : "{}", -1, SQLITE_STATIC);
            sqlite3_bind_text(sv, 8, r->text ? r->text : "", -1, SQLITE_STATIC);
            sqlite3_step(sv);
            sqlite3_reset(sv);
        } else if (r->type == R_RECORD && sr) {
            sqlite3_bind_int64(sr, 1, r->t);
            sqlite3_bind_int64(sr, 2, r->closed);
            if (r->name[0])
                sqlite3_bind_text(sr, 3, r->name, -1, SQLITE_STATIC);
            else
                sqlite3_bind_null(sr, 3);
            sqlite3_bind_text(sr, 4, r->kind, -1, SQLITE_STATIC);
            sqlite3_bind_text(sr, 5, r->grp ? r->grp : "", -1, SQLITE_STATIC);
            sqlite3_bind_text(sr, 6, r->text ? r->text : "", -1, SQLITE_STATIC);
            sqlite3_bind_text(sr, 7, r->detail ? r->detail : "", -1, SQLITE_STATIC);
            sqlite3_step(sr);
            sqlite3_reset(sr);
        } else if (r->type == R_INC_UPDATE && su) {
            if (r->closed)
                sqlite3_bind_int64(su, 1, r->closed);
            else
                sqlite3_bind_null(su, 1);
            if (r->text)
                sqlite3_bind_text(su, 2, r->text, -1, SQLITE_STATIC);
            else
                sqlite3_bind_null(su, 2);
            if (r->detail)
                sqlite3_bind_text(su, 3, r->detail, -1, SQLITE_STATIC);
            else
                sqlite3_bind_null(su, 3);
            if (r->parent)
                sqlite3_bind_int64(su, 4, r->parent);
            else
                sqlite3_bind_null(su, 4);
            sqlite3_bind_int64(su, 5, r->id);
            sqlite3_step(su);
            sqlite3_reset(su);
        }
    }
    sqlite3_finalize(sc);
    sqlite3_finalize(sm);
    sqlite3_finalize(se);
    sqlite3_finalize(si);
    sqlite3_finalize(su);
    sqlite3_finalize(sv);
    sqlite3_finalize(sg);
    sqlite3_finalize(sd);
    sqlite3_finalize(sx);
    sqlite3_finalize(sr);
    exec(H.db, "COMMIT");
}

/* Minutes from the 10 s rows, hours from the minutes: every period that
 * is over (with a margin for rows still on their way). */
static void summarise(int64_t now)
{
    int64_t done60 = meta_get("done60"), done3600 = meta_get("done3600");
    int64_t upto60 = (now - 30) / 60 * 60, upto3600 = (now - 120) / 3600 * 3600;
    sqlite3_stmt *st;
    static const char *const chan_sql[] = {
        "INSERT OR REPLACE INTO s60 SELECT t / 60 * 60, cid, SUM(CASE WHEN st = 1 THEN 10 ELSE 0 END), MAX(hl),"
        " AVG(inb), MIN(inb), MAX(inb), AVG(outb), MIN(outb), MAX(outb), SUM(cc), SUM(ovf), SUM(drp), SUM(late),"
        " SUM(lost), SUM(sw), SUM(rs) FROM s10 WHERE t >= ? AND t < ? GROUP BY t / 60, cid",
        "INSERT OR REPLACE INTO s3600 SELECT t / 3600 * 3600, cid, SUM(up), MAX(hl), AVG(in_avg), MIN(in_min),"
        " MAX(in_max), AVG(out_avg), MIN(out_min), MAX(out_max), SUM(cc), SUM(ovf), SUM(drp), SUM(late), SUM(lost),"
        " SUM(sw), SUM(rs) FROM s60 WHERE t >= ? AND t < ? GROUP BY t / 3600, cid",
    };
    static const char *const met_sql[] = {
        "INSERT OR REPLACE INTO m60 SELECT t / 60 * 60, mid, AVG(v), MIN(v), MAX(v) FROM m10"
        " WHERE t >= ? AND t < ? GROUP BY t / 60, mid",
        "INSERT OR REPLACE INTO m3600 SELECT t / 3600 * 3600, mid, AVG(v), MIN(vmin), MAX(vmax) FROM m60"
        " WHERE t >= ? AND t < ? GROUP BY t / 3600, mid",
    };

    for (int lvl = 0; lvl < 2; lvl++) {
        int64_t from = lvl ? done3600 : done60, to = lvl ? upto3600 : upto60;

        if (to <= from)
            continue;
        if (from == 0)
            from = to - (lvl ? 7 * 86400 : KEEP_S10);   /* a new file: what there is */
        exec(H.db, "BEGIN");
        for (int k = 0; k < 2; k++) {
            if (sqlite3_prepare_v2(H.db, k ? met_sql[lvl] : chan_sql[lvl], -1, &st, NULL) != SQLITE_OK)
                continue;
            sqlite3_bind_int64(st, 1, from);
            sqlite3_bind_int64(st, 2, to);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
        meta_set(lvl ? "done3600" : "done60", to);
        exec(H.db, "COMMIT");
    }
}

static int64_t db_bytes(void)
{
    struct stat sb;
    char wal[600];
    int64_t n = 0;

    if (stat(H.path, &sb) == 0)
        n += sb.st_size;
    snprintf(wal, sizeof(wal), "%s-wal", H.path);
    if (stat(wal, &sb) == 0)
        n += sb.st_size;
    return n;
}

static void delete_before(const char *table, int64_t t)
{
    sqlite3_stmt *st;
    char sql[96];

    snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE t < ?", table);
    if (sqlite3_prepare_v2(H.db, sql, -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(st, 1, t);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

static int64_t oldest(const char *table)
{
    sqlite3_stmt *st;
    char sql[64];
    int64_t t = 0;

    snprintf(sql, sizeof(sql), "SELECT MIN(t) FROM %s", table);
    if (sqlite3_prepare_v2(H.db, sql, -1, &st, NULL) != SQLITE_OK)
        return 0;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL)
        t = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return t;
}

/* Age limits, then the cap: the oldest detail goes first, then the oldest
 * minutes, then hours, the events last (they are small and say most), a
 * tenth of what is left at a time. */
static void tidy(int64_t now)
{
    static const char *const order[] = { "s10", "m10", "s60", "m60", "s3600", "m3600", "records", "events", "messages",
                                         "incidents", "evidence" };
    int64_t before;

    exec(H.db, "PRAGMA wal_checkpoint(TRUNCATE)");  /* the log folded in: the size is the file's */
    before = db_bytes();

    delete_before("s10", now - KEEP_S10);
    delete_before("m10", now - KEEP_S10);
    delete_before("s60", now - KEEP_S60);
    delete_before("m60", now - KEEP_S60);
    delete_before("s3600", now - KEEP_S3600);
    delete_before("m3600", now - KEEP_S3600);
    delete_before("events", now - KEEP_EVENTS);
    delete_before("incidents", now - KEEP_EVENTS);
    delete_before("records", now - KEEP_EVENTS);
    delete_before("evidence", now - KEEP_EVENTS);
    delete_before("messages", now - KEEP_EVENTS);
    for (int round = 0; round < 40 && db_bytes() > H.cap * 9 / 10; round++) {
        bool did = false;

        for (size_t i = 0; i < sizeof(order) / sizeof(order[0]) && !did; i++) {
            int64_t t0 = oldest(order[i]);

            /* the finer tables give way first, down to their last hour;
             * hours and events only when nothing finer is left to give */
            if (t0 && (t0 < now - 3600 || i >= 4)) {
                delete_before(order[i], t0 + (now - t0) / 10 + 1);
                did = true;
            }
        }
        if (!did)
            break;
        exec(H.db, "PRAGMA wal_checkpoint(TRUNCATE)");
        exec(H.db, "PRAGMA incremental_vacuum");
    }
    exec(H.db, "PRAGMA wal_checkpoint(TRUNCATE)");
    exec(H.db, "PRAGMA incremental_vacuum");
    if (before > H.cap)
        log_warn("history: %.0f MB was over the cap of %.0f MB; the oldest records went, now %.0f MB",
                 (double)before / 1048576, (double)H.cap / 1048576, (double)db_bytes() / 1048576);
}

static void *writer(void *arg)
{
    int64_t last_tidy = 0, last_sum = 0;

    (void)arg;
    for (;;) {
        struct rec *q;
        int n;
        bool stop;
        struct timespec ts;

        pthread_mutex_lock(&H.lock);
        if (!H.stop && H.qn == 0) {
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += FLUSH_S;
            pthread_cond_timedwait(&H.cond, &H.lock, &ts);
        }
        q = H.q;
        n = H.qn;
        H.q = NULL;
        H.qn = H.qcap = 0;
        stop = H.stop;
        pthread_mutex_unlock(&H.lock);

        if (n)
            write_batch(q, n);
        for (int i = 0; i < n; i++) {
            free(q[i].text);
            free(q[i].detail);
            free(q[i].grp);
        }
        free(q);

        int64_t now = time(NULL);
        int64_t since;

        pthread_mutex_lock(&H.lock);
        since = H.resum_since;
        H.resum_since = 0;
        pthread_mutex_unlock(&H.lock);
        if (since > 0) {
            meta_set("done60", since / 3600 * 3600);
            meta_set("done3600", since / 3600 * 3600);
            last_sum = 0;
        }
        if (now - last_sum >= 20) {
            summarise(now);
            last_sum = now;
        }
        if (now - last_tidy >= TIDY_S || db_bytes() > H.cap) {
            tidy(now);
            last_tidy = now;
        }
        if (stop)
            break;
    }
    return NULL;
}

static void push(struct rec *r)
{
    pthread_mutex_lock(&H.lock);
    if (!H.on || H.qn >= 200000) {
        H.dropped++;
        pthread_mutex_unlock(&H.lock);
        free(r->text);
        free(r->detail);
        free(r->grp);
        return;
    }
    if (H.qn == H.qcap) {
        int ncap = H.qcap ? H.qcap * 2 : 256;
        struct rec *nq = realloc(H.q, sizeof(*nq) * (size_t)ncap);

        if (!nq) {
            pthread_mutex_unlock(&H.lock);
            free(r->text);
            free(r->detail);
            free(r->grp);
            return;
        }
        H.q = nq;
        H.qcap = ncap;
    }
    H.q[H.qn++] = *r;
    pthread_mutex_unlock(&H.lock);
}

bool history_open(const char *path, int max_mb, char *err, size_t errlen)
{
    char dir[512], *slash;

    snprintf(H.path, sizeof(H.path), "%s", path);
    snprintf(dir, sizeof(dir), "%s", path);
    if ((slash = strrchr(dir, '/')) && slash != dir) {
        *slash = 0;
        mkdir(dir, 0755);
    }
    H.cap = H.cap_cfg = (int64_t)(max_mb > 16 ? max_mb : 16) * 1048576;
    if (sqlite3_open_v2(path, &H.db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL)
        != SQLITE_OK) {
        snprintf(err, errlen, "%s: %s", path, H.db ? sqlite3_errmsg(H.db) : "cannot open");
        sqlite3_close(H.db);
        H.db = NULL;
        return false;
    }
    sqlite3_busy_timeout(H.db, 5000);
    /* before any table exists: lets freed pages go back to the disk */
    exec(H.db, "PRAGMA auto_vacuum=INCREMENTAL");
    exec(H.db, "PRAGMA synchronous=NORMAL");
    exec(H.db, "PRAGMA journal_size_limit=16777216");
    if (!exec(H.db, SCHEMA)) {
        snprintf(err, errlen, "%s: the tables could not be made", path);
        sqlite3_close(H.db);
        H.db = NULL;
        return false;
    }
    /* columns added since the first version of the file */
    sqlite3_exec(H.db, "ALTER TABLE incidents ADD COLUMN diagnosis TEXT", NULL, NULL, NULL);
    sqlite3_exec(H.db, "ALTER TABLE incidents ADD COLUMN diag_t INTEGER", NULL, NULL, NULL);
    sqlite3_exec(H.db, "ALTER TABLE incidents ADD COLUMN actual TEXT", NULL, NULL, NULL);
    /* incidents still open when the supervisor last stopped: closed then */
    {
        sqlite3_stmt *st;

        if (sqlite3_prepare_v2(H.db, "UPDATE incidents SET closed = MAX(t, (SELECT MAX(t) FROM s10)),"
                               " summary = summary || ' (open when the supervisor stopped)' WHERE closed IS NULL",
                               -1, &st, NULL) == SQLITE_OK) {
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
        if (sqlite3_prepare_v2(H.db, "SELECT MAX(id) FROM incidents", -1, &st, NULL) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW)
                H.next_incident = sqlite3_column_int64(st, 0) + 1;
            sqlite3_finalize(st);
        }
        if (H.next_incident < 1)
            H.next_incident = 1;
        if (sqlite3_prepare_v2(H.db, "SELECT MAX(id) FROM evidence", -1, &st, NULL) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW)
                H.next_evidence = sqlite3_column_int64(st, 0) + 1;
            sqlite3_finalize(st);
        }
        if (H.next_evidence < 1)
            H.next_evidence = 1;
    }
    H.on = true;
    H.stop = false;
    if (pthread_create(&H.th, NULL, writer, NULL) != 0) {
        H.on = false;
        snprintf(err, errlen, "no thread for the history");
        sqlite3_close(H.db);
        H.db = NULL;
        return false;
    }
    log_info("history: %s (at most %d MB)", path, max_mb);
    return true;
}

void history_close(void)
{
    if (!H.on)
        return;
    pthread_mutex_lock(&H.lock);
    H.stop = true;
    pthread_cond_signal(&H.cond);
    pthread_mutex_unlock(&H.lock);
    pthread_join(H.th, NULL);
    pthread_mutex_lock(&H.lock);
    H.on = false;
    pthread_mutex_unlock(&H.lock);
    sqlite3_close(H.db);
    H.db = NULL;
}

void history_squeeze(int64_t bytes)
{
    pthread_mutex_lock(&H.lock);
    H.cap = bytes > 0 && bytes < H.cap_cfg ? (bytes > 4 * 1048576 ? bytes : 4 * 1048576) : H.cap_cfg;
    pthread_cond_signal(&H.cond);
    pthread_mutex_unlock(&H.lock);
}

int64_t history_bytes(void)
{
    return H.on ? db_bytes() : 0;
}

void history_resummarise(int64_t since)
{
    pthread_mutex_lock(&H.lock);
    H.resum_since = since > 0 ? since : 1;
    pthread_cond_signal(&H.cond);
    pthread_mutex_unlock(&H.lock);
}

bool history_enabled(void)
{
    return H.on;
}

void history_chan(int64_t t, const struct hist_chan *h)
{
    struct rec r;

    memset(&r, 0, sizeof(r));
    r.type = R_CHAN;
    r.t = t;
    r.c = *h;
    push(&r);
}

void history_metric(int64_t t, const char *name, double v)
{
    struct rec r;

    memset(&r, 0, sizeof(r));
    r.type = R_METRIC;
    r.t = t;
    snprintf(r.name, sizeof(r.name), "%s", name);
    r.v = v;
    push(&r);
}

void history_event(int64_t t, const char *chan, const char *level, const char *kind, const char *text)
{
    struct rec r;

    memset(&r, 0, sizeof(r));
    r.type = R_EVENT;
    r.t = t;
    snprintf(r.name, sizeof(r.name), "%s", chan ? chan : "");
    snprintf(r.level, sizeof(r.level), "%s", level);
    snprintf(r.kind, sizeof(r.kind), "%s", kind);
    r.text = strdup(text ? text : "");
    push(&r);
}

int64_t history_incident_open(int64_t t, const char *chan, const char *rule, const char *level,
                              const char *grp, const char *summary, const char *detail, int64_t parent)
{
    struct rec r;

    if (!H.on)
        return 0;
    memset(&r, 0, sizeof(r));
    r.type = R_INC_OPEN;
    pthread_mutex_lock(&H.lock);
    r.id = H.next_incident++;
    pthread_mutex_unlock(&H.lock);
    r.t = t;
    snprintf(r.name, sizeof(r.name), "%s", chan ? chan : "");
    snprintf(r.kind, sizeof(r.kind), "%s", rule);
    snprintf(r.level, sizeof(r.level), "%s", level);
    r.grp = strdup(grp ? grp : "");
    r.text = strdup(summary ? summary : "");
    r.detail = strdup(detail ? detail : "{}");
    r.parent = parent;
    push(&r);
    return r.id;
}

void history_record(int64_t t, int64_t closed, const char *chan, const char *rule, const char *grp,
                    const char *summary, const char *cause)
{
    struct rec r;

    if (!H.on)
        return;
    memset(&r, 0, sizeof(r));
    r.type = R_RECORD;
    r.t = t;
    r.closed = closed > t ? closed : t;
    snprintf(r.name, sizeof(r.name), "%s", chan ? chan : "");
    snprintf(r.kind, sizeof(r.kind), "%s", rule);
    r.grp = strdup(grp ? grp : "");
    r.text = strdup(summary ? summary : "");
    r.detail = strdup(cause ? cause : "");
    push(&r);
}

int64_t history_evidence_add(int64_t t, int64_t incident, const char *chan, const char *tool, const char *args,
                             const char *result, const char *file)
{
    struct rec r;

    if (!H.on)
        return 0;
    memset(&r, 0, sizeof(r));
    r.type = R_EVIDENCE;
    pthread_mutex_lock(&H.lock);
    r.id = H.next_evidence++;
    pthread_mutex_unlock(&H.lock);
    r.t = t;
    r.parent = incident;
    snprintf(r.name, sizeof(r.name), "%s", chan ? chan : "");
    snprintf(r.kind, sizeof(r.kind), "%s", tool);
    r.grp = strdup(args && *args == '{' ? args : "{}");
    r.detail = strdup(result && *result == '{' ? result : "{}");
    r.text = strdup(file ? file : "");
    push(&r);
    return r.id;
}

void history_incident_verdict(int64_t id, int64_t t, const char *verdict, const char *note, const char *actual)
{
    struct rec r;

    if (!H.on || id <= 0)
        return;
    memset(&r, 0, sizeof(r));
    r.type = R_INC_VERDICT;
    r.id = id;
    r.t = t;
    snprintf(r.kind, sizeof(r.kind), "%s", verdict);
    r.text = strdup(note ? note : "");
    r.detail = strdup(actual && *actual == '{' ? actual : "{}");
    push(&r);
}

void history_incident_diagnosis(int64_t id, int64_t t, const char *json)
{
    struct rec r;

    if (!H.on || id <= 0)
        return;
    memset(&r, 0, sizeof(r));
    r.type = R_INC_DIAG;
    r.id = id;
    r.t = t;
    r.detail = strdup(json && *json == '{' ? json : "{}");
    push(&r);
}

void history_message(int64_t t, const char *peer, const char *dir, const char *text, const char *data,
                     int64_t incident)
{
    struct rec r;

    if (!H.on)
        return;
    memset(&r, 0, sizeof(r));
    r.type = R_MESSAGE;
    r.t = t;
    r.parent = incident;
    snprintf(r.name, sizeof(r.name), "%s", peer ? peer : "");
    snprintf(r.kind, sizeof(r.kind), "%s", dir);
    r.text = strdup(text ? text : "");
    r.detail = strdup(data && *data == '{' ? data : "{}");
    push(&r);
}

void history_incident_update(int64_t id, int64_t closed, const char *summary, const char *detail, int64_t parent)
{
    struct rec r;

    if (!H.on || id <= 0)
        return;
    memset(&r, 0, sizeof(r));
    r.type = R_INC_UPDATE;
    r.id = id;
    r.closed = closed;
    r.text = summary ? strdup(summary) : NULL;
    r.detail = detail ? strdup(detail) : NULL;
    r.parent = parent;
    push(&r);
}

/* ---- reading ---------------------------------------------------------------- */

static sqlite3 *reader(void)
{
    sqlite3 *db = NULL;

    if (!H.on || sqlite3_open_v2(H.path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 3000);
    return db;
}

static void jstr(FILE *f, const char *s)
{
    fputc('"', f);
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;

        if (c == '"' || c == '\\')
            fprintf(f, "\\%c", c);
        else if (c < 0x20)
            fprintf(f, "\\u%04x", c);
        else
            fputc(c, f);
    }
    fputc('"', f);
}

/* The table and bucket for a span: at most about 720 points. */
static void pick(int64_t span, const char **chan_t, const char **met_t, int *step, int *bucket)
{
    if (span <= 6 * 3600) {
        *chan_t = "s10", *met_t = "m10", *step = 10;
    } else if (span <= 10 * 86400) {
        *chan_t = "s60", *met_t = "m60", *step = 60;
    } else {
        *chan_t = "s3600", *met_t = "m3600", *step = 3600;
    }
    *bucket = (int)(span / 720 / *step) * *step;
    if (*bucket < *step)
        *bucket = *step;
}

bool history_query_chan(FILE *f, const char *chan, int64_t from, int64_t to)
{
    sqlite3 *db = reader();
    sqlite3_stmt *st;
    const char *ct, *mt;
    int step, bucket;
    char sql[700];
    bool fine;

    if (!db)
        return false;
    pick(to - from, &ct, &mt, &step, &bucket);
    fine = step == 10;
    /* rates averaged and at their highest, counters summed, per bucket */
    snprintf(sql, sizeof(sql),
             "SELECT t / %d * %d, AVG(%s), MAX(%s), AVG(%s), MAX(%s), SUM(cc), SUM(ovf), SUM(drp), SUM(late),"
             " SUM(lost), SUM(sw), SUM(rs), MAX(hl), %s FROM %s WHERE cid = (SELECT id FROM chans WHERE name = ?)"
             " AND t >= ? AND t < ? GROUP BY t / %d ORDER BY 1",
             bucket, bucket, fine ? "inb" : "in_avg", fine ? "inb" : "in_max", fine ? "outb" : "out_avg",
             fine ? "outb" : "out_max", fine ? "SUM(CASE WHEN st = 1 THEN 10 ELSE 0 END)" : "SUM(up)", ct, bucket);
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    sqlite3_bind_text(st, 1, chan, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, from);
    sqlite3_bind_int64(st, 3, to);
    fprintf(f, "{\"step\": %d, \"rows\": [", bucket);
    for (int n = 0; sqlite3_step(st) == SQLITE_ROW; n++) {
        /* [t, in_kbps, in_max, out_kbps, out_max, cc, overflow, dropped, late, lost, switches, restarts,
         *  worst health, seconds running] */
        fprintf(f, "%s[%lld", n ? "," : "", (long long)sqlite3_column_int64(st, 0));
        for (int k = 1; k <= 13; k++)
            fprintf(f, ",%lld", (long long)sqlite3_column_int64(st, k));
        fputc(']', f);
    }
    fprintf(f, "], \"columns\": [\"t\", \"in_kbps\", \"in_max_kbps\", \"out_kbps\", \"out_max_kbps\", \"cc\","
               " \"overflow\", \"dropped\", \"late\", \"lost\", \"switches\", \"restarts\", \"health\", \"up_s\"]}");
    sqlite3_finalize(st);
    sqlite3_close(db);
    return true;
}

bool history_query_metrics(FILE *f, const char *metric, int64_t from, int64_t to)
{
    sqlite3 *db = reader();
    sqlite3_stmt *st;
    const char *ct, *mt;
    int step, bucket;
    char sql[600], like[128];
    const char *star = strchr(metric, '*');
    int64_t mid_prev = -1;
    int n = 0;

    if (!db)
        return false;
    pick(to - from, &ct, &mt, &step, &bucket);
    snprintf(like, sizeof(like), "%.*s%%", star ? (int)(star - metric) : (int)strlen(metric), metric);
    snprintf(sql, sizeof(sql),
             "SELECT m.name, x.t / %d * %d, AVG(x.v), %s FROM %s x JOIN metrics m ON m.id = x.mid"
             " WHERE m.name %s ? AND x.t >= ? AND x.t < ? GROUP BY x.mid, x.t / %d ORDER BY m.name, 2",
             bucket, bucket, step == 10 ? "MAX(x.v)" : "MAX(x.vmax)", mt, star ? "LIKE" : "=", bucket);
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    sqlite3_bind_text(st, 1, star ? like : metric, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, from);
    sqlite3_bind_int64(st, 3, to);
    fprintf(f, "{\"step\": %d, \"series\": {", bucket);
    {
        char prev[96] = "";

        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *name = (const char *)sqlite3_column_text(st, 0);

            if (!name)
                continue;
            if (strcmp(prev, name)) {
                if (prev[0])
                    fprintf(f, "]");
                fprintf(f, "%s", n++ ? ", " : "");
                jstr(f, name);
                fprintf(f, ": [");
                snprintf(prev, sizeof(prev), "%s", name);
                mid_prev = 0;
            }
            fprintf(f, "%s[%lld,%.4g,%.4g]", mid_prev++ ? "," : "", (long long)sqlite3_column_int64(st, 1),
                    sqlite3_column_double(st, 2), sqlite3_column_double(st, 3));
        }
        if (prev[0])
            fprintf(f, "]");
    }
    fprintf(f, "}}");
    sqlite3_finalize(st);
    sqlite3_close(db);
    return true;
}

bool history_query_records(FILE *f, const char *chan, int64_t from, int64_t to, int limit)
{
    sqlite3 *db = reader();
    sqlite3_stmt *st;
    char sql[300];

    if (!db)
        return false;
    snprintf(sql, sizeof(sql), "SELECT t, closed, chan, rule, grp, summary, cause FROM records"
             " WHERE t < ? AND closed >= ? %s ORDER BY t DESC, id DESC LIMIT ?", chan ? "AND chan = ?" : "");
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    sqlite3_bind_int64(st, 1, to);
    sqlite3_bind_int64(st, 2, from);
    if (chan) {
        sqlite3_bind_text(st, 3, chan, -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 4, limit);
    } else {
        sqlite3_bind_int(st, 3, limit);
    }
    fprintf(f, "{\"records\": [");
    for (int n = 0; sqlite3_step(st) == SQLITE_ROW; n++) {
        fprintf(f, "%s\n{\"t\": %lld, \"closed\": %lld, \"channel\": ", n ? "," : "",
                (long long)sqlite3_column_int64(st, 0), (long long)sqlite3_column_int64(st, 1));
        if (sqlite3_column_type(st, 2) == SQLITE_NULL)
            fprintf(f, "null");
        else
            jstr(f, (const char *)sqlite3_column_text(st, 2));
        fprintf(f, ", \"rule\": ");
        jstr(f, (const char *)sqlite3_column_text(st, 3));
        fprintf(f, ", \"source\": ");
        jstr(f, (const char *)sqlite3_column_text(st, 4));
        fprintf(f, ", \"summary\": ");
        jstr(f, (const char *)sqlite3_column_text(st, 5));
        fprintf(f, ", \"cause\": ");
        jstr(f, (const char *)sqlite3_column_text(st, 6));
        fputc('}', f);
    }
    fprintf(f, "]}");
    sqlite3_finalize(st);
    sqlite3_close(db);
    return true;
}

/* The summary: per source (its address, without the program or other
 * options), the CC bursts and short drops kept as records, the channel
 * incidents, and when in the day (the site's hours) the records came. */
#define SUM_SOURCES 256
struct sum_src {
    char key[200];
    int cc, cc_s, drops, drops_s, incidents;
    int hours[24];
    char chans[300];
    int n_chans;
};

static struct sum_src *sum_find(struct sum_src *v, int *n, const char *grp)
{
    char key[200];
    size_t k;

    snprintf(key, sizeof(key), "%s", grp && *grp ? grp : "(no source)");
    k = strcspn(key, "?");
    key[k] = 0;
    for (int i = 0; i < *n; i++)
        if (!strcmp(v[i].key, key))
            return &v[i];
    if (*n >= SUM_SOURCES)
        return NULL;
    memset(&v[*n], 0, sizeof(v[*n]));
    snprintf(v[*n].key, sizeof(v[*n].key), "%s", key);
    return &v[(*n)++];
}

static void sum_chan(struct sum_src *x, const char *chan)
{
    size_t n = strlen(x->chans), l;
    const char *p = x->chans;

    if (!chan || !*chan)
        return;
    l = strlen(chan);
    while ((p = strstr(p, chan))) {             /* already listed? */
        if ((p == x->chans || (p - x->chans >= 2 && p[-2] == ',')) && (p[l] == 0 || p[l] == ','))
            return;
        p += l;
    }
    if (n + l + 3 < sizeof(x->chans))
        snprintf(x->chans + n, sizeof(x->chans) - n, "%s%s", n ? ", " : "", chan);
    x->n_chans++;
}

static int sum_cmp(const void *a, const void *b)
{
    const struct sum_src *x = a, *y = b;

    return (y->cc + y->drops + y->incidents) - (x->cc + x->drops + x->incidents);
}

bool history_summary(FILE *f, int64_t from, int64_t to)
{
    sqlite3 *db = reader();
    sqlite3_stmt *st;
    struct sum_src *v;
    int n = 0, hours[24] = { 0 }, cc = 0, drops = 0, incidents = 0;

    if (!db)
        return false;
    if (!(v = calloc(SUM_SOURCES, sizeof(*v)))) {
        sqlite3_close(db);
        return false;
    }
    if (sqlite3_prepare_v2(db, "SELECT t, closed, chan, rule, grp FROM records WHERE t >= ? AND t < ?", -1, &st,
                           NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, from);
        sqlite3_bind_int64(st, 2, to);
        while (sqlite3_step(st) == SQLITE_ROW) {
            time_t t = (time_t)sqlite3_column_int64(st, 0);
            int lasted = (int)(sqlite3_column_int64(st, 1) - t);
            const char *rule = (const char *)sqlite3_column_text(st, 3);
            struct sum_src *x = sum_find(v, &n, (const char *)sqlite3_column_text(st, 4));
            struct tm tm;

            if (!x || !rule)
                continue;
            localtime_r(&t, &tm);
            if (!strcmp(rule, "cc_errors")) {
                x->cc++;
                x->cc_s += lasted;
                cc++;
            } else {
                x->drops++;
                x->drops_s += lasted;
                drops++;
            }
            x->hours[tm.tm_hour]++;
            hours[tm.tm_hour]++;
            sum_chan(x, (const char *)sqlite3_column_text(st, 2));
        }
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(db, "SELECT chan, grp FROM incidents WHERE t >= ? AND t < ? AND chan IS NOT NULL"
                           " AND (verdict IS NULL OR verdict != 'exclude')", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, from);
        sqlite3_bind_int64(st, 2, to);
        while (sqlite3_step(st) == SQLITE_ROW) {
            struct sum_src *x = sum_find(v, &n, (const char *)sqlite3_column_text(st, 1));

            if (!x)
                continue;
            x->incidents++;
            incidents++;
            sum_chan(x, (const char *)sqlite3_column_text(st, 0));
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    qsort(v, (size_t)n, sizeof(*v), sum_cmp);
    fprintf(f, "{\"from\": %lld, \"to\": %lld, \"cc_bursts\": %d, \"short_drops\": %d, \"incidents\": %d, \"hours\": [",
            (long long)from, (long long)to, cc, drops, incidents);
    for (int h = 0; h < 24; h++)
        fprintf(f, "%s%d", h ? ", " : "", hours[h]);
    fprintf(f, "], \"sources\": [");
    for (int i = 0; i < n; i++) {
        fprintf(f, "%s\n{\"source\": ", i ? "," : "");
        jstr(f, v[i].key);
        fprintf(f, ", \"cc_bursts\": %d, \"cc_s\": %d, \"short_drops\": %d, \"short_drops_s\": %d, \"incidents\": %d,"
                " \"channels\": ", v[i].cc, v[i].cc_s, v[i].drops, v[i].drops_s, v[i].incidents);
        jstr(f, v[i].chans);
        fprintf(f, ", \"n_channels\": %d, \"hours\": [", v[i].n_chans);
        for (int h = 0; h < 24; h++)
            fprintf(f, "%s%d", h ? ", " : "", v[i].hours[h]);
        fprintf(f, "]}");
    }
    fprintf(f, "]}");
    free(v);
    return true;
}

bool history_query_events(FILE *f, const char *chan, int64_t from, int64_t to, int limit)
{
    sqlite3 *db = reader();
    sqlite3_stmt *st;
    char sql[400];

    if (!db)
        return false;
    snprintf(sql, sizeof(sql),
             "SELECT e.t, c.name, e.level, e.kind, e.text FROM events e LEFT JOIN chans c ON c.id = e.cid"
             " WHERE e.t >= ? AND e.t < ? %s ORDER BY e.t DESC, e.id DESC LIMIT ?",
             chan ? (chan[0] ? "AND c.name = ?" : "AND e.cid IS NULL") : "");
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    sqlite3_bind_int64(st, 1, from);
    sqlite3_bind_int64(st, 2, to);
    if (chan && chan[0]) {
        sqlite3_bind_text(st, 3, chan, -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 4, limit);
    } else {
        sqlite3_bind_int(st, 3, limit);
    }
    fprintf(f, "{\"events\": [");
    for (int n = 0; sqlite3_step(st) == SQLITE_ROW; n++) {
        fprintf(f, "%s\n{\"t\": %lld, \"channel\": ", n ? "," : "", (long long)sqlite3_column_int64(st, 0));
        if (sqlite3_column_type(st, 1) == SQLITE_NULL)
            fprintf(f, "null");
        else
            jstr(f, (const char *)sqlite3_column_text(st, 1));
        fprintf(f, ", \"level\": ");
        jstr(f, (const char *)sqlite3_column_text(st, 2));
        fprintf(f, ", \"kind\": ");
        jstr(f, (const char *)sqlite3_column_text(st, 3));
        fprintf(f, ", \"text\": ");
        jstr(f, (const char *)sqlite3_column_text(st, 4));
        fputc('}', f);
    }
    fprintf(f, "]}");
    sqlite3_finalize(st);
    sqlite3_close(db);
    return true;
}

static __thread bool row_brief;            /* members without their evidence snapshot (the technician's report) */
static __thread bool report_mode;          /* history_report_incident: brief, and without an earlier answer */

static void incident_row(FILE *f, sqlite3_stmt *st)
{
    /* id, t, closed, chan, rule, level, grp, summary, detail, parent, verdict, note, verdict_t */
    fprintf(f, "{\"id\": %lld, \"opened\": %lld, \"closed\": ", (long long)sqlite3_column_int64(st, 0),
            (long long)sqlite3_column_int64(st, 1));
    if (sqlite3_column_type(st, 2) == SQLITE_NULL)
        fprintf(f, "null");
    else
        fprintf(f, "%lld", (long long)sqlite3_column_int64(st, 2));
    fprintf(f, ", \"channel\": ");
    if (sqlite3_column_type(st, 3) == SQLITE_NULL)
        fprintf(f, "null");
    else
        jstr(f, (const char *)sqlite3_column_text(st, 3));
    fprintf(f, ", \"rule\": ");
    jstr(f, (const char *)sqlite3_column_text(st, 4));
    fprintf(f, ", \"level\": ");
    jstr(f, (const char *)sqlite3_column_text(st, 5));
    fprintf(f, ", \"group\": ");
    jstr(f, (const char *)sqlite3_column_text(st, 6));
    fprintf(f, ", \"summary\": ");
    jstr(f, (const char *)sqlite3_column_text(st, 7));
    {
        const char *d = (const char *)sqlite3_column_text(st, 8);

        fprintf(f, ", \"detail\": %s", d && d[0] == '{' && !row_brief ? d : "{}");     /* JSON made by the supervisor */
    }
    fprintf(f, ", \"parent\": ");
    if (sqlite3_column_type(st, 9) == SQLITE_NULL)
        fprintf(f, "null");
    else
        fprintf(f, "%lld", (long long)sqlite3_column_int64(st, 9));
    fprintf(f, ", \"verdict\": ");
    if (sqlite3_column_type(st, 10) == SQLITE_NULL || report_mode)
        fprintf(f, "null");
    else
        jstr(f, (const char *)sqlite3_column_text(st, 10));
    fprintf(f, ", \"note\": ");
    if (sqlite3_column_type(st, 11) == SQLITE_NULL || report_mode)
        fprintf(f, "null");
    else
        jstr(f, (const char *)sqlite3_column_text(st, 11));
    {
        const char *dg = (const char *)sqlite3_column_text(st, 13);

        const char *ac = (const char *)sqlite3_column_text(st, 15);

        fprintf(f, ", \"diagnosis\": %s, \"diag_t\": %lld", dg && *dg == '{' && !report_mode ? dg : "null",
                (long long)sqlite3_column_int64(st, 14));
        fprintf(f, ", \"actual\": %s, \"verdict_t\": %lld", ac && *ac == '{' && !report_mode ? ac : "null",
                (long long)sqlite3_column_int64(st, 12));
    }
    fputc('}', f);
}

#define INC_COLS "id, t, closed, chan, rule, level, grp, summary, detail, parent, verdict, note, verdict_t, diagnosis, diag_t, actual"

/* id, t, incident, chan, tool, args, result, file */
static void evidence_row(FILE *f, sqlite3_stmt *st)
{
    const char *file = (const char *)sqlite3_column_text(st, 7);
    struct stat sb;
    bool there = file && *file && stat(file, &sb) == 0;
    const char *a = (const char *)sqlite3_column_text(st, 5), *r = (const char *)sqlite3_column_text(st, 6);

    fprintf(f, "{\"id\": %lld, \"t\": %lld, \"incident\": ", (long long)sqlite3_column_int64(st, 0),
            (long long)sqlite3_column_int64(st, 1));
    if (sqlite3_column_type(st, 2) == SQLITE_NULL)
        fprintf(f, "null");
    else
        fprintf(f, "%lld", (long long)sqlite3_column_int64(st, 2));
    fprintf(f, ", \"channel\": ");
    if (sqlite3_column_type(st, 3) == SQLITE_NULL)
        fprintf(f, "null");
    else
        jstr(f, (const char *)sqlite3_column_text(st, 3));
    fprintf(f, ", \"tool\": ");
    jstr(f, (const char *)sqlite3_column_text(st, 4));
    fprintf(f, ", \"args\": %s, \"result\": %s, \"file\": ", a && *a == '{' ? a : "{}", r && *r == '{' ? r : "{}");
    jstr(f, file && strrchr(file, '/') ? strrchr(file, '/') + 1 : file ? file : "");
    fprintf(f, ", \"file_bytes\": %lld}", there ? (long long)sb.st_size : -1LL);
}

bool history_incident_answer(int64_t id, char **diagnosis, char **verdict, char **note, char **actual)
{
    sqlite3 *db = reader();
    sqlite3_stmt *st;
    bool ok = false;

    *diagnosis = *verdict = *note = *actual = NULL;
    if (!db)
        return false;
    if (sqlite3_prepare_v2(db, "SELECT diagnosis, verdict, note, actual FROM incidents WHERE id = ?", -1, &st, NULL)
        == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            char **out[] = { diagnosis, verdict, note, actual };

            for (int k = 0; k < 4; k++)
                *out[k] = strdup(sqlite3_column_text(st, k) ? (const char *)sqlite3_column_text(st, k) : "");
            ok = true;
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return ok;
}

int history_verdict_ids(int64_t *ids, int max)
{
    sqlite3 *db = reader();
    sqlite3_stmt *st;
    int n = 0;

    if (!db)
        return 0;
    if (sqlite3_prepare_v2(db, "SELECT id FROM incidents WHERE verdict IS NOT NULL AND verdict != 'exclude' AND diagnosis IS NOT NULL ORDER BY t",
                           -1, &st, NULL) == SQLITE_OK) {
        while (n < max && sqlite3_step(st) == SQLITE_ROW)
            ids[n++] = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return n;
}

static void json_category(const char *j, char *out, size_t len)
{
    const char *p = j ? strstr(j, "\"category\"") : NULL;
    size_t n = 0;

    out[0] = 0;
    if (!p || !(p = strchr(p + 10, ':')))
        return;
    while (*++p == ' ')
        ;
    if (*p != '"')
        return;
    for (p++; *p && *p != '"' && n + 1 < len; p++)
        out[n++] = *p;
    out[n] = 0;
}

int history_eval_cases(struct hist_case *out, int max)
{
    sqlite3 *db = reader();
    sqlite3_stmt *st;
    int n = 0;

    if (!db)
        return 0;
    if (sqlite3_prepare_v2(db, "SELECT id, rule, COALESCE(chan, grp, ''), summary, verdict, actual, diagnosis, t, closed"
                               " FROM incidents WHERE verdict IS NOT NULL AND verdict != 'exclude' AND diagnosis IS NOT NULL AND parent IS NULL"
                               " ORDER BY t DESC", -1, &st, NULL) == SQLITE_OK) {
        while (n < max && sqlite3_step(st) == SQLITE_ROW) {
            struct hist_case *c = &out[n++];
            const char *t;

            memset(c, 0, sizeof(*c));
            c->id = sqlite3_column_int64(st, 0);
#define COPY(dst, col) snprintf(dst, sizeof(dst), "%s", (t = (const char *)sqlite3_column_text(st, col)) ? t : "")
            COPY(c->rule, 1);
            COPY(c->where, 2);
            COPY(c->summary, 3);
            COPY(c->verdict, 4);
#undef COPY
            json_category((const char *)sqlite3_column_text(st, 5), c->actual_category, sizeof(c->actual_category));
            json_category((const char *)sqlite3_column_text(st, 6), c->said_category, sizeof(c->said_category));
            c->lasted_s = sqlite3_column_type(st, 8) == SQLITE_NULL ? -1
                        : sqlite3_column_int64(st, 8) - sqlite3_column_int64(st, 7);
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return n;
}

bool history_open_readonly(const char *path, char *err, size_t errlen)
{
    sqlite3 *db = NULL;

    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        snprintf(err, errlen, "%s: %s", path, db ? sqlite3_errmsg(db) : "cannot open");
        sqlite3_close(db);
        return false;
    }
    sqlite3_close(db);
    snprintf(H.path, sizeof(H.path), "%s", path);
    H.on = true;
    return true;
}

void history_verdict_counts(FILE *f)
{
    sqlite3 *db = reader();
    sqlite3_stmt *st;
    int right = 0, partly = 0, wrong = 0, excluded = 0, open = 0, looked = 0;

    if (db && sqlite3_prepare_v2(db, "SELECT verdict, COUNT(*), SUM(diagnosis IS NOT NULL) FROM incidents"
                                     " WHERE parent IS NULL GROUP BY verdict", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(st, 0);
            int n = sqlite3_column_int(st, 1);

            if (!v)
                open = n, looked = sqlite3_column_int(st, 2);
            else if (!strcmp(v, "right"))
                right = n;
            else if (!strcmp(v, "partly"))
                partly = n;
            else if (!strcmp(v, "wrong"))
                wrong = n;
            else if (!strcmp(v, "exclude"))
                excluded = n;
        }
        sqlite3_finalize(st);
    }
    if (db)
        sqlite3_close(db);
    fprintf(f, "{\"right\": %d, \"partly\": %d, \"wrong\": %d, \"excluded\": %d, \"waiting\": %d, \"waiting_looked_at\": %d}",
            right, partly, wrong, excluded, open, looked);
}

bool history_query_incidents_brief(FILE *f, int64_t from, int64_t to, bool open_only, int limit)
{
    bool ok;

    row_brief = true;
    ok = history_query_incidents(f, NULL, from, to, open_only, limit);
    row_brief = false;
    return ok;
}

bool history_report_incident(FILE *f, int64_t id, int64_t now)
{
    bool ok;

    report_mode = true;
    ok = history_query_incident(f, id, now);
    report_mode = row_brief = false;
    return ok;
}

/* One past incident as the technician is shown it. */
static void similar_row(FILE *f, sqlite3_stmt *st, const char *where, int n)
{
    const char *dg = (const char *)sqlite3_column_text(st, 6);

    fprintf(f, "%s\n{\"where\": \"%s\", \"began\": %lld, \"lasted_s\": %lld, \"channel\": ", n ? "," : "", where,
            (long long)sqlite3_column_int64(st, 0),
            sqlite3_column_type(st, 1) == SQLITE_NULL ? -1LL
                : (long long)(sqlite3_column_int64(st, 1) - sqlite3_column_int64(st, 0)));
    if (sqlite3_column_type(st, 2) == SQLITE_NULL)
        fprintf(f, "null");
    else
        jstr(f, (const char *)sqlite3_column_text(st, 2));
    fprintf(f, ", \"rule\": ");
    jstr(f, (const char *)sqlite3_column_text(st, 3));
    fprintf(f, ", \"source\": ");
    jstr(f, (const char *)sqlite3_column_text(st, 4));
    fprintf(f, ", \"summary\": ");
    jstr(f, (const char *)sqlite3_column_text(st, 5));
    fprintf(f, ", \"technician_said\": %s, \"person_said\": ", dg && *dg == '{' ? dg : "null");
    if (sqlite3_column_type(st, 7) == SQLITE_NULL) {
        fprintf(f, "null");
    } else {
        fprintf(f, "{\"verdict\": ");
        jstr(f, (const char *)sqlite3_column_text(st, 7));
        fprintf(f, ", \"note\": ");
        jstr(f, sqlite3_column_type(st, 8) == SQLITE_NULL ? "" : (const char *)sqlite3_column_text(st, 8));
        {
            const char *ac = (const char *)sqlite3_column_text(st, 9);

            fprintf(f, ", \"it_was\": %s", ac && *ac == '{' && strcmp(ac, "{}") ? ac : "null");
        }
        fputc('}', f);
    }
    fputc('}', f);
}

/*
 * The past incidents most like this one, before it:
 *   - the same rule on the same channel or the same source, those with a
 *     person's verdict first, then the newest ("same channel or source");
 *   - and the same rule elsewhere, only those a person has judged, one for
 *     each different thing a person said it was, the newest of each
 *     ("elsewhere"): what was learnt on other channels reaches a channel
 *     that has had no such incident judged yet.
 * `limit` in all, at most two thirds of it from the first kind.
 */
bool history_similar(FILE *f, int64_t id, int limit)
{
    sqlite3 *db = reader();
    sqlite3_stmt *st;
    int n = 0, judged = 0, near = limit - limit / 3;

    if (!db)
        return false;
    if (sqlite3_prepare_v2(db,
            "SELECT o.t, o.closed, o.chan, o.rule, o.grp, o.summary, o.diagnosis, o.verdict, o.note, o.actual FROM incidents o,"
            " incidents i WHERE i.id = ? AND o.id != i.id AND o.t < i.t AND o.rule = i.rule AND"
            " (o.chan IS i.chan OR (o.grp = i.grp AND o.grp != '')) AND o.parent IS NULL"
            " AND (o.verdict IS NULL OR o.verdict != 'exclude')"
            " ORDER BY o.verdict IS NULL, o.t DESC LIMIT ?", -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int(st, 2, near);
    fprintf(f, "[");
    while (sqlite3_step(st) == SQLITE_ROW) {
        /* ones nobody judged teach little: only while there are few judged ones */
        if (sqlite3_column_type(st, 7) == SQLITE_NULL && judged >= 3)
            break;
        judged += sqlite3_column_type(st, 7) != SQLITE_NULL;
        similar_row(f, st, "same channel or source", n++);
    }
    sqlite3_finalize(st);
    if (n < limit && sqlite3_prepare_v2(db,
            "SELECT o.t, o.closed, o.chan, o.rule, o.grp, o.summary, o.diagnosis, o.verdict, o.note, o.actual FROM incidents o,"
            " incidents i WHERE i.id = ? AND o.id != i.id AND o.t < i.t AND o.rule = i.rule AND o.parent IS NULL"
            " AND o.verdict IS NOT NULL AND o.verdict != 'exclude' AND NOT (o.chan IS i.chan OR (o.grp = i.grp AND o.grp != ''))"
            " AND o.id IN (SELECT max(x.id) FROM incidents x, incidents y WHERE y.id = ? AND x.verdict IS NOT NULL AND x.verdict != 'exclude'"
            " AND x.t < y.t AND x.rule = y.rule AND x.parent IS NULL"
            " AND NOT (x.chan IS y.chan OR (x.grp = y.grp AND x.grp != ''))"
            " GROUP BY x.verdict, coalesce(CASE WHEN json_valid(x.actual) THEN json_extract(x.actual, '$.category') END,"
            " CASE WHEN json_valid(x.diagnosis) THEN json_extract(x.diagnosis, '$.category') END, ''))"
            " ORDER BY o.t DESC LIMIT ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        sqlite3_bind_int64(st, 2, id);
        sqlite3_bind_int(st, 3, limit - n);
        while (sqlite3_step(st) == SQLITE_ROW)
            similar_row(f, st, "elsewhere", n++);
        sqlite3_finalize(st);
    }
    fprintf(f, "]");
    sqlite3_close(db);
    return true;
}

bool history_query_evidence(FILE *f, const char *chan, int64_t incident, int limit)
{
    sqlite3 *db = reader();
    sqlite3_stmt *st;
    char sql[400];

    if (!db)
        return false;
    snprintf(sql, sizeof(sql), "SELECT id, t, incident, chan, tool, args, result, file FROM evidence WHERE 1 %s %s"
             " ORDER BY t DESC, id DESC LIMIT ?", chan ? "AND chan = ?" : "", incident ? "AND incident = ?" : "");
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    {
        int k = 1;

        if (chan)
            sqlite3_bind_text(st, k++, chan, -1, SQLITE_STATIC);
        if (incident)
            sqlite3_bind_int64(st, k++, incident);
        sqlite3_bind_int(st, k, limit);
    }
    fprintf(f, "{\"evidence\": [");
    for (int n = 0; sqlite3_step(st) == SQLITE_ROW; n++) {
        fprintf(f, "%s\n", n ? "," : "");
        evidence_row(f, st);
    }
    fprintf(f, "]}");
    sqlite3_finalize(st);
    sqlite3_close(db);
    return true;
}

bool history_query_messages(FILE *f, int64_t incident, int limit)
{
    sqlite3 *db = reader();
    sqlite3_stmt *st;

    if (!db)
        return false;
    if (sqlite3_prepare_v2(db, incident ? "SELECT t, peer, dir, text, data, incident FROM messages WHERE incident = ?"
                                          " ORDER BY t DESC, id DESC LIMIT ?"
                                        : "SELECT t, peer, dir, text, data, incident FROM messages"
                                          " ORDER BY t DESC, id DESC LIMIT ?", -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    if (incident) {
        sqlite3_bind_int64(st, 1, incident);
        sqlite3_bind_int(st, 2, limit);
    } else {
        sqlite3_bind_int(st, 1, limit);
    }
    fprintf(f, "[");
    for (int n = 0; sqlite3_step(st) == SQLITE_ROW; n++) {
        const char *d = (const char *)sqlite3_column_text(st, 4);

        fprintf(f, "%s\n{\"t\": %lld, \"peer\": ", n ? "," : "", (long long)sqlite3_column_int64(st, 0));
        jstr(f, (const char *)sqlite3_column_text(st, 1));
        fprintf(f, ", \"dir\": ");
        jstr(f, (const char *)sqlite3_column_text(st, 2));
        fprintf(f, ", \"text\": ");
        jstr(f, (const char *)sqlite3_column_text(st, 3));
        fprintf(f, ", \"data\": %s, \"incident\": ", d && *d == '{' ? d : "{}");
        if (sqlite3_column_type(st, 5) == SQLITE_NULL)
            fprintf(f, "null}");
        else
            fprintf(f, "%lld}", (long long)sqlite3_column_int64(st, 5));
    }
    fprintf(f, "]");
    sqlite3_finalize(st);
    sqlite3_close(db);
    return true;
}

bool history_evidence_file(int64_t id, char *path, size_t len)
{
    sqlite3 *db = reader();
    sqlite3_stmt *st;
    bool ok = false;

    if (!db)
        return false;
    if (sqlite3_prepare_v2(db, "SELECT file FROM evidence WHERE id = ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0) && *sqlite3_column_text(st, 0)) {
            snprintf(path, len, "%s", (const char *)sqlite3_column_text(st, 0));
            ok = true;
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return ok;
}

bool history_query_incidents(FILE *f, const char *chan, int64_t from, int64_t to, bool open_only, int limit)
{
    sqlite3 *db = reader();
    sqlite3_stmt *st;
    char sql[600];

    if (!db)
        return false;
    /* those that overlap the span; for a channel, its own and the source's it belongs to */
    snprintf(sql, sizeof(sql),
             "SELECT " INC_COLS " FROM incidents WHERE t < ? AND (closed IS NULL OR closed >= ?) %s %s"
             " ORDER BY closed IS NOT NULL, t DESC, id DESC LIMIT ?",
             open_only ? "AND closed IS NULL" : "",
             chan ? "AND (chan = ? OR id IN (SELECT parent FROM incidents WHERE chan = ? AND parent IS NOT NULL))"
                  : "");
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    sqlite3_bind_int64(st, 1, to);
    sqlite3_bind_int64(st, 2, from);
    if (chan) {
        sqlite3_bind_text(st, 3, chan, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 4, chan, -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 5, limit);
    } else {
        sqlite3_bind_int(st, 3, limit);
    }
    fprintf(f, "{\"incidents\": [");
    for (int n = 0; sqlite3_step(st) == SQLITE_ROW; n++) {
        fprintf(f, "%s\n", n ? "," : "");
        incident_row(f, st);
    }
    fprintf(f, "]}");
    sqlite3_finalize(st);
    sqlite3_close(db);
    return true;
}

bool history_query_incident(FILE *f, int64_t id, int64_t now)
{
    sqlite3 *db = reader();
    sqlite3_stmt *st;
    int64_t t0 = 0, t1 = 0;
    bool found = false;

    if (!db)
        return false;
    if (sqlite3_prepare_v2(db, "SELECT " INC_COLS " FROM incidents WHERE id = ?", -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    sqlite3_bind_int64(st, 1, id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        found = true;
        t0 = sqlite3_column_int64(st, 1);
        t1 = sqlite3_column_type(st, 2) == SQLITE_NULL ? now : sqlite3_column_int64(st, 2);
        fprintf(f, "{\"incident\": ");
        incident_row(f, st);
    }
    sqlite3_finalize(st);
    if (!found) {
        sqlite3_close(db);
        return false;
    }
    /* the channels' incidents that belong to it */
    row_brief = report_mode;
    fprintf(f, ", \"members\": [");
    if (sqlite3_prepare_v2(db, "SELECT " INC_COLS " FROM incidents WHERE parent = ? ORDER BY t", -1, &st, NULL)
        == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        for (int n = 0; sqlite3_step(st) == SQLITE_ROW; n++) {
            fprintf(f, "%s\n", n ? "," : "");
            incident_row(f, st);
        }
        sqlite3_finalize(st);
    }
    /* what the tools found for it and its members */
    fprintf(f, "], \"evidence\": [");
    /* its own, its members', and what was gathered on its channels while it lasted (a recording made
     * for one problem is evidence for the others it overlaps) */
    if (sqlite3_prepare_v2(db, "SELECT id, t, incident, chan, tool, args, result, file FROM evidence WHERE incident = ?"
                           " OR incident IN (SELECT id FROM incidents WHERE parent = ?)"
                           " OR (t >= ? AND t <= ? AND chan IN (SELECT chan FROM incidents WHERE (id = ? OR parent = ?)"
                           " AND chan IS NOT NULL)) ORDER BY t, id", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        sqlite3_bind_int64(st, 2, id);
        sqlite3_bind_int64(st, 3, t0);
        sqlite3_bind_int64(st, 4, t1 + 60);
        sqlite3_bind_int64(st, 5, id);
        sqlite3_bind_int64(st, 6, id);
        for (int n = 0; sqlite3_step(st) == SQLITE_ROW; n++) {
            fprintf(f, "%s\n", n ? "," : "");
            evidence_row(f, st);
        }
        sqlite3_finalize(st);
    }
    /* what happened meanwhile, a minute either side: on its channel or its
     * members' channels, and on the server */
    fprintf(f, "], \"events\": [");
    if (sqlite3_prepare_v2(db,
            "SELECT e.t, c.name, e.level, e.kind, e.text FROM events e LEFT JOIN chans c ON c.id = e.cid"
            " WHERE e.t >= ? AND e.t <= ? AND (e.cid IS NULL OR c.name IN"
            " (SELECT chan FROM incidents WHERE (id = ? OR parent = ?) AND chan IS NOT NULL))"
            " ORDER BY e.t, e.id LIMIT ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 5, report_mode ? 80 : 400);
        sqlite3_bind_int64(st, 1, t0 - 60);
        sqlite3_bind_int64(st, 2, t1 + 60);
        sqlite3_bind_int64(st, 3, id);
        sqlite3_bind_int64(st, 4, id);
        for (int n = 0; sqlite3_step(st) == SQLITE_ROW; n++) {
            fprintf(f, "%s\n{\"t\": %lld, \"channel\": ", n ? "," : "", (long long)sqlite3_column_int64(st, 0));
            if (sqlite3_column_type(st, 1) == SQLITE_NULL)
                fprintf(f, "null");
            else
                jstr(f, (const char *)sqlite3_column_text(st, 1));
            fprintf(f, ", \"level\": ");
            jstr(f, (const char *)sqlite3_column_text(st, 2));
            fprintf(f, ", \"kind\": ");
            jstr(f, (const char *)sqlite3_column_text(st, 3));
            fprintf(f, ", \"text\": ");
            jstr(f, (const char *)sqlite3_column_text(st, 4));
            fputc('}', f);
        }
        sqlite3_finalize(st);
    }
    /* what came and went on its channels (records: CC errors, short drops) from
     * 15 minutes before it to its end: context, never incidents of their own */
    fprintf(f, "], \"records\": [");
    if (sqlite3_prepare_v2(db,
            "SELECT t, closed, chan, rule, summary FROM records WHERE t <= ? AND closed >= ? AND chan IN"
            " (SELECT chan FROM incidents WHERE (id = ? OR parent = ?) AND chan IS NOT NULL)"
            " ORDER BY t, id LIMIT ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, t1 + 60);
        sqlite3_bind_int64(st, 2, t0 - 900);
        sqlite3_bind_int64(st, 3, id);
        sqlite3_bind_int64(st, 4, id);
        sqlite3_bind_int(st, 5, report_mode ? 40 : 200);
        for (int n = 0; sqlite3_step(st) == SQLITE_ROW; n++) {
            fprintf(f, "%s\n{\"t\": %lld, \"closed\": %lld, \"channel\": ", n ? "," : "",
                    (long long)sqlite3_column_int64(st, 0), (long long)sqlite3_column_int64(st, 1));
            jstr(f, (const char *)sqlite3_column_text(st, 2));
            fprintf(f, ", \"rule\": ");
            jstr(f, (const char *)sqlite3_column_text(st, 3));
            fprintf(f, ", \"summary\": ");
            jstr(f, (const char *)sqlite3_column_text(st, 4));
            fputc('}', f);
        }
        sqlite3_finalize(st);
    }
    fprintf(f, "]}");
    sqlite3_close(db);
    return true;
}

void history_info(FILE *f)
{
    if (!H.on) {
        fprintf(f, "null");
        return;
    }
    fprintf(f, "{\"path\": ");
    jstr(f, H.path);
    fprintf(f, ", \"bytes\": %lld, \"cap_bytes\": %lld, \"queued_dropped\": %d}", (long long)db_bytes(),
            (long long)H.cap, H.dropped);
}

#endif
