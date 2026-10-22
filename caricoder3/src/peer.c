/*
 * caricoder3 - the other transcoders
 * Copyright (c) 2026 CariTech Solutions
 */
#include "peer.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "history.h"
#include "json.h"
#include "log.h"

#define HELLO_EVERY_S 30
#define TASKS_MAX 32

struct task {
    int peer;                   /* index in P.st */
    char url[512];
    int program, seconds;
    int64_t incident;
    char why[200];
};

static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t th;
    bool running, stop;
    char my_name[64], key[128];
    struct peer_state st[PEERS_MAX];
    int n;
    struct task q[TASKS_MAX];
    int qn;
} P = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

/* ---- a small HTTP client ------------------------------------------------- */

static char *dupf(const char *fmt, const char *a)
{
    char buf[400];

    snprintf(buf, sizeof(buf), fmt, a);
    return strdup(buf);
}

static int wait_fd(int fd, short ev, int64_t deadline_ms)
{
    struct timespec ts;
    int64_t now;
    struct pollfd p = { .fd = fd, .events = ev };

    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (now >= deadline_ms)
        return 0;
    return poll(&p, 1, (int)(deadline_ms - now));
}

int peer_http(const char *url, const char *method, const char *path, const char *key, const char *body,
              int timeout_ms, char **out)
{
    return http_call(url, method, path, "application/json", key, body, timeout_ms, out);
}

/* A chunked body, joined in place. */
static void dechunk(char *b)
{
    char *r = b, *w = b;

    for (;;) {
        char *end;
        long n = strtol(r, &end, 16);

        if (end == r || n <= 0)
            break;
        r = strstr(end, "\r\n");
        if (!r)
            break;
        r += 2;
        if ((long)strlen(r) < n)
            n = (long)strlen(r);
        memmove(w, r, (size_t)n);
        w += n;
        r += n;
        if (!strncmp(r, "\r\n", 2))
            r += 2;
    }
    *w = 0;
}

