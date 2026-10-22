/*
 * caricoder3 - the IRDs (satellite receivers): ird.h
 * Copyright (c) 2026 CariTech Solutions
 */
#include "ird.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "json.h"
#include "log.h"
#include "history.h"
#include "peer.h"
#include "watchdog.h"
#include "ird_upcom.h"

#define SAMPLES 1440                    /* a day of minutes */
#define PROGS_MAX 64

struct ird {
    struct ird_state st;
    float samples[SAMPLES];             /* C/N while locked */
    int n_samples, next_sample;
    int out_progs[IRD_OUTS][PROGS_MAX]; /* the services in each output at the last comparison */
    int n_out_progs[IRD_OUTS];
    bool out_known[IRD_OUTS];
    struct upcom_tables *prev;          /* each input's tables at the last read (16, by source number) */
    bool prev_known[16];
};

static struct {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    pthread_t thread;
    bool started, stop, now;
    struct ird ird[IRDS_MAX];
    int n;
    struct ird_opts o;
    char binary[512];
    bool (*uses)(const char *url, int program, int pid, void *op);
    void *uses_op;
} I = { .lock = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER };

/* ---- reading an Upcom's tuner ---------------------------------------------------- */

/* "192.168.110.10" -> "http://192.168.110.10:80" (the HTTP client's own
 * default port is the peers', 8550) */
static void ird_base_url(const char *address, char *url, size_t len)
{
    const char *h = !strncmp(address, "http://", 7) ? address + 7 : address;
    size_t hl = strcspn(h, "/");

    snprintf(url, len, "http://%.*s%s", (int)hl, h, memchr(h, ':', hl) ? "" : ":80");
}

static double num(const char *s)
{
    return s && *s ? atof(s) : NAN;
}

/* "1.2 E-5" */
static double sci(const char *s)
{
    char *end;
    double m;
    const char *e;

    if (!s || !*s)
        return NAN;
    m = strtod(s, &end);
    if (end == s)
        return NAN;
    e = strpbrk(end, "Ee");
    return e ? m * pow(10, atoi(e + 1)) : m;
}

bool ird_parse_upcom(const char *body, struct ird_state *st)
{
    static struct json_doc d;           /* the IRD thread, or the tests */
    char err[100];
    int r, g, t;

    if (!body || json_parse(&d, body, err, sizeof(err)) < 0)
        return false;
    r = 0;
    if ((g = json_get(&d, r, "120")) < 0 || (t = json_get(&d, g, "0")) < 0)
        return false;
    st->locked = json_get_num(&d, t, "lock", 0) != 0;
    st->cn = num(json_get_str(&d, t, "CN", ""));
    st->ebn0 = num(json_get_str(&d, t, "EbN0", ""));
    st->ber = sci(json_get_str(&d, t, "ber", ""));
    st->strength = num(json_get_str(&d, t, "strength", ""));
    st->total_mbps = num(json_get_str(&d, t, "totalBR", ""));
    st->valid_mbps = num(json_get_str(&d, t, "validBR", ""));
    st->have = true;
    return true;
}

static int cmpf(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;

    return x < y ? -1 : x > y;
}

static double median(const struct ird *d)
{
    static float tmp[SAMPLES];

    memcpy(tmp, d->samples, sizeof(float) * (size_t)d->n_samples);
    qsort(tmp, (size_t)d->n_samples, sizeof(float), cmpf);
    return tmp[d->n_samples / 2];
}

