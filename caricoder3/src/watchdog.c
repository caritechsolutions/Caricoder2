/*
 * caricoder3 - the watchdog: rules and incidents
 * Copyright (c) 2026 CariTech Solutions
 */
#include "watchdog.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "history.h"
#include "json.h"
#include "log.h"

static const struct {
    const char *id, *name, *level;
    bool upstream;          /* a source problem when several channels of it have it */
} RULES[WD_RULES] = {
    [WD_NO_INPUT]    = { "no_input", "no input", "error", true },
    [WD_CC]          = { "cc_errors", "CC errors", "warn", true },
    [WD_LOSS]        = { "loss", "packets lost on the way in", "warn", false },
    [WD_DROPPING]    = { "dropping", "dropped or late pictures", "warn", false },
    [WD_OUT_STALLED] = { "output_stalled", "output stalled", "error", false },
    [WD_RESTARTS]    = { "restarts", "restarting again and again", "error", false },
    [WD_FAILED]      = { "failed", "channel failed", "error", false },
    [WD_BACKUP]      = { "backup", "running on the backup source", "warn", true },
    [WD_CLOCK]       = { "clock", "clock not locked", "warn", true },
    [WD_BLACK]       = { "black", "black picture", "error", true },
    [WD_FROZEN]      = { "frozen", "frozen picture", "error", true },
    [WD_SILENT]      = { "silent", "silent audio", "error", true },
    [WD_NO_DATA]     = { "no_data", "stream without data", "error", true },
    [WD_PIDS]        = { "pids_changed", "source PIDs changed", "warn", true },
    [WD_CPU]         = { "cpu", "CPU busy", "warn", false },
    [WD_GPU]         = { "gpu", "video engine busy", "warn", false },
    [WD_NIC_DROP]    = { "nic_drop", "network card dropping packets", "warn", false },
    [WD_DISK_LOW]    = { "disk_low", "disk space low", "warn", false },
    [WD_DISK_CRITICAL] = { "disk_critical", "disk nearly full", "error", false },
    [WD_DISK_FILLING] = { "disk_filling", "disk filling up", "warn", false },
    [WD_AV_SHIFT]    = { "av_shift", "audio timing moved (sync)", "error", true },
};

#define ENTRIES_MAX 2048
#define GROUPS_MAX  256

struct entry {
    bool used;
    char subject[80];
    char chan[64];          /* "" = the server */
    char group[512];
    enum wd_rule rule;
    bool on;
    bool instant;
    int64_t since, last_on;
    int64_t id;             /* > 0: the incident is open */
    int64_t parent;
    char what[160];
};

struct group {
    bool used;
    char group[512];
    enum wd_rule rule;
    int64_t id, since, last_close;
    int members;            /* open ones */
    char names[400];        /* every channel that has been in it */
    int n_names;
};

static struct {
    pthread_mutex_t lock;
    struct wd_limits lim;
    bool have_lim;
    struct entry e[ENTRIES_MAX];
    struct group g[GROUPS_MAX];
    void (*snap)(FILE *f, const char *chan, void *op);
    void *snap_op;
    void (*on_open)(int64_t id, const char *chan, const char *group, enum wd_rule r, void *op);
    void *open_op;
} W = { .lock = PTHREAD_MUTEX_INITIALIZER };

void wd_defaults(struct wd_limits *l)
{
    memset(l, 0, sizeof(*l));
    l->min_s = 10;
    l->clear_s = 30;
    l->group_s = 60;
    l->no_input_s = 5;
    l->cc_per_min = 10;
    l->dropped_per_min = 5;
    l->restarts = 3;
    l->restart_window_s = 600;
    l->cpu_pct = 90;
    l->gpu_pct = 90;
    l->load_s = 120;
    l->disk_warn_pct = 15;
    l->disk_crit_pct = 5;
    l->disk_full_h = 24;
    l->content_s = 30;
    l->av_shift_ms = 80;
    l->av_shift_s = 30;
}