int http_call(const char *url, const char *method, const char *path, const char *ctype, const char *key,
              const char *body, int timeout_ms, char **out)
{
    char host[256], port[16] = "8550", *req = NULL, *resp = NULL;
    const char *h = url, *colon, *slash;
    struct addrinfo hints, *ai = NULL;
    int fd = -1, status = -1;
    size_t rlen = 0, rcap = 0;
    struct timespec ts;
    int64_t deadline;

    *out = NULL;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    deadline = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + timeout_ms;
    if (!strncmp(h, "http://", 7))
        h += 7;
    slash = strchr(h, '/');
    colon = strchr(h, ':');
    if (colon && (!slash || colon < slash)) {
        snprintf(host, sizeof(host), "%.*s", (int)(colon - h), h);
        snprintf(port, sizeof(port), "%.*s", (int)(slash ? slash - colon - 1 : (long)strlen(colon + 1)), colon + 1);
    } else {
        snprintf(host, sizeof(host), "%.*s", (int)(slash ? slash - h : (long)strlen(h)), h);
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &ai) != 0 || !ai) {
        *out = dupf("cannot resolve %s", host);
        return -1;
    }
    fd = socket(ai->ai_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0 || (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0 && errno != EINPROGRESS)) {
        *out = dupf("cannot connect: %s", strerror(errno));
        goto done;
    }
    if (wait_fd(fd, POLLOUT, deadline) <= 0) {
        *out = dupf("%s: no answer (connect timed out)", host);
        goto done;
    }
    {
        int e = 0;
        socklen_t el = sizeof(e);

        getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el);
        if (e) {
            *out = dupf("cannot connect: %s", strerror(e));
            goto done;
        }
    }
    {
        size_t blen = body ? strlen(body) : 0, n = 0, len;

        len = strlen(method) + strlen(path) + strlen(host) + (key ? strlen(key) : 0) + strlen(ctype) + blen + 300;
        req = malloc(len);
        if (!req) {
            *out = strdup("out of memory");
            goto done;
        }
        snprintf(req, len, "%s %s HTTP/1.1\r\nHost: %s:%s\r\n%s%s%sContent-Type: %s\r\n"
                 "Content-Length: %zu\r\nConnection: close\r\n\r\n%s", method, path, host, port,
                 key && *key ? (strncmp(key, "Basic ", 6) ? "Authorization: CC3-Peer " : "Authorization: ") : "",
                 key && *key ? key : "", key && *key ? "\r\n" : "",
                 ctype, blen, body ? body : "");
        len = strlen(req);
        while (n < len) {
            ssize_t w = send(fd, req + n, len - n, MSG_NOSIGNAL);

            if (w > 0) {
                n += (size_t)w;
            } else if (w < 0 && (errno == EAGAIN || errno == EINTR)) {
                if (wait_fd(fd, POLLOUT, deadline) <= 0) {
                    *out = strdup("timed out sending");
                    goto done;
                }
            } else {
                *out = dupf("send: %s", strerror(errno));
                goto done;
            }
        }
    }
    for (;;) {
        ssize_t r;

        if (rlen + 4096 + 1 > rcap) {
            size_t nc = rcap ? rcap * 2 : 16384;
            char *nr;

            if (nc > (8u << 20) || !(nr = realloc(resp, nc))) {
                *out = strdup("the answer is too big");
                goto done;
            }
            resp = nr;
            rcap = nc;
        }
        r = recv(fd, resp + rlen, rcap - rlen - 1, 0);
        if (r > 0) {
            rlen += (size_t)r;
        } else if (r == 0) {
            break;
        } else if (errno == EAGAIN || errno == EINTR) {
            if (wait_fd(fd, POLLIN, deadline) <= 0) {
                *out = strdup("no answer in time");
                goto done;
            }
        } else {
            *out = dupf("recv: %s", strerror(errno));
            goto done;
        }
    }
    if (!resp || rlen < 12 || strncmp(resp, "HTTP/1.", 7)) {
        *out = strdup("not an HTTP answer");
        goto done;
    }
    resp[rlen] = 0;
    status = atoi(resp + 9);
    {
        char *b = strstr(resp, "\r\n\r\n");
        bool chunked = false;

        if (b) {
            *b = 0;
            for (char *hp = resp; *hp; hp++)
                if (!strncasecmp(hp, "\r\ntransfer-encoding:", 20) && strcasestr(hp + 20, "chunked"))
                    chunked = true;
        }
        *out = strdup(b ? b + 4 : "");
        if (*out && chunked)
            dechunk(*out);
    }
done:
    if (fd >= 0)
        close(fd);
    if (ai)
        freeaddrinfo(ai);
    free(req);
    free(resp);
    return status;
}

/* ---- the thread ------------------------------------------------------------ */

static void hello(int k)
{
    char url[256], key[128], *out = NULL, err[200] = "";
    int status;
    struct peer_state got;
    static struct json_doc doc;            /* this thread only */

    pthread_mutex_lock(&P.lock);
    snprintf(url, sizeof(url), "%s", P.st[k].url);
    snprintf(key, sizeof(key), "%s", P.key);
    P.st[k].last_try = time(NULL);
    pthread_mutex_unlock(&P.lock);
    memset(&got, 0, sizeof(got));
    status = peer_http(url, "GET", "/api/peer/hello", key, NULL, 4000, &out);
    if (status == 200 && out) {
        int root = json_parse(&doc, out, err, sizeof(err));

        if (root >= 0 && json_get_bool(&doc, root, "ok", false)) {
            got.reachable = true;
            snprintf(got.version, sizeof(got.version), "%s", json_get_str(&doc, root, "version", ""));
            snprintf(got.host, sizeof(got.host), "%s", json_get_str(&doc, root, "name", ""));
            got.channels = (int)json_get_num(&doc, root, "channels", 0);
            got.running = (int)json_get_num(&doc, root, "running", 0);
            got.open_incidents = (int)json_get_num(&doc, root, "open_incidents", 0);
            got.cpu = json_get_num(&doc, root, "cpu", -1);
        } else {
            snprintf(err, sizeof(err), "a strange answer");
        }
    } else if (status == 401) {
        snprintf(err, sizeof(err), "it does not take our key (the same key on both, under Peers)");
    } else if (status > 0) {
        snprintf(err, sizeof(err), "it answered %d (an older caricoder3?)", status);
    } else {
        snprintf(err, sizeof(err), "%.150s", out ? out : "no answer");
    }
    free(out);
    pthread_mutex_lock(&P.lock);
    if (k < P.n && !strcmp(P.st[k].url, url)) {
        struct peer_state *s = &P.st[k];
        bool was = s->reachable;

        s->reachable = got.reachable;
        if (got.reachable) {
            s->last_ok = time(NULL);
            snprintf(s->version, sizeof(s->version), "%s", got.version);
            snprintf(s->host, sizeof(s->host), "%s", got.host);
            s->channels = got.channels;
            s->running = got.running;
            s->open_incidents = got.open_incidents;
            s->cpu = got.cpu;
            s->error[0] = 0;
        } else {
            snprintf(s->error, sizeof(s->error), "%s", err);
        }
        if (was != got.reachable && s->last_try && (was || s->last_ok)) {
            char text[300];

            snprintf(text, sizeof(text), got.reachable ? "%s is back" : "%s does not answer: %s", s->name,
                     got.reachable ? "" : err);
            history_event(time(NULL), NULL, got.reachable ? "info" : "warn", "peer", text);
        }
    }
    pthread_mutex_unlock(&P.lock);
}