/* One poll of IRD k; called without the lock (the request takes time). */
static void poll_one(int k)
{
    struct ird_conf c;
    struct ird_state got;
    char url[300], *out = NULL;
    int status;
    int64_t t = time(NULL);
    struct wd_limits L;

    pthread_mutex_lock(&I.lock);
    c = I.ird[k].st.c;
    pthread_mutex_unlock(&I.lock);
    memset(&got, 0, sizeof(got));
    ird_base_url(c.address, url, sizeof(url));
    status = http_call(url, "POST", "/goform/subBoard.cgi", "application/x-www-form-urlencoded; charset=UTF-8", NULL,
                       "json=%7B%22120%22%3A%7B%22method%22%3A0%2C%22data%22%3A%7B%220000%22%3A%7B%7D%7D%7D%7D",
                       5000, &out);
    if (status == 200 && !ird_parse_upcom(out, &got)) {
        status = -2;
    }
    wd_get_limits(&L);
    pthread_mutex_lock(&I.lock);
    {
        struct ird *d = &I.ird[k];
        struct ird_state *st = &d->st;

        if (strcmp(st->c.name, c.name)) {        /* the list changed meanwhile */
            pthread_mutex_unlock(&I.lock);
            free(out);
            return;
        }
        st->last_try = t;
        if (status == 200) {
            st->answering = true;
            st->fails = 0;
            st->last_ok = t;
            st->error[0] = 0;
            st->have = true;
            st->locked = got.locked;
            st->cn = got.cn;
            st->ebn0 = got.ebn0;
            st->ber = got.ber;
            st->strength = got.strength;
            st->total_mbps = got.total_mbps;
            st->valid_mbps = got.valid_mbps;
            if (st->locked && !isnan(st->cn)) {
                /* below the normal: judged against the normal before this reading */
                bool below = st->cn_normal >= 0 && st->cn < st->cn_normal - L.ird_cn_drop_db;

                st->low = below ? st->low + 1 : 0;
                d->samples[d->next_sample] = (float)st->cn;
                d->next_sample = (d->next_sample + 1) % SAMPLES;
                if (d->n_samples < SAMPLES)
                    d->n_samples++;
                st->n_samples = d->n_samples;
                if (d->n_samples >= (I.o.baseline_n > 0 ? I.o.baseline_n : 30))
                    st->cn_normal = median(d);
            } else {
                st->low = 0;
            }
        } else {
            st->answering = false;
            st->fails++;
            st->have = false;
            st->low = 0;
            snprintf(st->error, sizeof(st->error), "%s",
                     status == -2 ? "the answer is not an Upcom tuner's" : status < 0 && out ? out
                     : status > 0 ? "an HTTP error" : "no answer");
            if (status > 0)
                snprintf(st->error, sizeof(st->error), "HTTP %d", status);
        }
    }
    {
        struct ird_state st = I.ird[k].st;

        pthread_mutex_unlock(&I.lock);
        if (st.answering && history_enabled()) {
            char m[96];

#define MET(key, v) do { if (!isnan(v)) { snprintf(m, sizeof(m), "ird.%s." key, st.c.name); history_metric(t, m, v); } } while (0)
            MET("cn", st.cn);
            MET("ebn0", st.ebn0);
            MET("ber", st.ber);
            MET("strength", st.strength);
            MET("total_mbps", st.total_mbps);
            MET("lock", st.locked ? 1.0 : 0.0);
#undef MET
        }
    }
    free(out);
}

/* ---- an Upcom's own tables ------------------------------------------------------- */

static char *upcom_ask(const char *base, int form, int key, char *err, size_t errlen)
{
    char body[200], *out = NULL;
    int status;

    /* json={"F":{"method":0,"data":{"K":{}}}} */
    snprintf(body, sizeof(body), "json=%%7B%%22%d%%22%%3A%%7B%%22method%%22%%3A0%%2C%%22data%%22%%3A%%7B%%22%d%%22%%3A%%7B%%7D%%7D%%7D%%7D",
             form, key);
    status = http_call(base, "POST", "/goform/remux.cgi", "application/x-www-form-urlencoded; charset=UTF-8", NULL, body,
                       8000, &out);
    if (status != 200) {
        snprintf(err, errlen, "its tables: %s", status < 0 && out ? out : "an HTTP error");
        free(out);
        return NULL;
    }
    return out;
}

static void add_text(char *dst, size_t len, const char *fmt, const char *name, int n, const char *rest)
{
    size_t at = strlen(dst);

    if (at + 30 >= len) {
        if (at + 6 < len && (at < 3 || strcmp(dst + at - 3, "...")))
            snprintf(dst + at, len - at, "; ...");
        return;
    }
    snprintf(dst + at, len - at, "%s%s (%d)", at ? "; " : "", name[0] ? name : "program", n);
    at = strlen(dst);
    snprintf(dst + at, len - at, fmt, rest);
}

