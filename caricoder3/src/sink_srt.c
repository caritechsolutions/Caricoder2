/*
 * caricoder3 - output over SRT (libsrt), caller or listener
 * Copyright (c) 2026 CariTech Solutions
 *
 *   srt://host:port[?latency=120&passphrase=...&pbkeylen=16&streamid=...&pkt=7]
 *   srt://:port[?mode=listener...]          listen on every address
 *   srt://10.0.0.5:port?mode=listener       listen on one address
 *
 * A caller connects in a thread of its own and connects again, after a
 * second, whenever the link breaks; while there is no link its packets
 * are dropped and counted. A listener accepts any number of receivers
 * (up to SRT_PEERS) and sends every datagram to each. Sends never block:
 * a receiver whose buffer is full loses that datagram, the others do
 * not. Statistics come from libsrt: RTT, retransmissions, loss.
 */
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <srt/srt.h>

/* srt.h pulls in syslog.h, whose LOG_* names clash with ours. */
#undef LOG_ERR
#undef LOG_WARN
#undef LOG_INFO
#undef LOG_DEBUG

#include "log.h"
#include "sink.h"

#define SRT_PEERS       8
#define RECONNECT_US    1000000
#define POLL_MS         200

struct srt_sink {
    char host[256];
    int port;
    bool listener;
    int latency_ms;
    char passphrase[80];
    int pbkeylen;
    char streamid[512];
    int ttl;

    pthread_mutex_t lock;                   /* peers, state */
    SRTSOCKET sock;                         /* the listening socket, or the caller's */
    SRTSOCKET peers[SRT_PEERS];             /* connected: where datagrams go */
    int n_peers;
    char state[48];

    pthread_t th;
    bool stop;
    struct packer pk;
    _Atomic uint64_t packets, bytes, errors, reconnects;
};

/* ---- the library, once -------------------------------------------------- */

static pthread_mutex_t lib_lock = PTHREAD_MUTEX_INITIALIZER;
static int lib_users;

static void srt_log_fn(void *opaque, int level, const char *file, int line,
                       const char *area, const char *message)
{
    (void)opaque; (void)file; (void)line; (void)area;
    if (level <= 3)                         /* LOG_ERR and worse */
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
        srt_setloglevel(4);                 /* warnings and errors, through our log */
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

static bool srt_takes(const char *url)
{
    return !strncmp(url, "srt://", 6);
}

static bool set_opts(struct srt_sink *s, SRTSOCKET u, char *err, size_t errlen)
{
    int tt = SRTT_LIVE, no = 0, yes = 1;

    if (srt_setsockflag(u, SRTO_TRANSTYPE, &tt, sizeof(tt)) < 0 ||
        srt_setsockflag(u, SRTO_SNDSYN, &no, sizeof(no)) < 0 ||
        srt_setsockflag(u, SRTO_RCVSYN, &yes, sizeof(yes)) < 0 ||
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
    if (s->ttl > 0)
        srt_setsockflag(u, SRTO_IPTTL, &s->ttl, sizeof(s->ttl));
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

static void set_state(struct srt_sink *s, const char *fmt, int n)
{
    pthread_mutex_lock(&s->lock);
    snprintf(s->state, sizeof(s->state), fmt, n);
    pthread_mutex_unlock(&s->lock);
}

/* Drops peers whose link is gone. Returns how many were dropped. */
static int prune(struct srt_sink *s)
{
    int dropped = 0;

    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < s->n_peers; ) {
        SRT_SOCKSTATUS st = srt_getsockstate(s->peers[i]);

        if (st == SRTS_BROKEN || st == SRTS_CLOSING || st == SRTS_CLOSED || st == SRTS_NONEXIST) {
            srt_close(s->peers[i]);
            s->peers[i] = s->peers[--s->n_peers];
            dropped++;
        } else {
            i++;
        }
    }
    pthread_mutex_unlock(&s->lock);
    return dropped;
}

static void caller_loop(struct srt_sink *s)
{
    char err[200];

    while (!__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE)) {
        struct sockaddr_storage sa;
        int salen, timeo = 3000;
        SRTSOCKET u;

        set_state(s, "connecting", 0);
        if (!resolve(s->host, s->port, &sa, &salen, err, sizeof(err))) {
            log_warn("srt %s:%d: %s", s->host, s->port, err);
            goto wait;
        }
        u = srt_create_socket();
        if (u == SRT_INVALID_SOCK || !set_opts(s, u, err, sizeof(err))) {
            log_warn("srt: %s", err);
            if (u != SRT_INVALID_SOCK)
                srt_close(u);
            goto wait;
        }
        srt_setsockflag(u, SRTO_CONNTIMEO, &timeo, sizeof(timeo));
        if (srt_connect(u, (struct sockaddr *)&sa, salen) < 0) {
            log_dbg("srt %s:%d: %s", s->host, s->port, srt_getlasterror_str());
            srt_close(u);
            goto wait;
        }
        pthread_mutex_lock(&s->lock);
        s->peers[0] = u;
        s->n_peers = 1;
        snprintf(s->state, sizeof(s->state), "connected");
        pthread_mutex_unlock(&s->lock);
        log_info("srt %s:%d: connected", s->host, s->port);
        while (!__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE)) {
            usleep(POLL_MS * 1000);
            if (prune(s)) {
                log_warn("srt %s:%d: connection lost, connecting again", s->host, s->port);
                atomic_fetch_add(&s->reconnects, 1);
                break;
            }
        }
        if (__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE))
            break;
wait:
        for (int i = 0; i < RECONNECT_US / (POLL_MS * 1000) && !__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE); i++)
            usleep(POLL_MS * 1000);
    }
}