const char *wd_rule_id(enum wd_rule r)
{
    return r < WD_RULES ? RULES[r].id : "?";
}

const char *wd_rule_name(enum wd_rule r)
{
    return r < WD_RULES ? RULES[r].name : "?";
}

int wd_rule_by_id(const char *id)
{
    for (int r = 0; r < WD_RULES; r++)
        if (!strcmp(RULES[r].id, id))
            return r;
    return -1;
}

#define LIMIT_FIELDS(X) X(min_s) X(content_s) X(clear_s) X(group_s) X(no_input_s) X(cc_per_min) X(dropped_per_min) \
    X(restarts) X(restart_window_s) X(cpu_pct) X(gpu_pct) X(load_s) X(disk_warn_pct) X(disk_crit_pct) X(disk_full_h) \
    X(av_shift_ms) X(av_shift_s)

void wd_limits_write(FILE *f, const struct wd_limits *l)
{
    int n = 0;

    fputc('{', f);
#define W_FIELD(k) fprintf(f, "\"%s\": %d, ", #k, l->k);
    LIMIT_FIELDS(W_FIELD)
#undef W_FIELD
    fprintf(f, "\"off\": [");
    for (int r = 0; r < WD_RULES; r++)
        if (l->off[r])
            fprintf(f, "%s\"%s\"", n++ ? ", " : "", RULES[r].id);
    fprintf(f, "]}");
}

void wd_limits_read(const struct json_doc *d, int obj, struct wd_limits *l)
{
    int a;

    if (obj < 0)
        return;
#define R_FIELD(k) { double v = json_get_num(d, obj, #k, -1); if (v >= 0 && v <= 1e7) l->k = (int)v; }
    LIMIT_FIELDS(R_FIELD)
#undef R_FIELD
    if ((a = json_get(d, obj, "off")) >= 0 && d->node[a].type == JSON_ARRAY) {
        memset(l->off, 0, sizeof(l->off));
        for (int j = d->node[a].first_child; j >= 0; j = d->node[j].next) {
            int r = d->node[j].type == JSON_STRING ? wd_rule_by_id(d->node[j].str) : -1;

            if (r >= 0)
                l->off[r] = true;
        }
    }
    if (l->min_s < 1)
        l->min_s = 1;
    if (l->clear_s < 1)
        l->clear_s = 1;
    if (l->content_s < 1)
        l->content_s = 1;
}

static void ensure_lim(void)
{
    if (!W.have_lim) {
        wd_defaults(&W.lim);
        W.have_lim = true;
    }
}

void wd_set_limits(const struct wd_limits *l)
{
    pthread_mutex_lock(&W.lock);
    W.lim = *l;
    W.have_lim = true;
    pthread_mutex_unlock(&W.lock);
}

void wd_get_limits(struct wd_limits *l)
{
    pthread_mutex_lock(&W.lock);
    ensure_lim();
    *l = W.lim;
    pthread_mutex_unlock(&W.lock);
}

void wd_set_on_open(void (*fn)(int64_t id, const char *chan, const char *group, enum wd_rule r, void *op), void *op)
{
    W.on_open = fn;
    W.open_op = op;
}

void wd_set_snapshot(void (*fn)(FILE *f, const char *chan, void *op), void *op)
{
    W.snap = fn;
    W.snap_op = op;
}

static int min_for(enum wd_rule r)
{
    if (r == WD_CPU || r == WD_GPU)
        return W.lim.load_s;
    if (r == WD_PIDS)
        return 0;
    if (r == WD_AV_SHIFT)
        return W.lim.av_shift_s;
    if (r == WD_BLACK || r == WD_FROZEN || r == WD_SILENT)
        return W.lim.content_s;
    return W.lim.min_s;
}

static struct entry *find(const char *subject, enum wd_rule r)
{
    for (int i = 0; i < ENTRIES_MAX; i++)
        if (W.e[i].used && W.e[i].rule == r && !strcmp(W.e[i].subject, subject))
            return &W.e[i];
    return NULL;
}

