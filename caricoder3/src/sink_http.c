/*
 * caricoder3 - the channel's stream over HTTP, for whoever asks
 * Copyright (c) 2026 CariTech Solutions
 *
 * Every channel the supervisor runs offers its output as one endless
 * HTTP response: GET http://server:8550/NAME/mpegts gives the transport
 * stream from that moment on, at the rate it is made. What Flussonic
 * calls "tshttp": another stage, another server or a player pulls it
 * when it wants it, and nothing is sent to anyone who does not ask.
 *
 * The channel listens on a Unix socket (RUN_DIR/NAME.ts); the supervisor
 * passes a request for /NAME/mpegts to it, with the asker's address in
 * X-Forwarded-For. Each client has a buffer of its own: one that reads
 * too slowly fills it and is dropped, without holding up the others or
 * the channel.
 *
 * The sink is "http-pull://" and is added by main for a named channel;
 * it is not in the channel's saved outputs.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "sink.h"

#define PULL_CLIENTS    SINK_CLIENTS
#define PULL_BUF        (4 << 20)           /* what a client may fall behind by: 4 s at 8 Mb/s */
#define PULL_REQ_MS     5000                /* how long a client has to send its request */

struct pull_client {
    int fd;
    char addr[64];
    time_t since;
    uint8_t *buf;                           /* waiting to be sent */
    size_t len;
    uint64_t sent;                          /* bytes, for the list */
    uint64_t rate_bytes;                    /* ... and at rate_at, for the rate */
    int64_t rate_at_ms;
    double mbps;
};

struct pull_sink {
    char path[108];
    int lfd;
    pthread_t th;
    bool stop;

    pthread_mutex_t lock;                   /* the clients */
    struct pull_client cl[PULL_CLIENTS];
    int n;
    _Atomic uint64_t packets, bytes, dropped, served;
};

static char g_socket[108];

void sink_http_set_socket(const char *path)
{
    snprintf(g_socket, sizeof(g_socket), "%s", path ? path : "");
}

static int64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool pull_takes(const char *url)
{
    return !strncmp(url, "http-pull://", 12);
}

/* The request, up to its blank line; the address the supervisor says it
 * came from. False when it is not a GET or does not come in time. */
static bool read_request(int fd, char *addr, size_t alen)
{
    char req[4096];
    size_t len = 0;
    int64_t until = now_ms() + PULL_REQ_MS;
    char *x;

    addr[0] = 0;
    while (len < sizeof(req) - 1) {
        struct pollfd pf = { fd, POLLIN, 0 };
        int64_t left = until - now_ms();
        ssize_t r;

        if (left <= 0 || poll(&pf, 1, (int)left) <= 0)
            return false;
        r = recv(fd, req + len, sizeof(req) - 1 - len, 0);
        if (r <= 0)
            return false;
        len += (size_t)r;
        req[len] = 0;
        if (strstr(req, "\r\n\r\n"))
            break;
    }
    if (strncmp(req, "GET ", 4))
        return false;
    x = strcasestr(req, "\r\nX-Forwarded-For:");
    if (x) {
        x += 18;
        while (*x == ' ')
            x++;
        snprintf(addr, alen, "%.*s", (int)strcspn(x, "\r\n, "), x);
    }
    return true;
}

static void drop(struct pull_sink *s, int i, const char *why)
{
    if (why)
        log_info("http: %s %s (%d left)", s->cl[i].addr[0] ? s->cl[i].addr : "a client", why, s->n - 1);
    close(s->cl[i].fd);
    free(s->cl[i].buf);
    s->cl[i] = s->cl[--s->n];
}

static void *accept_thread(void *arg)
{
    struct pull_sink *s = arg;
    static const char ok[] = "HTTP/1.1 200 OK\r\nContent-Type: video/MP2T\r\nCache-Control: no-cache\r\n"
                             "Connection: close\r\n\r\n";
    static const char full[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

    prctl(PR_SET_NAME, "cc3-http-pull");
    while (!__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE)) {
        struct pollfd pf = { s->lfd, POLLIN, 0 };
        struct pull_client c;
        int fd, buf = 1 << 20;

        if (poll(&pf, 1, 200) <= 0)
            continue;
        fd = accept4(s->lfd, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0)
            continue;
        memset(&c, 0, sizeof(c));
        if (!read_request(fd, c.addr, sizeof(c.addr))) {
            close(fd);
            continue;
        }
        c.buf = malloc(PULL_BUF);
        pthread_mutex_lock(&s->lock);
        if (s->n >= PULL_CLIENTS || !c.buf) {
            pthread_mutex_unlock(&s->lock);
            if (send(fd, full, sizeof(full) - 1, MSG_NOSIGNAL) < 0)
                log_dbg("http: a client left before it was refused");
            log_warn("http: %s refused: %d clients already", c.addr[0] ? c.addr : "a client", PULL_CLIENTS);
            free(c.buf);
            close(fd);
            continue;
        }
        if (send(fd, ok, sizeof(ok) - 1, MSG_NOSIGNAL) != (ssize_t)(sizeof(ok) - 1)) {
            pthread_mutex_unlock(&s->lock);
            free(c.buf);
            close(fd);
            continue;
        }
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        c.fd = fd;
        c.since = time(NULL);
        c.rate_at_ms = now_ms();
        s->cl[s->n++] = c;
        atomic_fetch_add(&s->served, 1);
        log_info("http: %s is pulling the stream (%d now)", c.addr[0] ? c.addr : "a client", s->n);
        pthread_mutex_unlock(&s->lock);
    }
    return NULL;
}