static void listener_loop(struct srt_sink *s)
{
    int eid = srt_epoll_create();
    int ev = SRT_EPOLL_IN | SRT_EPOLL_ERR;

    if (eid < 0)
        return;
    srt_epoll_add_usock(eid, s->sock, &ev);
    while (!__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE)) {
        SRTSOCKET rd[1];
        int n_rd = 1;
        int rc = srt_epoll_wait(eid, rd, &n_rd, NULL, NULL, POLL_MS, NULL, NULL, NULL, NULL);

        if (rc > 0 && n_rd > 0) {
            struct sockaddr_storage sa;
            int salen = sizeof(sa);
            SRTSOCKET u = srt_accept(s->sock, (struct sockaddr *)&sa, &salen);

            if (u != SRT_INVALID_SOCK) {
                int no = 0;
                char who[64] = "?";

                srt_setsockflag(u, SRTO_SNDSYN, &no, sizeof(no));
                getnameinfo((struct sockaddr *)&sa, (socklen_t)salen, who, sizeof(who),
                            NULL, 0, NI_NUMERICHOST);
                pthread_mutex_lock(&s->lock);
                if (s->n_peers < SRT_PEERS) {
                    s->peers[s->n_peers++] = u;
                    log_info("srt :%d: receiver %s joined (%d now)", s->port, who, s->n_peers);
                } else {
                    srt_close(u);
                    log_warn("srt :%d: receiver %s refused: %d receivers already", s->port, who, SRT_PEERS);
                }
                pthread_mutex_unlock(&s->lock);
            }
        }
        if (prune(s))
            log_info("srt :%d: a receiver left", s->port);
        pthread_mutex_lock(&s->lock);
        snprintf(s->state, sizeof(s->state), "listening: %d receiver%s", s->n_peers,
                 s->n_peers == 1 ? "" : "s");
        pthread_mutex_unlock(&s->lock);
    }
    srt_epoll_release(eid);
}

static void *srt_thread(void *arg)
{
    struct srt_sink *s = arg;

    prctl(PR_SET_NAME, "cc3-srt");
    if (s->listener)
        listener_loop(s);
    else
        caller_loop(s);
    return NULL;
}

/* ---- the sink ------------------------------------------------------------- */