static struct entry *add(const char *subject, const char *chan, const char *group, enum wd_rule r)
{
    for (int i = 0; i < ENTRIES_MAX; i++)
        if (!W.e[i].used) {
            struct entry *e = &W.e[i];

            memset(e, 0, sizeof(*e));
            e->used = true;
            snprintf(e->subject, sizeof(e->subject), "%s", subject);
            snprintf(e->chan, sizeof(e->chan), "%s", chan ? chan : "");
            snprintf(e->group, sizeof(e->group), "%s", group ? group : "");
            e->rule = r;
            return e;
        }
    return NULL;
}

void wd_observe(int64_t t, const char *subject, const char *chan, const char *group, enum wd_rule r,
                bool on, const char *what, int64_t began)
{
    struct entry *e;

    if (r >= WD_RULES)
        return;
    pthread_mutex_lock(&W.lock);
    ensure_lim();
    if (W.lim.off[r])
        on = false;
    e = find(subject, r);
    if (on) {
        if (!e)
            e = add(subject, chan, group, r);
        if (e) {
            if (!e->on && !e->id)
                e->since = began > 0 && began < t ? began : t;
            e->on = true;
            e->last_on = t;
            if (what && *what)
                snprintf(e->what, sizeof(e->what), "%s", what);
            if (group && *group && strcmp(e->group, group))
                snprintf(e->group, sizeof(e->group), "%s", group);
        }
    } else if (e) {
        e->on = false;
        if (!e->id)
            e->used = false;            /* too short to be an incident */
    }
    pthread_mutex_unlock(&W.lock);
}

void wd_instant(int64_t t, const char *chan, const char *group, enum wd_rule r, const char *what)
{
    struct entry *e;

    pthread_mutex_lock(&W.lock);
    ensure_lim();
    if (r < WD_RULES && !W.lim.off[r] && (e = add(chan, chan, group, r))) {
        e->instant = true;
        e->on = false;
        e->since = e->last_on = t;
        snprintf(e->what, sizeof(e->what), "%s", what ? what : "");
    }
    pthread_mutex_unlock(&W.lock);
}

static char *snapshot(const struct entry *e)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);

    if (!f)
        return NULL;
    fprintf(f, "{\"what\": \"");
    for (const char *p = e->what; *p; p++)
        if (*p == '"' || *p == '\\')
            fprintf(f, "\\%c", *p);
        else if ((unsigned char)*p >= 0x20)
            fputc(*p, f);
    fprintf(f, "\", \"source\": \"");
    for (const char *p = e->group; *p; p++)
        if (*p == '"' || *p == '\\')
            fprintf(f, "\\%c", *p);
        else if ((unsigned char)*p >= 0x20)
            fputc(*p, f);
    fprintf(f, "\"");
    if (W.snap) {
        fprintf(f, ", \"then\": ");
        W.snap(f, e->chan[0] ? e->chan : NULL, W.snap_op);
    }
    fprintf(f, "}");
    fclose(f);
    return buf;
}

static void summary_of(const struct entry *e, char *out, size_t len)
{
    if (e->what[0])
        snprintf(out, len, "%s", e->what);
    else
        snprintf(out, len, "%s", RULES[e->rule].name);
}

static void open_entry(struct entry *e, int64_t t)
{
    char sum[200];
    char *det;

    (void)t;
    summary_of(e, sum, sizeof(sum));
    det = snapshot(e);
    e->id = history_incident_open(e->since, e->chan[0] ? e->chan : NULL, RULES[e->rule].id, RULES[e->rule].level,
                                  e->group, sum, det, 0);
    free(det);
    if (!e->id)
        e->id = -1;                 /* no record kept: still followed, for the state */
    history_event(e->since, e->chan[0] ? e->chan : NULL, RULES[e->rule].level, "incident", sum);
    log_info("incident: %s%s%s", e->chan[0] ? e->chan : "server", ": ", sum);
    if (W.on_open && e->id > 0)
        W.on_open(e->id, e->chan[0] ? e->chan : NULL, e->group, e->rule, W.open_op);
}