static void check(const struct task *t)
{
    char url[256], name[64], key[128], my[64], *out = NULL, body[1200], q[600], qw[420], qm[140], err[200] = "";
    char text[700], res[900], args[800];
    int status;
    static struct json_doc doc;            /* this thread only */
    int64_t now = time(NULL);

    pthread_mutex_lock(&P.lock);
    if (t->peer >= P.n) {
        pthread_mutex_unlock(&P.lock);
        return;
    }
    snprintf(url, sizeof(url), "%s", P.st[t->peer].url);
    snprintf(name, sizeof(name), "%s", P.st[t->peer].name);
    snprintf(key, sizeof(key), "%s", P.key);
    snprintf(my, sizeof(my), "%s", P.my_name);
    pthread_mutex_unlock(&P.lock);

    snprintf(text, sizeof(text), "%s, are you free? %.200s. Can you check %.300s from your side?", name,
             t->why[0] ? t->why : "I have a problem with a source", t->url);
    history_message(now, name, "out", text, "{}", t->incident);
    snprintf(body, sizeof(body), "{\"url\": %s, \"program\": %d, \"seconds\": %d, \"from\": %s, \"why\": %s}",
             json_quote(t->url, q, sizeof(q)), t->program, t->seconds, json_quote(my, qm, sizeof(qm)),
             json_quote(t->why, qw, sizeof(qw)));
    status = peer_http(url, "POST", "/api/peer/check", key, body, (t->seconds + 15) * 1000, &out);
    snprintf(args, sizeof(args), "{\"peer\": %s, \"url\": %s, \"seconds\": %d}", json_quote(name, qw, sizeof(qw)),
             json_quote(t->url, q, sizeof(q)), t->seconds);
    if (status == 200 && out && json_parse(&doc, out, err, sizeof(err)) >= 0) {
        bool seen = json_get_bool(&doc, 0, "seen", false);
        double bps = json_get_num(&doc, 0, "bitrate", 0);
        int services = (int)json_get_num(&doc, 0, "services", 0);
        const char *say = json_get_str(&doc, 0, "say", "");
        const char *e = json_get_str(&doc, 0, "error", "");

        snprintf(text, sizeof(text), "%s", say[0] ? say : seen ? "I see it." : "I do not see it.");
        history_message(time(NULL), name, "in", text, out, t->incident);
        snprintf(res, sizeof(res), "{\"ok\": true, \"seen\": %s, \"bitrate\": %.0f, \"services\": %d, \"error\": %s, \"say\": %s}",
                 seen ? "true" : "false", bps, services, json_quote(e, q, sizeof(q)), json_quote(text, qw, sizeof(qw)));
    } else {
        snprintf(err, sizeof(err), "%.150s", status == 401 ? "it does not take our key" : status > 0 ? "a strange answer"
                                                                                                   : out ? out : "no answer");
        snprintf(text, sizeof(text), "(no answer from %s: %s)", name, err);
        history_message(time(NULL), name, "in", text, "{}", t->incident);
        snprintf(res, sizeof(res), "{\"ok\": false, \"error\": %s}", json_quote(err, q, sizeof(q)));
    }
    free(out);
    history_evidence_add(time(NULL), t->incident, NULL, "peer_check", args, res, "");
}