/* One read of IRD k's tables; without the lock. */
static void tables_one(int k)
{
    static struct upcom_tables remux, src[16];
    bool have[16] = { false }, said[UPCOM_SVC_MAX * 2] = { false };
    int said_n[UPCOM_SVC_MAX * 2], n_said = 0;
    struct ird_conf c;
    char url[300], err[160] = "", what[480] = "", labels[480] = "", w[600];
    char *body;
    int services = 0, diffs = 0;
    int64_t t = time(NULL);
    struct { char text[300]; } changes[8];
    int n_changes = 0;

    pthread_mutex_lock(&I.lock);
    c = I.ird[k].st.c;
    pthread_mutex_unlock(&I.lock);
    if (strcmp(c.kind, "upcom"))
        return;
    ird_base_url(c.address, url, sizeof(url));
    body = upcom_ask(url, 8, 0, err, sizeof(err));
    if (body && !upcom_parse(body, &remux))
        snprintf(err, sizeof(err), "its remux has no tables");
    free(body);
    if (!err[0]) {
        for (int i = 0; i < remux.n_src; i++) {
            int s = remux.src[i][0];

            if (s < 0 || s >= 16 || have[s])
                continue;
            body = upcom_ask(url, 3, s, err, sizeof(err));
            if (!body)
                break;
            have[s] = upcom_parse(body, &src[s]);
            free(body);
        }
    }
    if (!err[0]) {
        for (int i = 0; i < remux.n; i++) {
            const struct upcom_svc *o = &remux.svc[i], *in;
            int s = upcom_source_of(&remux, o->n);

            in = s >= 0 && s < 16 && have[s] ? upcom_find(&src[s], o->n) : NULL;
            if (!in) {
                char sv[300];

                upcom_streams_text(o, sv, sizeof(sv));
                add_text(what, sizeof(what), " is in the remux (%s) but not on the satellite", o->name, o->n, sv);
                diffs++;
                continue;
            }
            services++;
            if (!upcom_same(&remux, o, s, in, w, sizeof(w))) {
                add_text(what, sizeof(what), ": %s", o->name, o->n, w);
                diffs++;
            } else if (upcom_labels_differ(o, in, w, sizeof(w))) {
                add_text(labels, sizeof(labels), ": %s", o->name, o->n, w);
            }
        }
    }
    pthread_mutex_lock(&I.lock);
    {
        struct ird *d = &I.ird[k];

        if (!strcmp(d->st.c.name, c.name)) {
            if (!err[0]) {
                /* what changed on the satellite since the last read: each service once */
                if (!d->prev)
                    d->prev = calloc(16, sizeof(*d->prev));
                for (int s = 0; d->prev && s < 16; s++) {
                    if (!have[s])
                        continue;
                    if (d->prev_known[s]) {
                        for (int i = 0; i < src[s].n; i++) {
                            const struct upcom_svc *now = &src[s].svc[i], *was = upcom_find(&d->prev[s], now->n);
                            bool done = false;

                            for (int j = 0; j < n_said; j++)
                                done |= said_n[j] == now->n && said[j];
                            if (done || !was || !upcom_changed(was, now, w, sizeof(w)) || n_changes == 8)
                                continue;
                            snprintf(changes[n_changes++].text, sizeof(changes[0].text),
                                     "IRD %.30s: %.40s (%d) changed on the satellite: %.180s", c.name,
                                     now->name[0] ? now->name : "service", now->n, w);
                            if (n_said < UPCOM_SVC_MAX * 2) {
                                said_n[n_said] = now->n;
                                said[n_said++] = true;
                            }
                        }
                    }
                    d->prev[s] = src[s];
                    d->prev_known[s] = true;
                }
                d->st.tables_bad = diffs ? d->st.tables_bad + 1 : 0;
                d->st.tables_services = services;
                snprintf(d->st.tables_what, sizeof(d->st.tables_what), "%s", what);
                snprintf(d->st.tables_labels, sizeof(d->st.tables_labels), "%s", labels);
            }
            d->st.tables_t = t;
            snprintf(d->st.tables_error, sizeof(d->st.tables_error), "%s", err);
        }
    }
    pthread_mutex_unlock(&I.lock);
    for (int i = 0; i < n_changes; i++) {
        char subj[96];

        snprintf(subj, sizeof(subj), "ird/%.40s/satellite", c.name);
        wd_instant_server(t, subj, WD_SAT_CHANGED, changes[i].text);
        log_info("%s", changes[i].text);
    }
}

/* ---- comparing what comes down with what is sent ---------------------------------- */

/* Runs `binary probe URL --seconds S --brief` for each url at once; the
 * outputs, malloc'd ("" when one gave nothing). */