static void *srt_open(const char *url, char *err, size_t errlen)
{
    struct srt_sink *s = calloc(1, sizeof(*s));
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
    s->ttl = sink_url_param(url, "ttl", v, sizeof(v)) ? atoi(v) : 0;
    packer_init(&s->pk, sink_url_param(url, "pkt", v, sizeof(v)) ? atoi(v) : 7);
    pthread_mutex_init(&s->lock, NULL);
    s->sock = SRT_INVALID_SOCK;
    snprintf(s->state, sizeof(s->state), "%s", s->listener ? "listening: 0 receivers" : "connecting");

    if (!lib_get(err, errlen)) {
        free(s);
        return NULL;
    }
    if (s->listener) {
        struct sockaddr_storage sa;
        int salen;
        bool ok;

        err[0] = 0;
        int no = 0;

        s->sock = srt_create_socket();
        ok = s->sock != SRT_INVALID_SOCK && set_opts(s, s->sock, err, errlen) &&
             srt_setsockflag(s->sock, SRTO_RCVSYN, &no, sizeof(no)) >= 0 &&    /* accept never blocks */
             resolve(s->host, s->port, &sa, &salen, err, errlen) &&
             srt_bind(s->sock, (struct sockaddr *)&sa, salen) >= 0 &&
             srt_listen(s->sock, 4) >= 0;
        if (!ok) {
            if (!err[0])
                snprintf(err, errlen, "%s", srt_getlasterror_str());
            if (s->sock != SRT_INVALID_SOCK)
                srt_close(s->sock);
            lib_put();
            free(s);
            return NULL;
        }
    }
    if (pthread_create(&s->th, NULL, srt_thread, s) != 0) {
        snprintf(err, errlen, "cannot start the SRT thread");
        if (s->sock != SRT_INVALID_SOCK)
            srt_close(s->sock);
        lib_put();
        free(s);
        return NULL;
    }
    return s;
}

static void send_dgram(void *ctx, const uint8_t *dgram, int len)
{
    struct srt_sink *s = ctx;

    pthread_mutex_lock(&s->lock);
    if (s->n_peers == 0)
        atomic_fetch_add(&s->errors, 1);    /* nobody to send to: lost */
    for (int i = 0; i < s->n_peers; i++)
        if (srt_sendmsg2(s->peers[i], (const char *)dgram, len, NULL) < 0)
            atomic_fetch_add(&s->errors, 1);
    pthread_mutex_unlock(&s->lock);
}

static void srt_write(void *ctx, const uint8_t (*pkt)[TS_SIZE], int n)
{
    struct srt_sink *s = ctx;

    if (n == 0) {
        packer_flush(&s->pk, send_dgram, s);
        return;
    }
    packer_feed(&s->pk, pkt, n, send_dgram, s);
    atomic_fetch_add(&s->packets, (uint64_t)n);
    atomic_fetch_add(&s->bytes, (uint64_t)n * TS_SIZE);
}

static void srt_stats(void *ctx, struct sink_stats *st)
{
    struct srt_sink *s = ctx;

    pthread_mutex_lock(&s->lock);
    snprintf(st->state, sizeof(st->state), "%s", s->state);
    if (s->n_peers) {
        st->retransmitted = st->lost = 0;
        for (int i = 0; i < s->n_peers; i++) {
            SRT_TRACEBSTATS b;

            if (srt_bstats(s->peers[i], &b, 0) == 0) {
                if (i == 0)
                    st->rtt_ms = b.msRTT;
                st->retransmitted += b.pktRetransTotal;
                st->lost += b.pktSndLossTotal;
            }
        }
    }
    pthread_mutex_unlock(&s->lock);
    st->packets = atomic_load(&s->packets);
    st->bytes = atomic_load(&s->bytes);
    st->errors = atomic_load(&s->errors);
    st->reconnects = atomic_load(&s->reconnects);
}

static void srt_close_sink(void *ctx)
{
    struct srt_sink *s = ctx;

    __atomic_store_n(&s->stop, true, __ATOMIC_RELEASE);
    pthread_join(s->th, NULL);
    for (int i = 0; i < s->n_peers; i++)
        srt_close(s->peers[i]);
    if (s->sock != SRT_INVALID_SOCK)
        srt_close(s->sock);
    pthread_mutex_destroy(&s->lock);
    lib_put();
    free(s);
}

const struct sink_ops sink_srt_ops = {
    .name = "srt", .takes = srt_takes, .open = srt_open, .write = srt_write,
    .stats = srt_stats, .close = srt_close_sink,
};