static void *thread_main(void *arg)
{
    int64_t last_hello = 0;

    (void)arg;
    for (;;) {
        struct task t;
        bool have = false;
        struct timespec ts;

        pthread_mutex_lock(&P.lock);
        if (!P.stop && !P.qn) {
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&P.cond, &P.lock, &ts);
        }
        if (P.stop) {
            pthread_mutex_unlock(&P.lock);
            break;
        }
        if (P.qn) {
            t = P.q[0];
            memmove(P.q, P.q + 1, sizeof(P.q[0]) * (size_t)(P.qn - 1));
            P.qn--;
            have = true;
        }
        pthread_mutex_unlock(&P.lock);
        if (have) {
            check(&t);
            continue;
        }
        if (time(NULL) - last_hello >= HELLO_EVERY_S) {
            int n;

            last_hello = time(NULL);
            pthread_mutex_lock(&P.lock);
            n = P.n;
            pthread_mutex_unlock(&P.lock);
            for (int k = 0; k < n; k++)
                hello(k);
        }
    }
    return NULL;
}

void peer_set(const struct peer_conf *list, int n, const char *my_name, const char *key)
{
    pthread_mutex_lock(&P.lock);
    snprintf(P.my_name, sizeof(P.my_name), "%s", my_name ? my_name : "");
    snprintf(P.key, sizeof(P.key), "%s", key ? key : "");
    {
        struct peer_state old[PEERS_MAX];
        int on = P.n;

        memcpy(old, P.st, sizeof(old));
        P.n = 0;
        for (int i = 0; i < n && P.n < PEERS_MAX; i++) {
            struct peer_state *s = &P.st[P.n++];

            memset(s, 0, sizeof(*s));
            snprintf(s->name, sizeof(s->name), "%s", list[i].name);
            snprintf(s->url, sizeof(s->url), "%s", list[i].url);
            for (int k = 0; k < on; k++)
                if (!strcmp(old[k].url, s->url)) {
                    *s = old[k];
                    snprintf(s->name, sizeof(s->name), "%s", list[i].name);
                }
        }
    }
    if (!P.running) {
        P.stop = false;
        P.running = pthread_create(&P.th, NULL, thread_main, NULL) == 0;
    }
    pthread_mutex_unlock(&P.lock);
}

void peer_stop(void)
{
    pthread_mutex_lock(&P.lock);
    if (!P.running) {
        pthread_mutex_unlock(&P.lock);
        return;
    }
    P.stop = true;
    pthread_cond_signal(&P.cond);
    pthread_mutex_unlock(&P.lock);
    pthread_join(P.th, NULL);
    P.running = false;
}

int peer_get(struct peer_state *out, int max)
{
    int n;

    pthread_mutex_lock(&P.lock);
    n = P.n < max ? P.n : max;
    memcpy(out, P.st, sizeof(*out) * (size_t)n);
    pthread_mutex_unlock(&P.lock);
    return n;
}

int peer_ask_check(const char *url, int program, int seconds, int64_t incident, const char *why)
{
    int asked = 0;

    pthread_mutex_lock(&P.lock);
    for (int k = 0; k < P.n && P.qn < TASKS_MAX; k++) {
        struct task *t;

        if (!P.st[k].reachable)
            continue;
        t = &P.q[P.qn++];
        memset(t, 0, sizeof(*t));
        t->peer = k;
        snprintf(t->url, sizeof(t->url), "%s", url);
        t->program = program;
        t->seconds = seconds;
        t->incident = incident;
        snprintf(t->why, sizeof(t->why), "%s", why ? why : "");
        asked++;
    }
    pthread_cond_signal(&P.cond);
    pthread_mutex_unlock(&P.lock);
    return asked;
}