static void probe_many(const char *const *urls, int n, int seconds, char **outs)
{
    pid_t pid[IRD_OUTS + 1];
    int fd[IRD_OUTS + 1];
    size_t len[IRD_OUTS + 1] = { 0 }, cap[IRD_OUTS + 1] = { 0 };
    int64_t deadline = time(NULL) + seconds + 20;
    char secs[16];

    snprintf(secs, sizeof(secs), "%d", seconds);
    for (int i = 0; i < n; i++) {
        int p[2];

        outs[i] = NULL;
        pid[i] = -1;
        fd[i] = -1;
        if (pipe2(p, O_CLOEXEC) < 0)
            continue;
        pid[i] = fork();
        if (pid[i] == 0) {
            int nul = open("/dev/null", O_WRONLY);

            dup2(p[1], 1);
            if (nul >= 0)
                dup2(nul, 2);
            execl(I.binary, I.binary, "probe", urls[i], "--seconds", secs, "--brief", (char *)NULL);
            _exit(127);
        }
        close(p[1]);
        if (pid[i] < 0) {
            close(p[0]);
            continue;
        }
        fd[i] = p[0];
    }
    for (;;) {
        struct pollfd pf[IRD_OUTS + 1];
        int m = 0, map[IRD_OUTS + 1];
        int64_t left = deadline - time(NULL);

        for (int i = 0; i < n; i++)
            if (fd[i] >= 0) {
                pf[m].fd = fd[i];
                pf[m].events = POLLIN;
                map[m++] = i;
            }
        if (!m || left <= 0 || I.stop)
            break;
        if (poll(pf, (nfds_t)m, 500) <= 0)
            continue;
        for (int j = 0; j < m; j++) {
            int i = map[j];
            ssize_t r;

            if (!(pf[j].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            if (len[i] + 4097 > cap[i]) {
                size_t nc = cap[i] ? cap[i] * 2 : 16384;
                char *nb = nc < (4u << 20) ? realloc(outs[i], nc) : NULL;

                if (!nb) {
                    close(fd[i]);
                    fd[i] = -1;
                    continue;
                }
                outs[i] = nb;
                cap[i] = nc;
            }
            r = read(fd[i], outs[i] + len[i], cap[i] - len[i] - 1);
            if (r > 0) {
                len[i] += (size_t)r;
            } else if (r == 0 || (errno != EINTR && errno != EAGAIN)) {
                close(fd[i]);
                fd[i] = -1;
            }
        }
    }
    for (int i = 0; i < n; i++) {
        if (fd[i] >= 0)
            close(fd[i]);
        if (pid[i] > 0) {
            kill(pid[i], SIGKILL);
            waitpid(pid[i], NULL, 0);
        }
        if (!outs[i])
            outs[i] = strdup("");
        else
            outs[i][len[i]] = 0;
    }
}

struct prog {
    int n;
    char name[64];
    bool scr;
    double br;
    int ns;
    struct { int pid, type; char codec[16]; double br; double scrambled; char lang[8]; } s[16];
};

static int read_progs(const char *json, struct prog *out, int max, char *err, size_t errlen)
{
    static struct json_doc d;           /* the IRD thread only */
    int root, a, n = 0;

    if (!json || !*json) {
        snprintf(err, errlen, "nothing from the probe");
        return -1;
    }
    if ((root = json_parse(&d, json, err, errlen)) < 0)
        return -1;
    if (!json_get_bool(&d, root, "ok", false)) {
        snprintf(err, errlen, "%s", json_get_str(&d, root, "error", "the probe failed"));
        return -1;
    }
    a = json_get(&d, root, "programs");
    for (int j = a >= 0 ? d.node[a].first_child : -1; j >= 0 && n < max; j = d.node[j].next) {
        struct prog *p = &out[n++];
        int st = json_get(&d, j, "st");

        memset(p, 0, sizeof(*p));
        p->n = (int)json_get_num(&d, j, "n", 0);
        snprintf(p->name, sizeof(p->name), "%s", json_get_str(&d, j, "name", ""));
        for (char *q = p->name; *q; q++)
            if ((unsigned char)*q < 32)
                memmove(q, q + 1, strlen(q));       /* the DVB character table byte */
        p->scr = json_get_bool(&d, j, "scr", false);
        p->br = json_get_num(&d, j, "br", 0);
        for (int k = st >= 0 ? d.node[st].first_child : -1; k >= 0 && p->ns < 16; k = d.node[k].next) {
            int f = d.node[k].first_child, idx = 0;

            for (; f >= 0; f = d.node[f].next, idx++) {
                const struct json_node *nd = &d.node[f];

                if (idx == 0) p->s[p->ns].pid = (int)nd->num;
                else if (idx == 1) p->s[p->ns].type = (int)nd->num;
                else if (idx == 2) snprintf(p->s[p->ns].codec, sizeof(p->s[0].codec), "%s", nd->str ? nd->str : "");
                else if (idx == 3) p->s[p->ns].br = nd->num;
                else if (idx == 4) p->s[p->ns].scrambled = nd->num;
                else if (idx == 5) snprintf(p->s[p->ns].lang, sizeof(p->s[0].lang), "%s", nd->str ? nd->str : "");
            }
            p->ns++;
        }
    }
    return n;
}

static const struct prog *find_prog(const struct prog *ps, int n, int number)
{
    for (int i = 0; i < n; i++)
        if (ps[i].n == number)
            return &ps[i];
    return NULL;
}

static void streams_text(const struct prog *p, char *out, size_t len)
{
    size_t at = 0;

    out[0] = 0;
    for (int i = 0; i < p->ns && at + 24 < len; i++)
        at += (size_t)snprintf(out + at, len - at, "%s%d %s%s", i ? ", " : "", p->s[i].pid,
                               p->s[i].codec[0] ? p->s[i].codec : "?", p->s[i].br < 1 ? " (no data)" : "");
    if (!p->ns)
        snprintf(out, len, "no streams");
}

static bool same_streams(const struct prog *a, const struct prog *b)
{
    if (a->ns != b->ns)
        return false;
    for (int i = 0; i < a->ns; i++) {
        bool found = false;

        for (int j = 0; j < b->ns; j++)
            if (a->s[i].pid == b->s[j].pid && !strcmp(a->s[i].codec, b->s[j].codec))
                found = true;
        if (!found)
            return false;
    }
    return true;
}

static void add_diff(char *what, size_t len, int *n, const char *fmt, const char *name, int number, const char *rest)
{
    size_t at = strlen(what);

    (*n)++;
    if (at + 40 >= len) {
        if (at + 5 < len && strcmp(what + at - 3, "..."))
            snprintf(what + at, len - at, "; ...");
        return;
    }
    snprintf(what + at, len - at, "%s", at ? "; " : "");
    at = strlen(what);
    if (name[0])
        snprintf(what + at, len - at, "%s (%d)", name, number);
    else
        snprintf(what + at, len - at, "program %d", number);
    at = strlen(what);
    snprintf(what + at, len - at, fmt, rest);
}

static const char *kind_of(const char *codec)
{
    static const char *const video[] = { "h264", "hevc", "mpeg2video", "mpeg1video", NULL };
    static const char *const audio[] = { "aac", "aac_latm", "ac3", "eac3", "mp2", "mp3", NULL };

    for (int i = 0; video[i]; i++)
        if (!strcmp(codec, video[i]))
            return "video";
    for (int i = 0; audio[i]; i++)
        if (!strcmp(codec, audio[i]))
            return "audio";
    return "stream";
}

/* "audio PID 2023 (labelled spa)" */
static void stream_words(const struct prog *p, int k, char *out, size_t len)
{
    snprintf(out, len, "%s PID %d%s%s%s", kind_of(p->s[k].codec), p->s[k].pid, p->s[k].lang[0] ? " (labelled " : "",
             p->s[k].lang, p->s[k].lang[0] ? ")" : "");
}

static bool used(const char *url, int program, int pid)
{
    bool (*fn)(const char *, int, int, void *) = I.uses;

    return !fn || !url || fn(url, program, pid, I.uses_op);       /* not known: as if it were */
}

int ird_compare(const char *raw_json, const char *out_json, const char *url, char *what, size_t len, char *notes,
                size_t nlen, int *services)
{
    static struct prog raw[PROGS_MAX], out[PROGS_MAX];
    char err[160];
    int nr = read_progs(raw_json, raw, PROGS_MAX, err, sizeof(err)), no, diffs = 0, nnotes = 0;

    what[0] = 0;
    notes[0] = 0;
    *services = 0;
    if (nr < 0) {
        snprintf(what, len, "the raw output: %s", err);
        return -1;
    }
    no = read_progs(out_json, out, PROGS_MAX, err, sizeof(err));
    if (no < 0) {
        snprintf(what, len, "the output: %s", err);
        return -1;
    }
    for (int i = 0; i < no; i++) {
        const struct prog *o = &out[i], *r = find_prog(raw, nr, o->n);
        char a[200], b[200];
        bool data = false;

        if (o->n == 0)
            continue;
        for (int k = 0; k < o->ns; k++)
            data |= o->s[k].br >= 1;
        if (!r) {
            if (data) {
                streams_text(o, a, sizeof(a));
                add_diff(what, len, &diffs, " is in the output but not on the satellite (%s)", o->name, o->n, a);
            }
            continue;                   /* an empty entry of the output's own: not a service */
        }
        (*services)++;
        if (!same_streams(o, r)) {
            char both[420];

            streams_text(o, a, sizeof(a));
            streams_text(r, b, sizeof(b));
            snprintf(both, sizeof(both), "output %s; satellite %s", a, b);
            add_diff(what, len, &diffs, ": streams differ: %s", o->name[0] ? o->name : r->name, o->n, both);
            continue;
        }
        {
            /* this many scrambled packets in a few seconds: not decrypted. Said
             * per stream, and whether our channels take it */
            int n_scr = 0;

            for (int k = 0; k < o->ns; k++)
                n_scr += o->s[k].scrambled > 20;
            if (n_scr && n_scr == o->ns) {
                bool any = false;

                for (int k = 0; k < o->ns; k++)
                    any |= used(url, o->n, o->s[k].pid);
                if (any)
                    add_diff(what, len, &diffs, ": the whole service is still scrambled in the output%s", o->name, o->n,
                             "");
                else
                    add_diff(notes, nlen, &nnotes, ": the whole service is still scrambled in the output%s", o->name,
                             o->n, " (none of our channels takes it)");
            } else {
                for (int k = 0; k < o->ns; k++) {
                    char sw[80], msg[200];

                    if (o->s[k].scrambled <= 20)
                        continue;
                    stream_words(o, k, sw, sizeof(sw));
                    if (used(url, o->n, o->s[k].pid)) {
                        snprintf(msg, sizeof(msg), "%s is still scrambled in the output; its other streams are clear", sw);
                        add_diff(what, len, &diffs, ": %s", o->name, o->n, msg);
                    } else {
                        snprintf(msg, sizeof(msg), "%s is still scrambled in the output; its other streams are clear; "
                                 "none of our channels takes it", sw);
                        add_diff(notes, nlen, &nnotes, ": %s", o->name, o->n, msg);
                    }
                }
            }
        }
        for (int k = 0; k < o->ns; k++) {
            for (int j = 0; j < r->ns; j++)
                if (r->s[j].pid == o->s[k].pid && r->s[j].br >= 8000 && o->s[k].br < 1) {
                    char sw[80], msg[200];

                    stream_words(o, k, sw, sizeof(sw));
                    snprintf(msg, sizeof(msg), "%s has data on the satellite but none in the output%s", sw,
                             used(url, o->n, o->s[k].pid) ? "" : "; none of our channels takes it");
                    if (used(url, o->n, o->s[k].pid))
                        add_diff(what, len, &diffs, ": %s", o->name, o->n, msg);
                    else
                        add_diff(notes, nlen, &nnotes, ": %s", o->name, o->n, msg);
                }
        }
    }
    return diffs;
}

/* One comparison of IRD k; without the lock. */
static void compare_one(int k)
{
    struct ird_conf c;
    const char *urls[IRD_OUTS + 1];
    char *outs[IRD_OUTS + 1];
    char what[480] = "", notes[480] = "", err[160] = "";
    int total = 0, services = 0, nurls;
    int left[IRD_OUTS][PROGS_MAX], n_left[IRD_OUTS] = { 0 };
    static struct prog now_progs[PROGS_MAX];
    int64_t t = time(NULL);

    pthread_mutex_lock(&I.lock);
    c = I.ird[k].st.c;
    pthread_mutex_unlock(&I.lock);
    if (!c.raw[0] || !c.n_out)
        return;
    urls[0] = c.raw;
    for (int i = 0; i < c.n_out; i++)
        urls[i + 1] = c.outputs[i];
    nurls = c.n_out + 1;
    probe_many(urls, nurls, I.o.probe_s > 0 ? I.o.probe_s : 5, outs);
    for (int i = 0; i < c.n_out; i++) {
        char w[480], nw[480];
        int sv = 0, d = ird_compare(outs[0], outs[i + 1], c.outputs[i], w, sizeof(w), nw, sizeof(nw), &sv);

        if (d < 0) {
            snprintf(err, sizeof(err), "%.150s", w);
            continue;
        }
        total += d;
        services += sv;
        if (d) {
            size_t at = strlen(what);

            snprintf(what + at, sizeof(what) - at, "%s%.100s: %.300s", at ? " | " : "",
                     c.n_out > 1 ? c.outputs[i] : "the output", w);
        }
        if (nw[0]) {
            size_t at = strlen(notes);

            snprintf(notes + at, sizeof(notes) - at, "%s%.100s: %.300s", at ? " | " : "",
                     c.n_out > 1 ? c.outputs[i] : "the output", nw);
        }
        /* which services it had last time and has not now */
        {
            char e2[100];
            int n = read_progs(outs[i + 1], now_progs, PROGS_MAX, e2, sizeof(e2));

            pthread_mutex_lock(&I.lock);
            {
                struct ird *dd = &I.ird[k];

                if (n >= 0 && !strcmp(dd->st.c.name, c.name)) {
                    if (dd->out_known[i])
                        for (int a = 0; a < dd->n_out_progs[i]; a++)
                            if (!find_prog(now_progs, n, dd->out_progs[i][a]) && n_left[i] < PROGS_MAX)
                                left[i][n_left[i]++] = dd->out_progs[i][a];
                    dd->n_out_progs[i] = 0;
                    for (int a = 0; a < n && a < PROGS_MAX; a++)
                        if (now_progs[a].n)
                            dd->out_progs[i][dd->n_out_progs[i]++] = now_progs[a].n;
                    dd->out_known[i] = true;
                }
            }
            pthread_mutex_unlock(&I.lock);
        }
    }
    for (int i = 0; i < nurls; i++)
        free(outs[i]);
    pthread_mutex_lock(&I.lock);
    {
        struct ird_state *st = &I.ird[k].st;

        if (!strcmp(st->c.name, c.name)) {
            st->remux_t = t;
            snprintf(st->remux_error, sizeof(st->remux_error), "%s", err);
            if (!err[0] || total) {
                st->remux_bad = total ? st->remux_bad + 1 : 0;
                st->remux_services = services;
                snprintf(st->remux_what, sizeof(st->remux_what), "%s", what);
                snprintf(st->remux_notes, sizeof(st->remux_notes), "%s", notes);
            }
        }
    }
    pthread_mutex_unlock(&I.lock);
    for (int i = 0; i < c.n_out; i++)
        for (int a = 0; a < n_left[i]; a++) {
            char subj[96], w[200];

            snprintf(subj, sizeof(subj), "ird/%s/left", c.name);
            snprintf(w, sizeof(w), "IRD %s: service %d is no longer in %s", c.name, left[i][a], c.outputs[i]);
            wd_instant_server(t, subj, WD_REMUX, w);
        }
}

/* ---- the thread ------------------------------------------------------------------- */

static void *run(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&I.lock);
    while (!I.stop) {
        int64_t t = time(NULL);
        bool now = I.now;
        int todo_poll = -1, todo_cmp = -1;

        I.now = false;
        for (int k = 0; k < I.n; k++) {
            struct ird_state *st = &I.ird[k].st;

            if (now)
                st->last_try = 0, st->remux_t = 0;
            if (todo_poll < 0 && t - st->last_try >= (I.o.poll_s > 0 ? I.o.poll_s : 60))
                todo_poll = k;
            if (todo_cmp < 0 && I.o.remux_s > 0 && st->c.raw[0] && st->c.n_out && t - st->remux_t >= I.o.remux_s &&
                st->last_try)
                todo_cmp = k;
        }
        if (todo_poll >= 0) {
            I.ird[todo_poll].st.last_try = t;
            pthread_mutex_unlock(&I.lock);
            poll_one(todo_poll);
            if (I.ird[todo_poll].st.answering)
                tables_one(todo_poll);
            pthread_mutex_lock(&I.lock);
            continue;
        }
        if (todo_cmp >= 0) {
            I.ird[todo_cmp].st.remux_t = t;
            pthread_mutex_unlock(&I.lock);
            compare_one(todo_cmp);
            pthread_mutex_lock(&I.lock);
            continue;
        }
        {
            struct timespec ts;

            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&I.wake, &I.lock, &ts);
        }
    }
    pthread_mutex_unlock(&I.lock);
    return NULL;
}

