/*
 * caricoder3 - input over SRT (libsrt), caller or listener
 * Copyright (c) 2026 CariTech Solutions
 *
 *   srt://host:port[?latency=120&passphrase=...&pbkeylen=16&streamid=...]
 *   srt://:port[?mode=listener...]          listen on every address
 *   srt://10.0.0.5:port?mode=listener       listen on one address
 *
 * A caller connects to a sender and connects again, after a second,
 * whenever the link breaks. A listener accepts one sender at a time:
 * while one is connected, another is refused (a sender that reconnects
 * after its link dropped gets in once the old socket is seen broken,
 * which takes libsrt's peer idle timeout, a few seconds). Datagrams are
 * read with a 200 ms timeout so the input thread keeps checking whether
 * it should stop. Statistics come from libsrt: RTT, retransmissions,
 * loss.
 */
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <srt/srt.h>

#undef LOG_ERR
#undef LOG_WARN
#undef LOG_INFO
#undef LOG_DEBUG

#include "input.h"
#include "log.h"
#include "sink.h"                       /* the URL helpers */

#define RECV_TIMEOUT_MS 200
#define RECONNECT_US    1000000

struct srt_in {
    char host[256];
    int port;
    bool listener;
    int latency_ms;
    char passphrase[80];
    int pbkeylen;
    char streamid[512];

    SRTSOCKET sock;                     /* the listening socket, or SRT_INVALID_SOCK */
    int eid;                            /* epoll on it: accept only when someone is knocking */
    SRTSOCKET link;                     /* the connected socket, or SRT_INVALID_SOCK */
    int64_t next_try_us;                /* a caller: not before this (CLOCK_MONOTONIC) */
    int64_t last_data_us;               /* when the link last delivered */
    bool delivered;                     /* this link has delivered at least once */
    struct sockaddr_storage sa;         /* a caller: the sender, resolved */
    int salen;
    int tries;

    pthread_mutex_t lock;               /* state, link (stats read them from another thread) */
    char state[48];
    _Atomic uint64_t reconnects, errors;
    _Atomic int64_t bytes;
};

/* ---- the library, counted (libsrt counts srt_startup/srt_cleanup itself too) */

static pthread_mutex_t lib_lock = PTHREAD_MUTEX_INITIALIZER;
static int lib_users;

static void srt_log_fn(void *opaque, int level, const char *file, int line,
                       const char *area, const char *message)
{
    (void)opaque; (void)file; (void)line; (void)area;
    if (level <= 3)
        log_err("srt: %s", message);
    else if (level == 4)
        log_warn("srt: %s", message);
    else
        log_dbg("srt: %s", message);
}

static bool lib_get(char *err, size_t errlen)
{
    pthread_mutex_lock(&lib_lock);
    if (lib_users == 0) {
        if (srt_startup() < 0) {
            pthread_mutex_unlock(&lib_lock);
            snprintf(err, errlen, "libsrt would not start");
            return false;
        }
        srt_setloglevel(4);
        srt_setloghandler(NULL, srt_log_fn);
    }
    lib_users++;
    pthread_mutex_unlock(&lib_lock);
    return true;
}

static void lib_put(void)
{
    pthread_mutex_lock(&lib_lock);
    if (--lib_users == 0)
        srt_cleanup();
    pthread_mutex_unlock(&lib_lock);
}

/* ---- sockets ------------------------------------------------------------- */

static bool srt_in_takes(const char *url)
{
    return !strncmp(url, "srt://", 6);
}

static int64_t mono_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static bool set_opts(struct srt_in *s, SRTSOCKET u, char *err, size_t errlen)
{
    int tt = SRTT_LIVE, yes = 1, timeo = RECV_TIMEOUT_MS, idle = 3000;

    if (srt_setsockflag(u, SRTO_TRANSTYPE, &tt, sizeof(tt)) < 0 ||
        srt_setsockflag(u, SRTO_PEERIDLETIMEO, &idle, sizeof(idle)) < 0 ||
        srt_setsockflag(u, SRTO_RCVSYN, &yes, sizeof(yes)) < 0 ||
        srt_setsockflag(u, SRTO_RCVTIMEO, &timeo, sizeof(timeo)) < 0 ||
        srt_setsockflag(u, SRTO_PEERLATENCY, &s->latency_ms, sizeof(s->latency_ms)) < 0 ||
        srt_setsockflag(u, SRTO_RCVLATENCY, &s->latency_ms, sizeof(s->latency_ms)) < 0) {
        snprintf(err, errlen, "%s", srt_getlasterror_str());
        return false;
    }
    if (s->passphrase[0] &&
        (srt_setsockflag(u, SRTO_PASSPHRASE, s->passphrase, (int)strlen(s->passphrase)) < 0 ||
         srt_setsockflag(u, SRTO_PBKEYLEN, &s->pbkeylen, sizeof(s->pbkeylen)) < 0)) {
        snprintf(err, errlen, "passphrase: %s", srt_getlasterror_str());
        return false;
    }
    if (s->streamid[0] && !s->listener &&
        srt_setsockflag(u, SRTO_STREAMID, s->streamid, (int)strlen(s->streamid)) < 0) {
        snprintf(err, errlen, "streamid: %s", srt_getlasterror_str());
        return false;
    }
    return true;
}

