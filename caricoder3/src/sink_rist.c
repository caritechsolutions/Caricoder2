/*
 * caricoder3 - output over RIST (librist), caller or listener
 * Copyright (c) 2026 CariTech Solutions
 *
 *   rist://host:port[?profile=main&buffer=1000&secret=...&aes-type=128&bandwidth=6000&cname=...&pkt=7]
 *   rist://@:port[?...]                     listen (librist's own notation)
 *   rist://@10.0.0.5:port                   listen on one address
 *
 * librist does the connecting, the reconnecting, the retransmission and
 * the encryption itself; this file hands it datagrams and reads its
 * statistics. The URL parameters after `profile` are librist's own
 * (buffer, bandwidth, secret, aes-type, cname, rtt-min, rtt-max,
 * session-timeout, weight, miface ...); `profile` is main (default),
 * simple or advanced.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <librist/librist.h>

#include "log.h"
#include "sink.h"

struct rist_sink {
    struct rist_ctx *ctx;
    struct rist_logging_settings *logs;
    struct rist_peer *peer;
    bool listener;
    bool closing;
    char where[256];

    pthread_mutex_t lock;                   /* what the callbacks write */
    int connected;                          /* peers up (a caller: 0 or 1) */
    double rtt_ms;
    uint64_t retransmitted;

    struct packer pk;
    _Atomic uint64_t packets, bytes, errors, reconnects;
};

static bool rist_takes(const char *url)
{
    return !strncmp(url, "rist://", 7);
}

static int rist_log_fn(void *arg, enum rist_log_level level, const char *msg)
{
    size_t n = strlen(msg);
    char line[300];

    (void)arg;
    snprintf(line, sizeof(line), "%.*s", (int)(n && msg[n - 1] == '\n' ? n - 1 : n), msg);
    if (level <= RIST_LOG_ERROR)
        log_err("rist: %s", line);
    else if (level == RIST_LOG_WARN)
        log_warn("rist: %s", line);
    else
        log_dbg("rist: %s", line);
    return 0;
}

static void on_status(void *arg, struct rist_peer *peer, enum rist_connection_status st)
{
    struct rist_sink *s = arg;

    (void)peer;
    if (__atomic_load_n(&s->closing, __ATOMIC_ACQUIRE))
        return;
    pthread_mutex_lock(&s->lock);
    switch (st) {
    case RIST_CONNECTION_ESTABLISHED:
    case RIST_CLIENT_CONNECTED:
        s->connected++;
        log_info("rist %s: %s", s->where, s->listener ? "a receiver joined" : "connected");
        break;
    case RIST_CONNECTION_TIMED_OUT:
    case RIST_CLIENT_TIMED_OUT:
        if (s->connected > 0)
            s->connected--;
        atomic_fetch_add(&s->reconnects, 1);
        log_warn("rist %s: %s", s->where, s->listener ? "a receiver timed out"
                                                       : "connection lost, connecting again");
        break;
    }
    pthread_mutex_unlock(&s->lock);
}

static int on_stats(void *arg, const struct rist_stats *st)
{
    struct rist_sink *s = arg;

    if (st->stats_type == RIST_STATS_SENDER_PEER) {
        /* The struct carries the RTT as whole milliseconds; the JSON
         * librist writes alongside has it to the microsecond. */
        const char *r = st->stats_json ? strstr(st->stats_json, "\"rtt\":") : NULL;

        pthread_mutex_lock(&s->lock);
        s->rtt_ms = r ? atof(r + 6) : (double)st->stats.sender_peer.rtt;
        s->retransmitted = st->stats.sender_peer.retransmitted;
        pthread_mutex_unlock(&s->lock);
    }
    rist_stats_free(st);
    return 0;
}