void ird_set(const struct ird_conf *list, int n, const struct ird_opts *o)
{
    pthread_mutex_lock(&I.lock);
    {
        struct ird old[IRDS_MAX];
        int old_n = I.n;

        memcpy(old, I.ird, sizeof(old));
        I.o = *o;
        snprintf(I.binary, sizeof(I.binary), "%s", o->binary ? o->binary : "caricoder3");
        I.o.binary = I.binary;
        I.n = n > IRDS_MAX ? IRDS_MAX : n;
        for (int k = 0; k < I.n; k++) {
            struct ird *d = &I.ird[k];
            int keep = -1;

            for (int j = 0; j < old_n; j++)
                if (!strcmp(old[j].st.c.name, list[k].name) && !strcmp(old[j].st.c.address, list[k].address))
                    keep = j;
            if (keep >= 0) {
                *d = old[keep];         /* its readings and its normal stay */
            } else {
                memset(d, 0, sizeof(*d));
                d->st.cn_normal = -1;
            }
            d->st.c = list[k];
        }
    }
    if (!I.started && I.n) {
        I.started = true;
        I.stop = false;
        pthread_create(&I.thread, NULL, run, NULL);
    }
    pthread_cond_signal(&I.wake);
    pthread_mutex_unlock(&I.lock);
}