static bool resolve(const char *host, int port, struct sockaddr_storage *sa, int *salen,
                    char *err, size_t errlen)
{
    struct addrinfo hints, *res;
    char portstr[8];
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = host[0] ? 0 : AI_PASSIVE;
    snprintf(portstr, sizeof(portstr), "%d", port);
    rc = getaddrinfo(host[0] ? host : NULL, portstr, &hints, &res);
    if (rc != 0) {
        snprintf(err, errlen, "%s: %s", host, gai_strerror(rc));
        return false;
    }
    memcpy(sa, res->ai_addr, res->ai_addrlen);
    *salen = (int)res->ai_addrlen;
    freeaddrinfo(res);
    return true;
}

static void drop_link(struct srt_in *s, const char *why)
{
    pthread_mutex_lock(&s->lock);
    if (s->link != SRT_INVALID_SOCK) {
        srt_close(s->link);
        s->link = SRT_INVALID_SOCK;
    }
    snprintf(s->state, sizeof(s->state), "%s", s->listener ? "listening: no sender" : "connecting");
    pthread_mutex_unlock(&s->lock);
    if (why) {
        atomic_fetch_add(&s->reconnects, 1);
        if (s->listener)
            log_warn("srt :%d: the sender's link is gone (%s), waiting for one", s->port, why);
        else
            log_warn("srt %s:%d: connection lost (%s), connecting again", s->host, s->port, why);
    }
}

/* A caller: one connection attempt, when it is time for one. */
static void caller_try(struct srt_in *s)
{
    struct sockaddr_storage sa;
    int salen, timeo = 1500;            /* short: the input thread waits on it */
    char err[200];
    SRTSOCKET u;

    if (mono_us() < s->next_try_us) {
        usleep(RECV_TIMEOUT_MS * 1000);
        return;
    }
    s->next_try_us = mono_us() + RECONNECT_US;
    /* The name is resolved at open and again every tenth try (a DNS
     * change), not on every try: a slow resolver would hold the thread. */
    if (s->salen == 0 || s->tries++ % 10 == 9) {
        if (!resolve(s->host, s->port, &sa, &salen, err, sizeof(err))) {
            log_warn("srt %s:%d: %s", s->host, s->port, err);
            return;
        }
        s->sa = sa;
        s->salen = salen;
    }
    sa = s->sa;
    salen = s->salen;
    u = srt_create_socket();
    if (u == SRT_INVALID_SOCK || !set_opts(s, u, err, sizeof(err))) {
        log_warn("srt: %s", err);
        if (u != SRT_INVALID_SOCK)
            srt_close(u);
        return;
    }
    srt_setsockflag(u, SRTO_CONNTIMEO, &timeo, sizeof(timeo));
    if (srt_connect(u, (struct sockaddr *)&sa, salen) < 0) {
        log_dbg("srt %s:%d: %s", s->host, s->port, srt_getlasterror_str());
        srt_close(u);
        return;
    }
    pthread_mutex_lock(&s->lock);
    s->link = u;
    snprintf(s->state, sizeof(s->state), "connected");
    pthread_mutex_unlock(&s->lock);
    s->last_data_us = mono_us();
    s->delivered = false;
    log_info("srt %s:%d: connected", s->host, s->port);
}

/* Someone knocking at the listening socket? (waits up to wait_ms) */
static bool knocking(struct srt_in *s, int wait_ms)
{
    SRTSOCKET rd[1];
    int n_rd = 1;

    return srt_epoll_wait(s->eid, rd, &n_rd, NULL, NULL, wait_ms, NULL, NULL, NULL, NULL) > 0 && n_rd > 0;
}

/* A listener: accept a sender if one is knocking. The accepted socket
 * inherits the listener's options: blocking reads with the timeout. */
