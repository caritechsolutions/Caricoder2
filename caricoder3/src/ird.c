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

struct series {                         /* a day of readings, for a normal (median) */
    float v[SAMPLES];
    int n, next;
};

struct ird {
    struct ird_state st;
    struct series cn[IRD_TUNERS];       /* each tuner's C/N while locked */
    struct series temp;
    int out_progs[IRD_OUTS][PROGS_MAX]; /* the services in each output at the last comparison */
    int n_out_progs[IRD_OUTS];
    bool out_known[IRD_OUTS];
    struct upcom_tables *prev;          /* each input's tables at the last read (16, by source number) */
    char *ref[IRD_OUTS];                /* with no raw output: each output's last probe (brief JSON) */
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
    const char *h = strstr(address, "://") ? strstr(address, "://") + 3 : address, *at;
    size_t hl;

    at = strchr(h, '@');
    if (at && at < h + strcspn(h, "/"))
        h = at + 1;                     /* user:password@ */
    hl = strcspn(h, "/");
    snprintf(url, len, "http://%.*s%s", (int)hl, h, memchr(h, ':', hl) ? "" : ":80");
}

/* "Basic <base64 of user:password>" from KIND://user:password@host, else `dflt` ("" none) */
static void ird_auth(const char *address, const char *dflt, char *out, size_t len)
{
    static const char B[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const char *h = strstr(address, "://") ? strstr(address, "://") + 3 : address, *at = strchr(h, '@');
    char up[128];
    size_t n, o = 0;

    if (at && at < h + strcspn(h, "/"))
        snprintf(up, sizeof(up), "%.*s", (int)(at - h), h);
    else
        snprintf(up, sizeof(up), "%s", dflt);
    out[0] = 0;
    if (!up[0])
        return;
    n = strlen(up);
    o = (size_t)snprintf(out, len, "Basic ");
    for (size_t i = 0; i < n && o + 5 < len; i += 3) {
        unsigned v = (unsigned char)up[i] << 16 | (i + 1 < n ? (unsigned char)up[i + 1] << 8 : 0) |
                     (i + 2 < n ? (unsigned char)up[i + 2] : 0);

        out[o++] = B[v >> 18 & 63];
        out[o++] = B[v >> 12 & 63];
        out[o++] = i + 1 < n ? B[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? B[v & 63] : '=';
    }
    out[o] = 0;
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
    st->n_tuners = 1;
    st->t[0].locked = json_get_num(&d, t, "lock", 0) != 0;
    st->t[0].cn = num(json_get_str(&d, t, "CN", ""));
    st->t[0].ebn0 = num(json_get_str(&d, t, "EbN0", ""));
    st->t[0].ber = sci(json_get_str(&d, t, "ber", ""));
    st->t[0].strength = num(json_get_str(&d, t, "strength", ""));
    st->t[0].quality = NAN;
    st->t[0].total_mbps = num(json_get_str(&d, t, "totalBR", ""));
    st->t[0].valid_mbps = num(json_get_str(&d, t, "validBR", ""));
    st->t[0].have = true;
    return true;
}

/* {"tuner_count": 2, "tuner": [{"index", "status": {"lock", "c_n_100": "12.25 dB",
 * "power": "-33.12 dBm", "ber": "0.00e+00", "strength", "quality", "bitrate_valid", ...}}]} */
bool ird_parse_mcr_tuners(const char *body, struct ird_state *st)
{
    static struct json_doc d;           /* the IRD thread, or the tests */
    char err[100];
    int a, n = 0;

    if (!body || json_parse(&d, body, err, sizeof(err)) < 0 || (a = json_get(&d, 0, "tuner")) < 0)
        return false;
    for (int j = d.node[a].first_child; j >= 0 && n < IRD_TUNERS; j = d.node[j].next) {
        int x = json_get(&d, j, "status");
        struct ird_tuner *t = &st->t[n++];

        if (x < 0)
            return false;
        t->locked = json_get_num(&d, x, "lock", 0) != 0;
        t->cn = num(json_get_str(&d, x, "c_n_100", ""));
        t->ebn0 = NAN;
        t->ber = sci(json_get_str(&d, x, "ber", ""));
        t->strength = num(json_get_str(&d, x, "power", ""));
        t->quality = json_get_num(&d, x, "quality", NAN);
        t->total_mbps = json_get_num(&d, x, "bitrate_total", NAN);
        t->valid_mbps = json_get_num(&d, x, "bitrate_valid", NAN);
        t->have = true;
    }
    st->n_tuners = n;
    return n > 0;
}

/* {"uptime": "1 Day-01:31:21", "Temperature": "67.06 Degree Celsius\n", ...} */
bool ird_parse_mcr_status(const char *body, int64_t *uptime_s, double *temp_c)
{
    static struct json_doc d;
    char err[100];
    const char *u, *p;
    int days = 0, h = 0, m = 0, sec = 0;

    if (!body || json_parse(&d, body, err, sizeof(err)) < 0 || json_get(&d, 0, "uptime") < 0)
        return false;
    u = json_get_str(&d, 0, "uptime", "");
    p = u;
    if (strstr(u, "Day")) {
        days = atoi(u);
        p = strchr(u, '-');
        p = p ? p + 1 : u;
    }
    if (sscanf(p, "%d:%d:%d", &h, &m, &sec) != 3)
        return false;
    *uptime_s = (int64_t)days * 86400 + h * 3600 + m * 60 + sec;
    *temp_c = num(json_get_str(&d, 0, "Temperature", ""));
    return true;
}

static int cmpf(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;

    return x < y ? -1 : x > y;
}

static double median(const struct series *s)
{
    static float tmp[SAMPLES];

    memcpy(tmp, s->v, sizeof(float) * (size_t)s->n);
    qsort(tmp, (size_t)s->n, sizeof(float), cmpf);
    return tmp[s->n / 2];
}

/* Adds a reading; the normal once there are enough. */
static double series_add(struct series *s, double v)
{
    s->v[s->next] = (float)v;
    s->next = (s->next + 1) % SAMPLES;
    if (s->n < SAMPLES)
        s->n++;
    return s->n >= (I.o.baseline_n > 0 ? I.o.baseline_n : 30) ? median(s) : -1;
}

/* One poll of IRD k; called without the lock (the requests take time). */
static void poll_one(int k)
{
    struct ird_conf c;
    struct ird_state got;
    char url[300], auth[200], *out = NULL;
    int status;
    int64_t t = time(NULL), uptime = -1;
    double temp = NAN;
    struct wd_limits L;
    char restart[200] = "";

    pthread_mutex_lock(&I.lock);
    c = I.ird[k].st.c;
    pthread_mutex_unlock(&I.lock);
    memset(&got, 0, sizeof(got));
    ird_base_url(c.address, url, sizeof(url));
    if (!strcmp(c.kind, "mcr")) {
        ird_auth(c.address, "admin:admin", auth, sizeof(auth));
        status = http_call(url, "POST", "/cgi.php?proctype=tuner", "application/x-www-form-urlencoded; charset=UTF-8",
                           auth, "op_code=status&ch_index=0", 5000, &out);
        if (status == 200 && !ird_parse_mcr_tuners(out, &got))
            status = -2;
        if (status == 200) {
            free(out);
            out = NULL;
            if (http_call(url, "POST", "/cgi.php?proctype=runstatus", "application/x-www-form-urlencoded; charset=UTF-8",
                          auth, "", 5000, &out) == 200)
                got.have_health = ird_parse_mcr_status(out, &uptime, &temp);
        }
    } else {
        status = http_call(url, "POST", "/goform/subBoard.cgi", "application/x-www-form-urlencoded; charset=UTF-8", NULL,
                           "json=%7B%22120%22%3A%7B%22method%22%3A0%2C%22data%22%3A%7B%220000%22%3A%7B%7D%7D%7D%7D",
                           5000, &out);
        if (status == 200 && !ird_parse_upcom(out, &got))
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
            st->n_tuners = got.n_tuners;
            for (int i = 0; i < got.n_tuners; i++) {
                struct ird_tuner *tu = &st->t[i];
                double normal = tu->cn_normal;
                int low = tu->low, ns = tu->n_samples;

                *tu = got.t[i];
                tu->cn_normal = normal;
                tu->low = low;
                tu->n_samples = ns;
                if (tu->locked && !isnan(tu->cn)) {
                    /* below the normal: judged against the normal before this reading */
                    double nn;

                    tu->low = tu->cn_normal >= 0 && tu->cn < tu->cn_normal - L.ird_cn_drop_db ? tu->low + 1 : 0;
                    nn = series_add(&d->cn[i], tu->cn);
                    tu->n_samples = d->cn[i].n;
                    if (nn >= 0)
                        tu->cn_normal = nn;
                } else {
                    tu->low = 0;
                }
            }
            if (got.have_health) {
                /* a restart: up less than it was, more than the time between two looks can explain */
                if (st->have_health && st->uptime_s >= 0 && uptime + 120 < st->uptime_s + (t - st->last_ok)) {
                    st->restarted_t = t;
                    snprintf(restart, sizeof(restart), "IRD %.40s restarted: up %lld min (it had been up %lld h %02lld min)",
                             c.name, (long long)(uptime / 60), (long long)(st->uptime_s / 3600),
                             (long long)(st->uptime_s / 60 % 60));
                }
                st->have_health = true;
                st->uptime_s = uptime;
                st->temp_c = temp;
                if (!isnan(temp)) {
                    double limit = L.ird_temp_max_c > 0 ? L.ird_temp_max_c
                                 : st->temp_normal >= 0 ? st->temp_normal + L.ird_temp_rise_c : -1;
                    double nn;

                    st->temp_high = limit > 0 && temp > limit ? st->temp_high + 1 : 0;
                    nn = series_add(&d->temp, temp);
                    if (nn >= 0)
                        st->temp_normal = nn;
                }
            }
        } else {
            st->answering = false;
            st->fails++;
            for (int i = 0; i < st->n_tuners; i++) {
                st->t[i].have = false;
                st->t[i].low = 0;
            }
            st->temp_high = 0;
            snprintf(st->error, sizeof(st->error), "%s",
                     status == -2 ? "the answer is not the one this kind of IRD gives" : status < 0 && out ? out
                     : status > 0 ? "an HTTP error" : "no answer");
            if (status == 401)
                snprintf(st->error, sizeof(st->error), "HTTP 401: the user or password is not the IRD's");
            else if (status > 0)
                snprintf(st->error, sizeof(st->error), "HTTP %d", status);
        }
    }
    {
        struct ird_state st = I.ird[k].st;

        pthread_mutex_unlock(&I.lock);
        if (restart[0]) {
            char subj[96];

            snprintf(subj, sizeof(subj), "ird/%.40s/restart", c.name);
            wd_instant_server(t, subj, WD_IRD_RESTART, restart);
            log_info("%s", restart);
        }
        if (st.answering && history_enabled()) {
            char m[96];

            for (int i = 0; i < st.n_tuners; i++) {
                const struct ird_tuner *tu = &st.t[i];
                char pre[64];

                /* tuner 1's under the IRD's own name (as before there were several) */
                snprintf(pre, sizeof(pre), i ? "%.40s.t%d" : "%.40s", st.c.name, i + 1);
#define MET(key, v) do { if (!isnan(v)) { snprintf(m, sizeof(m), "ird.%s." key, pre); history_metric(t, m, v); } } while (0)
                MET("cn", tu->cn);
                MET("ebn0", tu->ebn0);
                MET("ber", tu->ber);
                MET("strength", tu->strength);
                MET("total_mbps", tu->total_mbps);
                MET("lock", tu->locked ? 1.0 : 0.0);
#undef MET
            }
            if (st.have_health) {
                snprintf(m, sizeof(m), "ird.%.40s.uptime_s", st.c.name);
                history_metric(t, m, (double)st.uptime_s);
                if (!isnan(st.temp_c)) {
                    snprintf(m, sizeof(m), "ird.%.40s.temp_c", st.c.name);
                    history_metric(t, m, st.temp_c);
                }
            }
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

/* What changed in an output since the last probe of it: streams, or a
 * service that was clear and is scrambled now. */
static int reference_changes(const char *was_json, const char *now_json, char *what, size_t len, int *services)
{
    static struct prog was[PROGS_MAX], now[PROGS_MAX];
    char err[160];
    int nw = read_progs(was_json, was, PROGS_MAX, err, sizeof(err)), nn, n = 0;

    what[0] = 0;
    *services = 0;
    nn = read_progs(now_json, now, PROGS_MAX, err, sizeof(err));
    if (nn < 0) {
        snprintf(what, len, "the output: %s", err);
        return -1;
    }
    for (int i = 0; i < nn; i++) {
        const struct prog *o = &now[i], *p = nw >= 0 ? find_prog(was, nw, o->n) : NULL;
        int scr_now = 0, scr_was = 0;
        char a[200], b[200];

        if (!o->n)
            continue;
        (*services)++;
        if (!p)
            continue;                   /* new in it: said by the comparison of the service lists */
        if (!same_streams(o, p)) {
            char both[420];

            streams_text(p, a, sizeof(a));
            streams_text(o, b, sizeof(b));
            snprintf(both, sizeof(both), "was %s; now %s", a, b);
            add_diff(what, len, &n, ": changed in the output: %s", o->name, o->n, both);
            continue;
        }
        for (int k = 0; k < o->ns; k++)
            scr_now += o->s[k].scrambled > 20;
        for (int k = 0; k < p->ns; k++)
            scr_was += p->s[k].scrambled > 20;
        if (scr_now && !scr_was)
            add_diff(what, len, &n, ": was clear and is scrambled now%s", o->name, o->n, "");
        else if (!scr_now && scr_was)
            add_diff(what, len, &n, ": was scrambled and is clear now%s", o->name, o->n, "");
    }
    return n;
}

/* With no raw output: each output probed and compared with its last probe; without the lock. */
static void reference_one(int k)
{
    struct ird_conf c;
    const char *urls[IRD_OUTS];
    char *outs[IRD_OUTS], err[160] = "", changes[IRD_OUTS][480];
    int services = 0;
    int64_t t = time(NULL);

    pthread_mutex_lock(&I.lock);
    c = I.ird[k].st.c;
    pthread_mutex_unlock(&I.lock);
    if (!c.n_out)
        return;
    for (int i = 0; i < c.n_out; i++)
        urls[i] = c.outputs[i];
    probe_many(urls, c.n_out, I.o.probe_s > 0 ? I.o.probe_s : 5, outs);
    pthread_mutex_lock(&I.lock);
    {
        struct ird *d = &I.ird[k];

        for (int i = 0; i < c.n_out; i++) {
            int sv = 0, n;

            changes[i][0] = 0;
            if (strcmp(d->st.c.name, c.name))
                break;
            if (!strstr(outs[i], "\"ok\": true")) {
                snprintf(err, sizeof(err), "%.100s: nothing could be read from it", c.outputs[i]);
                continue;
            }
            n = d->ref[i] ? reference_changes(d->ref[i], outs[i], changes[i], sizeof(changes[i]), &sv)
                          : reference_changes(outs[i], outs[i], changes[i], sizeof(changes[i]), &sv);
            if (n < 0) {
                snprintf(err, sizeof(err), "%.150s", changes[i]);
                changes[i][0] = 0;
                continue;
            }
            services += sv;
            free(d->ref[i]);
            d->ref[i] = strdup(outs[i]);
        }
        if (!strcmp(d->st.c.name, c.name)) {
            d->st.remux_t = t;
            d->st.remux_services = services;
            d->st.remux_bad = 0;
            d->st.remux_what[0] = 0;
            snprintf(d->st.remux_error, sizeof(d->st.remux_error), "%s", err);
            snprintf(d->st.remux_notes, sizeof(d->st.remux_notes), "%s",
                     services ? "no raw output: each probe is compared with the one before" : "");
        }
    }
    pthread_mutex_unlock(&I.lock);
    for (int i = 0; i < c.n_out; i++) {
        if (changes[i][0]) {
            char subj[96], w[600];

            snprintf(subj, sizeof(subj), "ird/%.40s/output", c.name);
            snprintf(w, sizeof(w), "IRD %.40s, %.60s: %.400s", c.name, c.outputs[i], changes[i]);
            wd_instant_server(t, subj, WD_SAT_CHANGED, w);
            log_info("%s", w);
        }
        free(outs[i]);
    }
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
            if (todo_cmp < 0 && I.o.remux_s > 0 && st->c.n_out && t - st->remux_t >= I.o.remux_s && st->last_try)
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
            if (I.ird[todo_cmp].st.c.raw[0])
                compare_one(todo_cmp);
            else
                reference_one(todo_cmp);
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
        /* on the heap: a few hundred kB, called from the web threads */
        struct ird *old = malloc(sizeof(I.ird));
        int old_n = old ? I.n : 0;

        if (old)
            memcpy(old, I.ird, sizeof(I.ird));
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
                for (int i = 0; i < IRD_TUNERS; i++)
                    d->st.t[i].cn_normal = -1;
                d->st.temp_normal = -1;
                d->st.temp_c = NAN;
                d->st.uptime_s = -1;
            }
            d->st.c = list[k];
        }
        for (int j = 0; j < old_n; j++) {
            bool kept = false;

            for (int k = 0; k < I.n; k++)
                kept |= !strcmp(I.ird[k].st.c.name, old[j].st.c.name) && !strcmp(I.ird[k].st.c.address, old[j].st.c.address);
            if (!kept) {
                free(old[j].prev);      /* an IRD that went: its tables, its last probes */
                for (int i = 0; i < IRD_OUTS; i++)
                    free(old[j].ref[i]);
            }
        }
        free(old);
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

/* Every IRD's state, on the heap (the callers run on threads with modest stacks). */
static struct ird_state *states(int *n)
{
    struct ird_state *st = malloc(sizeof(*st) * IRDS_MAX);

    *n = st ? ird_get(st, IRDS_MAX) : 0;
    return st;
}

void ird_watch(int64_t t, int cn_drop_db)
{
    int n;
    struct ird_state *st = states(&n);

    for (int k = 0; k < n; k++) {
        const struct ird_state *s = &st[k];
        char subj[96], what[480];

        snprintf(subj, sizeof(subj), "ird/%.40s", s->c.name);
        snprintf(what, sizeof(what), "IRD %.40s (%.100s) not answering: %.150s", s->c.name, s->c.address, s->error);
        wd_observe(t, subj, NULL, "", WD_IRD_DOWN, s->fails >= 3, what, s->last_ok ? s->last_ok : 0);
        for (int i = 0; i < IRD_TUNERS; i++) {
            const struct ird_tuner *tu = &s->t[i];
            char tn[16] = "";

            /* tuner 1 under the IRD's own subject (as before there were several) */
            if (i)
                snprintf(subj, sizeof(subj), "ird/%.40s/t%d", s->c.name, i + 1);
            if (s->n_tuners > 1)
                snprintf(tn, sizeof(tn), " tuner %d", i + 1);
            snprintf(what, sizeof(what), "IRD %.40s%s: tuner not locked (no signal)", s->c.name, tn);
            wd_observe(t, subj, NULL, "", WD_IRD_UNLOCKED, i < s->n_tuners && s->answering && tu->have && !tu->locked,
                       what, 0);
            if (isnan(tu->ebn0))
                snprintf(what, sizeof(what), "IRD %.40s%s: C/N %.1f dB, normally %.1f dB (level %.1f dBm, BER %.1e)",
                         s->c.name, tn, tu->cn, tu->cn_normal, tu->strength, tu->ber);
            else
                snprintf(what, sizeof(what), "IRD %.40s%s: C/N %.1f dB, normally %.1f dB (Eb/N0 %.1f dB, BER %.1e)",
                         s->c.name, tn, tu->cn, tu->cn_normal, tu->ebn0, tu->ber);
            wd_observe(t, subj, NULL, "", WD_IRD_CN,
                       i < s->n_tuners && s->answering && tu->locked && tu->low >= 2 && cn_drop_db > 0, what, 0);
        }
        snprintf(subj, sizeof(subj), "ird/%.40s", s->c.name);
        snprintf(what, sizeof(what), "IRD %.40s: %.1f °C inside, normally %.1f °C", s->c.name, s->temp_c, s->temp_normal);
        wd_observe(t, subj, NULL, "", WD_IRD_HOT, s->answering && s->have_health && s->temp_high >= 2, what, 0);
        snprintf(subj, sizeof(subj), "ird/%.40s/remux", s->c.name);
        snprintf(what, sizeof(what), "IRD %.40s: %.400s", s->c.name, s->remux_what);
        wd_observe(t, subj, NULL, "", WD_REMUX, s->remux_bad >= 2, what, 0);
        snprintf(subj, sizeof(subj), "ird/%.40s/tables", s->c.name);
        snprintf(what, sizeof(what), "IRD %.40s: the remux differs from the satellite: %.380s", s->c.name, s->tables_what);
        wd_observe(t, subj, NULL, "", WD_REMUX, s->answering && s->tables_bad >= 2, what, 0);
    }
    free(st);
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

static void tuner_json(FILE *f, const struct ird_tuner *t)
{
    fprintf(f, ", \"locked\": %s", t->locked ? "true" : "false");
    jnum(f, "cn_db", t->cn, "%.1f");
    jnum(f, "ebn0_db", t->ebn0, "%.1f");
    jnum(f, "ber", t->ber, "%.2g");
    jnum(f, "strength_dbm", t->strength, "%.1f");
    jnum(f, "quality", t->quality, "%.0f");
    jnum(f, "total_mbps", t->total_mbps, "%.2f");
    jnum(f, "valid_mbps", t->valid_mbps, "%.2f");
    if (t->cn_normal >= 0)
        fprintf(f, ", \"cn_normal_db\": %.1f", t->cn_normal);
    else
        fprintf(f, ", \"cn_normal_db\": null");
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
    /* tuner 1 at the top (as before there were several), and each in "tuners" */
    if (s->t[0].have)
        tuner_json(f, &s->t[0]);
    else if (s->t[0].cn_normal >= 0)
        fprintf(f, ", \"cn_normal_db\": %.1f", s->t[0].cn_normal);
    else
        fprintf(f, ", \"cn_normal_db\": null");
    fprintf(f, ", \"readings\": %d, \"tuners\": [", s->t[0].n_samples);
    for (int i = 0; i < s->n_tuners; i++) {
        fprintf(f, "%s{\"tuner\": %d, \"have\": %s", i ? ", " : "", i + 1, s->t[i].have ? "true" : "false");
        if (s->t[i].have)
            tuner_json(f, &s->t[i]);
        fprintf(f, ", \"readings\": %d}", s->t[i].n_samples);
    }
    fprintf(f, "]");
    if (s->have_health) {
        fprintf(f, ", \"uptime_s\": %lld", (long long)s->uptime_s);
        jnum(f, "temperature_c", s->temp_c, "%.1f");
        if (s->temp_normal >= 0)
            fprintf(f, ", \"temperature_normal_c\": %.1f", s->temp_normal);
        if (s->restarted_t)
            fprintf(f, ", \"restarted\": %lld", (long long)s->restarted_t);
    }
    fprintf(f, ", \"remux\": {\"checked\": %lld, \"services\": %d, \"differences\": ",
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
    int n;
    struct ird_state *st = states(&n);

    if (!url || !*url) {
        free(st);
        return false;
    }
    for (int k = 0; k < n; k++)
        for (int i = 0; i < st[k].c.n_out; i++)
            if (same_url(url, st[k].c.outputs[i])) {
                one_json(f, &st[k]);
                free(st);
                return true;
            }
    free(st);
    return false;
}

void ird_json(FILE *f)
{
    int n;
    struct ird_state *st = states(&n);

    fputc('[', f);
    for (int k = 0; k < n; k++) {
        fprintf(f, "%s", k ? ", " : "");
        one_json(f, &st[k]);
    }
    fputc(']', f);
    free(st);
}