static void close_entry(struct entry *e, int64_t t)
{
    char sum[200], text[260];

    (void)t;
    summary_of(e, sum, sizeof(sum));
    if (e->id > 0)
        history_incident_update(e->id, e->last_on > e->since ? e->last_on : e->since, NULL, NULL, e->parent);
    snprintf(text, sizeof(text), "over: %s (%lld s)", sum, (long long)(e->last_on - e->since));
    history_event(e->last_on, e->chan[0] ? e->chan : NULL, "info", "incident", text);
    for (int k = 0; k < GROUPS_MAX; k++)
        if (W.g[k].used && W.g[k].id == e->parent && e->parent) {
            W.g[k].members--;
            if (e->last_on > W.g[k].last_close)
                W.g[k].last_close = e->last_on;
        }
    e->used = false;
}

static struct group *group_for(const char *grp, enum wd_rule r)
{
    for (int k = 0; k < GROUPS_MAX; k++)
        if (W.g[k].used && W.g[k].rule == r && !strcmp(W.g[k].group, grp))
            return &W.g[k];
    return NULL;
}

static void group_summary(const struct group *g, char *out, size_t len)
{
    snprintf(out, len, "%s on %d channels of %.150s: %.300s", RULES[g->rule].name, g->n_names, g->group, g->names);
}

static void group_add_name(struct group *g, const char *name)
{
    size_t n = strlen(g->names);

    if (n + strlen(name) + 3 < sizeof(g->names))
        snprintf(g->names + n, sizeof(g->names) - n, "%s%s", n ? ", " : "", name);
    g->n_names++;
}

static void groups(int64_t t)
{
    for (int i = 0; i < ENTRIES_MAX; i++) {
        struct entry *e = &W.e[i];
        struct group *g;
        char sum[600];

        if (!e->used || e->id == 0 || e->parent || !RULES[e->rule].upstream || !e->group[0] || !e->chan[0])
            continue;
        g = group_for(e->group, e->rule);
        if (!g) {
            /* other channels of this source with it, begun near the same time? */
            int with[ENTRIES_MAX / 8], n = 0;
            int64_t since = e->since;

            for (int j = 0; j < ENTRIES_MAX && n < (int)(sizeof(with) / sizeof(with[0])); j++)
                if (j != i && W.e[j].used && W.e[j].id && !W.e[j].parent && W.e[j].rule == e->rule && W.e[j].chan[0] &&
                    !strcmp(W.e[j].group, e->group) && strcmp(W.e[j].chan, e->chan) &&
                    llabs(W.e[j].since - e->since) <= W.lim.group_s)
                    with[n++] = j;
            if (!n)
                continue;
            for (int k = 0; k < GROUPS_MAX && !g; k++)
                if (!W.g[k].used)
                    g = &W.g[k];
            if (!g)
                continue;
            memset(g, 0, sizeof(*g));
            g->used = true;
            snprintf(g->group, sizeof(g->group), "%s", e->group);
            g->rule = e->rule;
            group_add_name(g, e->chan);
            for (int k = 0; k < n; k++) {
                group_add_name(g, W.e[with[k]].chan);
                if (W.e[with[k]].since < since)
                    since = W.e[with[k]].since;
            }
            g->since = since;
            group_summary(g, sum, sizeof(sum));
            g->id = history_incident_open(g->since, NULL, RULES[g->rule].id, RULES[g->rule].level, g->group,
                                          sum, "{}", 0);
            if (g->id <= 0)
                g->id = -2;
            history_event(g->since, NULL, RULES[g->rule].level, "incident", sum);
            log_info("incident: %s", sum);
            if (W.on_open && g->id > 0)
                W.on_open(g->id, NULL, g->group, g->rule, W.open_op);
            for (int k = -1; k < n; k++) {
                struct entry *m = k < 0 ? e : &W.e[with[k]];

                m->parent = g->id;
                g->members++;
                if (m->id > 0 && g->id > 0)
                    history_incident_update(m->id, 0, NULL, NULL, g->id);
            }
            continue;
        }
        /* a further channel joins the source's incident */
        e->parent = g->id;
        g->members++;
        group_add_name(g, e->chan);
        group_summary(g, sum, sizeof(sum));
        if (g->id > 0) {
            history_incident_update(g->id, 0, sum, NULL, 0);
            if (e->id > 0)
                history_incident_update(e->id, 0, NULL, NULL, g->id);
        }
    }
    /* groups with nobody left in them are over */
    for (int k = 0; k < GROUPS_MAX; k++) {
        struct group *g = &W.g[k];

        if (!g->used || g->members > 0)
            continue;
        if (g->id > 0)
            history_incident_update(g->id, g->last_close ? g->last_close : t, NULL, NULL, 0);
        g->used = false;
    }
}