static void listener_try(struct srt_in *s)
{
    struct sockaddr_storage sa;
    int salen = sizeof(sa);
    SRTSOCKET u;

    if (!knocking(s, RECV_TIMEOUT_MS))
        return;
    u = srt_accept(s->sock, (struct sockaddr *)&sa, &salen);
    if (u == SRT_INVALID_SOCK) {
        /* nobody after all, or the listening socket itself is broken:
         * either way not a reason to spin */
        SRT_SOCKSTATUS st = srt_getsockstate(s->sock);

        if (st == SRTS_BROKEN || st == SRTS_CLOSED || st == SRTS_NONEXIST) {
            atomic_fetch_add(&s->errors, 1);
            log_err("srt :%d: the listening socket is broken", s->port);
        }
        usleep(RECV_TIMEOUT_MS * 1000);
        return;
    }
    char who[64] = "?";

    getnameinfo((struct sockaddr *)&sa, (socklen_t)salen, who, sizeof(who), NULL, 0, NI_NUMERICHOST);
    pthread_mutex_lock(&s->lock);
    s->link = u;
    snprintf(s->state, sizeof(s->state), "sender %.30s", who);
    pthread_mutex_unlock(&s->lock);
    s->last_data_us = mono_us();
    s->delivered = false;
    log_info("srt :%d: sender %s connected", s->port, who);
}

/* While a sender is delivering, any other one is turned away. A sender
 * that was delivering and has gone quiet for a second (its link is dead
 * but libsrt has not timed it out yet) is replaced by the newcomer: an
 * encoder that restarts and pushes again must not have to wait for the
 * timeout. One that has not delivered yet is given the latency plus a
 * second: with TSBPD nothing arrives before the latency has passed. */
static void listener_others(struct srt_in *s)
{
    int64_t quiet_us = 1000000 + (s->delivered ? 0 : (int64_t)s->latency_ms * 1000);

    if (!knocking(s, 0))
        return;
    if (mono_us() - s->last_data_us > quiet_us) {
        drop_link(s, "silent for a second, a new sender is knocking");
        listener_try(s);
        return;
    }
    struct sockaddr_storage sa;
    int salen = sizeof(sa);
    SRTSOCKET u = srt_accept(s->sock, (struct sockaddr *)&sa, &salen);

    if (u != SRT_INVALID_SOCK) {
        char who[64] = "?";

        getnameinfo((struct sockaddr *)&sa, (socklen_t)salen, who, sizeof(who), NULL, 0, NI_NUMERICHOST);
        log_warn("srt :%d: sender %s refused: one is delivering already", s->port, who);
        srt_close(u);
    }
}

/* ---- the input ------------------------------------------------------------ */

static void *srt_in_open(const char *url, char *err, size_t errlen)
{
    struct srt_in *s = calloc(1, sizeof(*s));
    char v[512];

    if (!s) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    if (!sink_url_hostport(url, s->host, sizeof(s->host), &s->port)) {
        snprintf(err, errlen, "srt://host:port expected");
        free(s);
        return NULL;
    }
    s->listener = !s->host[0];
    if (sink_url_param(url, "mode", v, sizeof(v))) {
        if (!strcmp(v, "listener"))
            s->listener = true;
        else if (!strcmp(v, "caller"))
            s->listener = false;
        else {
            snprintf(err, errlen, "mode must be caller or listener");
            free(s);
            return NULL;
        }
    }
    if (!s->listener && !s->host[0]) {
        snprintf(err, errlen, "a caller needs a host");
        free(s);
        return NULL;
    }
    s->latency_ms = sink_url_param(url, "latency", v, sizeof(v)) ? atoi(v) : SRT_LIVE_DEF_LATENCY_MS;
    if (s->latency_ms < 20 || s->latency_ms > 10000) {
        snprintf(err, errlen, "latency must be 20..10000 ms");
        free(s);
        return NULL;
    }
    if (sink_url_param(url, "passphrase", s->passphrase, sizeof(s->passphrase)) &&
        (strlen(s->passphrase) < 10 || strlen(s->passphrase) > 79)) {
        snprintf(err, errlen, "the passphrase must be 10 to 79 characters");
        free(s);
        return NULL;
    }
    s->pbkeylen = sink_url_param(url, "pbkeylen", v, sizeof(v)) ? atoi(v) : 16;
    if (s->pbkeylen != 16 && s->pbkeylen != 24 && s->pbkeylen != 32) {
        snprintf(err, errlen, "pbkeylen must be 16, 24 or 32");
        free(s);
        return NULL;
    }
    sink_url_param(url, "streamid", s->streamid, sizeof(s->streamid));
    pthread_mutex_init(&s->lock, NULL);
    s->sock = s->link = SRT_INVALID_SOCK;
    s->eid = -1;
    snprintf(s->state, sizeof(s->state), "%s", s->listener ? "listening: no sender" : "connecting");

    if (!lib_get(err, errlen)) {
        free(s);
        return NULL;
    }
    if (s->listener) {
        struct sockaddr_storage sa;
        int salen, ev = SRT_EPOLL_IN | SRT_EPOLL_ERR;
        bool ok;

        err[0] = 0;
        s->sock = srt_create_socket();
        ok = s->sock != SRT_INVALID_SOCK && set_opts(s, s->sock, err, errlen) &&
             resolve(s->host, s->port, &sa, &salen, err, errlen) &&
             srt_bind(s->sock, (struct sockaddr *)&sa, salen) >= 0 &&
             srt_listen(s->sock, 2) >= 0 &&
             (s->eid = srt_epoll_create()) >= 0 &&
             srt_epoll_add_usock(s->eid, s->sock, &ev) >= 0;
        if (!ok) {
            if (!err[0])
                snprintf(err, errlen, "%s", srt_getlasterror_str());
            if (s->eid >= 0)
                srt_epoll_release(s->eid);
            if (s->sock != SRT_INVALID_SOCK)
                srt_close(s->sock);
            lib_put();
            free(s);
            return NULL;
        }
    }
    return s;
}