static void *rist_open(const char *url, char *err, size_t errlen)
{
    struct rist_sink *s = calloc(1, sizeof(*s));
    struct rist_peer_config *pc = NULL;
    enum rist_profile profile = RIST_PROFILE_MAIN;
    char v[64], clean[SINK_URL_MAX];
    const char *q;

    if (!s) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    if (sink_url_param(url, "profile", v, sizeof(v))) {
        if (!strcmp(v, "simple"))
            profile = RIST_PROFILE_SIMPLE;
        else if (!strcmp(v, "main"))
            profile = RIST_PROFILE_MAIN;
        else if (!strcmp(v, "advanced"))
            profile = RIST_PROFILE_ADVANCED;
        else {
            snprintf(err, errlen, "profile must be simple, main or advanced");
            free(s);
            return NULL;
        }
    }
    packer_init(&s->pk, sink_url_param(url, "pkt", v, sizeof(v)) ? atoi(v) : 7);
    s->listener = !strncmp(url, "rist://@", 8);
    q = strchr(url, '?');
    snprintf(s->where, sizeof(s->where), "%.*s", (int)(q ? q - url - 7 : (long)strlen(url) - 7), url + 7);
    pthread_mutex_init(&s->lock, NULL);

    /* Our own parameters are taken out before librist sees the URL. */
    {
        size_t o = 0;
        const char *p = url;
        bool first = true;

        while (*p && *p != '?' && o + 1 < sizeof(clean))
            clean[o++] = *p++;
        while (*p == '?' || *p == '&') {
            const char *e = strpbrk(p + 1, "&");
            size_t len = e ? (size_t)(e - p - 1) : strlen(p + 1);
            bool ours = !strncmp(p + 1, "profile=", 8) || !strncmp(p + 1, "pkt=", 4);

            if (!ours && o + len + 2 < sizeof(clean)) {
                clean[o++] = first ? '?' : '&';
                memcpy(clean + o, p + 1, len);
                o += len;
                first = false;
            }
            p += 1 + len;
        }
        clean[o] = 0;
    }

    if (rist_logging_set(&s->logs, RIST_LOG_WARN, rist_log_fn, s, NULL, NULL) != 0 ||
        rist_sender_create(&s->ctx, profile, 0, s->logs) != 0) {
        snprintf(err, errlen, "librist would not start a sender");
        goto fail;
    }
    if (rist_parse_address2(clean, &pc) != 0) {
        snprintf(err, errlen, "librist does not understand '%s'", clean);
        goto fail;
    }
    if (rist_connection_status_callback_set(s->ctx, on_status, s) != 0 ||
        rist_stats_callback_set(s->ctx, 1000, on_stats, s) != 0 ||
        rist_peer_create(s->ctx, &s->peer, pc) != 0) {
        snprintf(err, errlen, "librist refused the peer '%s'", clean);
        goto fail;
    }
    rist_peer_config_free2(&pc);
    if (rist_start(s->ctx) != 0) {
        snprintf(err, errlen, "librist would not start");
        goto fail;
    }
    return s;
fail:
    if (pc)
        rist_peer_config_free2(&pc);
    if (s->ctx)
        rist_destroy(s->ctx);
    if (s->logs)
        rist_logging_settings_free2(&s->logs);
    pthread_mutex_destroy(&s->lock);
    free(s);
    return NULL;
}

static void send_dgram(void *ctx, const uint8_t *dgram, int len)
{
    struct rist_sink *s = ctx;
    struct rist_data_block b;

    memset(&b, 0, sizeof(b));
    b.payload = dgram;
    b.payload_len = (size_t)len;
    if (rist_sender_data_write(s->ctx, &b) < 0)
        atomic_fetch_add(&s->errors, 1);
}

static void rist_write(void *ctx, const uint8_t (*pkt)[TS_SIZE], int n)
{
    struct rist_sink *s = ctx;

    if (n == 0) {
        packer_flush(&s->pk, send_dgram, s);
        return;
    }
    packer_feed(&s->pk, pkt, n, send_dgram, s);
    atomic_fetch_add(&s->packets, (uint64_t)n);
    atomic_fetch_add(&s->bytes, (uint64_t)n * TS_SIZE);
}

static void rist_stats_fn(void *ctx, struct sink_stats *st)
{
    struct rist_sink *s = ctx;

    pthread_mutex_lock(&s->lock);
    if (s->listener)
        snprintf(st->state, sizeof(st->state), "listening: %d receiver%s", s->connected,
                 s->connected == 1 ? "" : "s");
    else
        snprintf(st->state, sizeof(st->state), "%s", s->connected ? "connected" : "connecting");
    if (s->connected) {
        st->rtt_ms = s->rtt_ms;
        st->retransmitted = (int64_t)s->retransmitted;
    }
    pthread_mutex_unlock(&s->lock);
    st->packets = atomic_load(&s->packets);
    st->bytes = atomic_load(&s->bytes);
    st->errors = atomic_load(&s->errors);
    st->reconnects = atomic_load(&s->reconnects);
}

static void rist_close(void *ctx)
{
    struct rist_sink *s = ctx;

    __atomic_store_n(&s->closing, true, __ATOMIC_RELEASE);
    rist_destroy(s->ctx);                   /* stops its threads; the peer goes with it */
    rist_logging_settings_free2(&s->logs);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

const struct sink_ops sink_rist_ops = {
    .name = "rist", .takes = rist_takes, .open = rist_open, .write = rist_write,
    .stats = rist_stats_fn, .close = rist_close,
};