void ird_stop(void)
{
    pthread_mutex_lock(&I.lock);
    if (!I.started) {
        pthread_mutex_unlock(&I.lock);
        return;
    }
    I.stop = true;
    pthread_cond_signal(&I.wake);
    pthread_mutex_unlock(&I.lock);
    pthread_join(I.thread, NULL);
    I.started = false;
}

void ird_set_uses(bool (*fn)(const char *url, int program, int pid, void *op), void *op)
{
    pthread_mutex_lock(&I.lock);
    I.uses = fn;
    I.uses_op = op;
    pthread_mutex_unlock(&I.lock);
}

void ird_check_now(void)
{
    pthread_mutex_lock(&I.lock);
    I.now = true;
    pthread_cond_signal(&I.wake);
    pthread_mutex_unlock(&I.lock);
}

int ird_get(struct ird_state *out, int max)
{
    int n;

    pthread_mutex_lock(&I.lock);
    n = I.n < max ? I.n : max;
    for (int k = 0; k < n; k++)
        out[k] = I.ird[k].st;
    pthread_mutex_unlock(&I.lock);
    return n;
}

void ird_watch(int64_t t, int cn_drop_db)
{
    struct ird_state st[IRDS_MAX];
    int n = ird_get(st, IRDS_MAX);

    for (int k = 0; k < n; k++) {
        const struct ird_state *s = &st[k];
        char subj[96], what[480];

        snprintf(subj, sizeof(subj), "ird/%.40s", s->c.name);
        snprintf(what, sizeof(what), "IRD %.40s (%.100s) not answering: %.150s", s->c.name, s->c.address, s->error);
        wd_observe(t, subj, NULL, "", WD_IRD_DOWN, s->fails >= 3, what, s->last_ok ? s->last_ok : 0);
        snprintf(what, sizeof(what), "IRD %.40s: tuner not locked (no signal)", s->c.name);
        wd_observe(t, subj, NULL, "", WD_IRD_UNLOCKED, s->answering && s->have && !s->locked, what, 0);
        snprintf(what, sizeof(what), "IRD %.40s: C/N %.1f dB, normally %.1f dB (Eb/N0 %.1f dB, BER %.1e)", s->c.name, s->cn,
                 s->cn_normal, s->ebn0, s->ber);
        wd_observe(t, subj, NULL, "", WD_IRD_CN, s->answering && s->locked && s->low >= 2 && cn_drop_db > 0, what, 0);
        snprintf(subj, sizeof(subj), "ird/%.40s/remux", s->c.name);
        snprintf(what, sizeof(what), "IRD %.40s: %.400s", s->c.name, s->remux_what);
        wd_observe(t, subj, NULL, "", WD_REMUX, s->remux_bad >= 2, what, 0);
        snprintf(subj, sizeof(subj), "ird/%.40s/tables", s->c.name);
        snprintf(what, sizeof(what), "IRD %.40s: the remux differs from the satellite: %.380s", s->c.name, s->tables_what);
        wd_observe(t, subj, NULL, "", WD_REMUX, s->answering && s->tables_bad >= 2, what, 0);
    }
}