static void *pull_open(const char *url, char *err, size_t errlen)
{
    struct pull_sink *s;
    struct sockaddr_un sa;

    (void)url;
    if (!g_socket[0]) {
        snprintf(err, errlen, "the stream over HTTP is for a channel run by the supervisor (--name)");
        return NULL;
    }
    s = calloc(1, sizeof(*s));
    if (!s) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    pthread_mutex_init(&s->lock, NULL);
    snprintf(s->path, sizeof(s->path), "%s", g_socket);
    unlink(s->path);
    s->lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", s->path);
    if (s->lfd < 0 || bind(s->lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(s->lfd, 32) < 0 ||
        pthread_create(&s->th, NULL, accept_thread, s) != 0) {
        snprintf(err, errlen, "%s: %s", s->path, strerror(errno));
        if (s->lfd >= 0)
            close(s->lfd);
        unlink(s->path);
        pthread_mutex_destroy(&s->lock);
        free(s);
        return NULL;
    }
    return s;
}

/* Sends what a client's buffer holds, as far as its socket takes it. */
static bool send_some(struct pull_client *c)
{
    size_t off = 0;

    while (off < c->len) {
        ssize_t r = send(c->fd, c->buf + off, c->len - off, MSG_NOSIGNAL);

        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return false;                   /* gone */
        off += (size_t)r;
    }
    c->sent += off;
    if (off && off < c->len)
        memmove(c->buf, c->buf + off, c->len - off);
    c->len -= off;
    return true;
}

static void pull_write(void *ctx, const uint8_t (*pkt)[TS_SIZE], int n)
{
    struct pull_sink *s = ctx;
    size_t add = (size_t)n * TS_SIZE;
    int64_t now;

    atomic_fetch_add(&s->packets, (uint64_t)n);
    atomic_fetch_add(&s->bytes, (uint64_t)add);
    pthread_mutex_lock(&s->lock);
    now = s->n ? now_ms() : 0;
    for (int i = 0; i < s->n; ) {
        struct pull_client *c = &s->cl[i];

        if (c->len + add > PULL_BUF) {
            atomic_fetch_add(&s->dropped, 1);
            drop(s, i, "reads too slowly: dropped");
            continue;
        }
        if (add) {
            memcpy(c->buf + c->len, pkt, add);
            c->len += add;
        }
        if (!send_some(c)) {
            drop(s, i, "left");
            continue;
        }
        if (now - c->rate_at_ms >= 2000) {
            c->mbps = (double)(c->sent - c->rate_bytes) * 8.0 / 1e3 / (double)(now - c->rate_at_ms);
            c->rate_bytes = c->sent;
            c->rate_at_ms = now;
        }
        i++;
    }
    pthread_mutex_unlock(&s->lock);
}

static void pull_stats(void *ctx, struct sink_stats *st)
{
    struct pull_sink *s = ctx;

    pthread_mutex_lock(&s->lock);
    snprintf(st->state, sizeof(st->state), "%d client%s", s->n, s->n == 1 ? "" : "s");
    st->has_clients = true;
    for (int i = 0; i < s->n && st->n_clients < SINK_CLIENTS; i++) {
        struct sink_client *c = &st->clients[st->n_clients++];

        memset(c, 0, sizeof(*c));
        snprintf(c->addr, sizeof(c->addr), "%s", s->cl[i].addr);
        c->since = (int64_t)s->cl[i].since;
        c->rtt_ms = -1;
        c->mbps = s->cl[i].mbps;
        c->sent = (int64_t)(s->cl[i].sent / TS_SIZE);
        c->retransmitted = c->lost = -1;
    }
    pthread_mutex_unlock(&s->lock);
    st->packets = atomic_load(&s->packets);
    st->bytes = atomic_load(&s->bytes);
    st->errors = 0;                         /* a client that was too slow is its own trouble, not the output's */
    st->reconnects = 0;
}

static void pull_close(void *ctx)
{
    struct pull_sink *s = ctx;

    __atomic_store_n(&s->stop, true, __ATOMIC_RELEASE);
    pthread_join(s->th, NULL);
    while (s->n)
        drop(s, 0, NULL);
    close(s->lfd);
    unlink(s->path);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

const struct sink_ops sink_http_ops = {
    .name = "http", .takes = pull_takes, .open = pull_open, .write = pull_write,
    .stats = pull_stats, .close = pull_close,
};