void wd_tick(int64_t t)
{
    pthread_mutex_lock(&W.lock);
    ensure_lim();
    for (int i = 0; i < ENTRIES_MAX; i++) {
        struct entry *e = &W.e[i];

        if (!e->used)
            continue;
        if (e->on && t - e->last_on > 30) {
            e->on = false;                  /* nobody says any more (the channel went) */
            if (!e->id) {
                e->used = false;
                continue;
            }
        }
        /* long enough by what was seen (not by the clock alone: the last
         * sample may be a couple of seconds old) */
        if (!e->id && (e->instant || (e->on && e->last_on - e->since >= min_for(e->rule))))
            open_entry(e, t);
    }
    groups(t);
    for (int i = 0; i < ENTRIES_MAX; i++) {
        struct entry *e = &W.e[i];

        if (e->used && e->id && !e->on && t - e->last_on >= W.lim.clear_s)
            close_entry(e, t);
    }
    groups(t);
    pthread_mutex_unlock(&W.lock);
}

void wd_forget(int64_t t, const char *subject)
{
    pthread_mutex_lock(&W.lock);
    for (int i = 0; i < ENTRIES_MAX; i++) {
        struct entry *e = &W.e[i];

        if (!e->used || strcmp(e->subject, subject))
            continue;
        if (e->id) {
            if (e->on)
                e->last_on = t;
            e->on = false;
            close_entry(e, t);
        } else {
            e->used = false;
        }
    }
    groups(t);
    pthread_mutex_unlock(&W.lock);
}

static void jesc(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++)
        if (*s == '"' || *s == '\\')
            fprintf(f, "\\%c", *s);
        else if ((unsigned char)*s >= 0x20)
            fputc(*s, f);
    fputc('"', f);
}

void wd_state_json(FILE *f, int64_t t)
{
    int open = 0, pending = 0, n = 0;

    pthread_mutex_lock(&W.lock);
    fprintf(f, "{\"items\": [");
    for (int i = 0; i < ENTRIES_MAX; i++) {
        const struct entry *e = &W.e[i];

        if (!e->used)
            continue;
        if (e->id)
            open++;
        else
            pending++;
        fprintf(f, "%s{\"subject\": ", n++ ? ", " : "");
        jesc(f, e->subject);
        fprintf(f, ", \"rule\": \"%s\", \"since\": %lld, \"for_s\": %lld, \"incident\": %lld, \"on\": %s, \"what\": ",
                RULES[e->rule].id, (long long)e->since, (long long)(t - e->since), (long long)(e->id > 0 ? e->id : 0),
                e->on ? "true" : "false");
        jesc(f, e->what);
        fputc('}', f);
    }
    fprintf(f, "], \"open\": %d, \"pending\": %d}", open, pending);
    pthread_mutex_unlock(&W.lock);
}