static void jstr(FILE *f, const char *s)
{
    char q[1024];

    fputs(json_quote(s, q, sizeof(q)), f);
}

static void jnum(FILE *f, const char *key, double v, const char *fmt)
{
    fprintf(f, ", \"%s\": ", key);
    if (isnan(v))
        fputs("null", f);
    else
        fprintf(f, fmt, v);
}

static void one_json(FILE *f, const struct ird_state *s)
{
    fprintf(f, "{\"name\": ");
    jstr(f, s->c.name);
    fprintf(f, ", \"address\": ");
    jstr(f, s->c.address);
    fprintf(f, ", \"kind\": ");
    jstr(f, s->c.kind);
    {
        char u[300];

        ird_base_url(s->c.address, u, sizeof(u));
        fprintf(f, ", \"url\": ");
        jstr(f, u);
    }
    fprintf(f, ", \"raw\": ");
    jstr(f, s->c.raw);
    fprintf(f, ", \"outputs\": [");
    for (int i = 0; i < s->c.n_out; i++) {
        fprintf(f, "%s", i ? ", " : "");
        jstr(f, s->c.outputs[i]);
    }
    fprintf(f, "], \"answering\": %s, \"fails\": %d, \"last_ok\": %lld, \"error\": ", s->answering ? "true" : "false",
            s->fails, (long long)s->last_ok);
    jstr(f, s->error);
    if (s->have) {
        fprintf(f, ", \"locked\": %s", s->locked ? "true" : "false");
        jnum(f, "cn_db", s->cn, "%.1f");
        jnum(f, "ebn0_db", s->ebn0, "%.1f");
        jnum(f, "ber", s->ber, "%.2g");
        jnum(f, "strength_dbm", s->strength, "%.1f");
        jnum(f, "total_mbps", s->total_mbps, "%.2f");
        jnum(f, "valid_mbps", s->valid_mbps, "%.2f");
    }
    if (s->cn_normal >= 0)
        fprintf(f, ", \"cn_normal_db\": %.1f", s->cn_normal);
    else
        fprintf(f, ", \"cn_normal_db\": null");
    fprintf(f, ", \"readings\": %d, \"remux\": {\"checked\": %lld, \"services\": %d, \"differences\": ", s->n_samples,
            (long long)s->remux_t, s->remux_services);
    jstr(f, s->remux_what);
    fprintf(f, ", \"notes\": ");
    jstr(f, s->remux_notes);
    fprintf(f, ", \"times_in_a_row\": %d, \"error\": ", s->remux_bad);
    jstr(f, s->remux_error);
    fprintf(f, "}, \"tables\": {\"checked\": %lld, \"services\": %d, \"differences\": ", (long long)s->tables_t,
            s->tables_services);
    jstr(f, s->tables_what);
    fprintf(f, ", \"labels\": ");
    jstr(f, s->tables_labels);
    fprintf(f, ", \"times_in_a_row\": %d, \"error\": ", s->tables_bad);
    jstr(f, s->tables_error);
    fprintf(f, "}}");
}