static int srt_in_recv(void *ctx, uint8_t *buf, int size)
{
    struct srt_in *s = ctx;
    SRTSOCKET u;

    pthread_mutex_lock(&s->lock);
    u = s->link;
    pthread_mutex_unlock(&s->lock);

    if (u == SRT_INVALID_SOCK) {
        if (s->listener)
            listener_try(s);
        else
            caller_try(s);
        return 0;
    }
    if (s->listener) {
        listener_others(s);
        pthread_mutex_lock(&s->lock);
        u = s->link;                    /* may have just been replaced */
        pthread_mutex_unlock(&s->lock);
        if (u == SRT_INVALID_SOCK)
            return 0;
    }

    int n = srt_recvmsg2(u, (char *)buf, size, NULL);

    if (n > 0) {
        atomic_fetch_add(&s->bytes, n);
        s->last_data_us = mono_us();
        s->delivered = true;
        return n;
    }
    if (n == 0)
        return 0;
    int e = srt_getlasterror(NULL);

    if (e == SRT_EASYNCRCV || e == SRT_ETIMEOUT) {
        /* nothing within the timeout; the link itself may still be gone */
        SRT_SOCKSTATUS st = srt_getsockstate(u);

        if (st == SRTS_BROKEN || st == SRTS_CLOSING || st == SRTS_CLOSED || st == SRTS_NONEXIST)
            drop_link(s, "peer gone");
        return 0;
    }
    drop_link(s, srt_getlasterror_str());
    return 0;
}

static void srt_in_stats(void *ctx, struct input_stats *st)
{
    struct srt_in *s = ctx;

    pthread_mutex_lock(&s->lock);
    snprintf(st->state, sizeof(st->state), "%s", s->state);
    st->connected = s->link != SRT_INVALID_SOCK;
    if (st->connected) {
        SRT_TRACEBSTATS b;

        if (srt_bstats(s->link, &b, 0) == 0) {
            st->rtt_ms = b.msRTT;
            /* loss seen on arrival, of which what was not recovered in time */
            st->lost = b.pktRcvDropTotal;
            st->retransmitted = b.pktRcvLossTotal > b.pktRcvDropTotal ? b.pktRcvLossTotal - b.pktRcvDropTotal : 0;
        }
    }
    pthread_mutex_unlock(&s->lock);
    st->reconnects = atomic_load(&s->reconnects);
    st->errors = atomic_load(&s->errors);
    st->bytes = atomic_load(&s->bytes);
}

static void srt_in_close(void *ctx)
{
    struct srt_in *s = ctx;

    if (s->link != SRT_INVALID_SOCK)
        srt_close(s->link);
    if (s->eid >= 0)
        srt_epoll_release(s->eid);
    if (s->sock != SRT_INVALID_SOCK)
        srt_close(s->sock);
    pthread_mutex_destroy(&s->lock);
    lib_put();
    free(s);
}

const struct input_ops input_srt_ops = {
    .name = "srt", .takes = srt_in_takes, .open = srt_in_open, .recv = srt_in_recv,
    .stats = srt_in_stats, .close = srt_in_close,
};