static bool same_url(const char *a, const char *b)
{
    size_t la = strcspn(a, "?"), lb = strcspn(b, "?");

    if (!strncmp(a, "udp://@", 7))
        a += 7, la -= 7;
    else if (!strncmp(a, "udp://", 6))
        a += 6, la -= 6;
    if (!strncmp(b, "udp://@", 7))
        b += 7, lb -= 7;
    else if (!strncmp(b, "udp://", 6))
        b += 6, lb -= 6;
    return la == lb && !strncmp(a, b, la);
}

bool ird_for_url(FILE *f, const char *url)
{
    struct ird_state st[IRDS_MAX];
    int n = ird_get(st, IRDS_MAX);

    if (!url || !*url)
        return false;
    for (int k = 0; k < n; k++)
        for (int i = 0; i < st[k].c.n_out; i++)
            if (same_url(url, st[k].c.outputs[i])) {
                one_json(f, &st[k]);
                return true;
            }
    return false;
}

void ird_json(FILE *f)
{
    struct ird_state st[IRDS_MAX];
    int n = ird_get(st, IRDS_MAX);

    fputc('[', f);
    for (int k = 0; k < n; k++) {
        fprintf(f, "%s", k ? ", " : "");
        one_json(f, &st[k]);
    }
    fputc(']', f);
}
