/*
 * caricoder3 - input over RIST (librist), caller or listener
 * Copyright (c) 2026 CariTech Solutions
 *
 *   rist://@:port[?profile=main&buffer=1000&secret=...&aes-type=128&cname=...]
 *                                           listen (librist's own notation)
 *   rist://@10.0.0.5:port                   listen on one address
 *   rist://host:port[?...]                  call a sender that listens
 *
 * librist does the connecting, the recovery and the decryption; this
 * file reads its output queue one datagram at a time (200 ms timeout)
 * and its statistics. The URL parameters after `profile` are librist's
 * own; `profile` is main (default), simple or advanced.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <librist/librist.h>

#include "input.h"
#include "log.h"
#include "sink.h"                       /* the URL helpers */

#define RECV_TIMEOUT_MS 200

struct rist_in {
    struct rist_ctx *ctx;
    struct rist_logging_settings *logs;
    struct rist_peer *peer;
    bool listener;
    bool closing;
    char where[256];

    pthread_mutex_t lock;               /* what the callbacks write */
    int connected;
    double rtt_ms;
    uint64_t recovered, lost;
    _Atomic uint64_t reconnects, errors;
    _Atomic int64_t bytes;
};

static bool rist_in_takes(const char *url)
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
    struct rist_in *s = arg;

    (void)peer;
    if (__atomic_load_n(&s->closing, __ATOMIC_ACQUIRE))
        return;
    pthread_mutex_lock(&s->lock);
    switch (st) {
    case RIST_CONNECTION_ESTABLISHED:
    case RIST_CLIENT_CONNECTED:
        s->connected++;
        log_info("rist %s: %s", s->where, s->listener ? "a sender connected" : "connected");
        break;
    case RIST_CONNECTION_TIMED_OUT:
    case RIST_CLIENT_TIMED_OUT:
        if (s->connected > 0)
            s->connected--;
        atomic_fetch_add(&s->reconnects, 1);
        log_warn("rist %s: %s", s->where, s->listener ? "the sender timed out"
                                                       : "connection lost, connecting again");
        break;
    }
    pthread_mutex_unlock(&s->lock);
}

static int on_stats(void *arg, const struct rist_stats *st)
{
    struct rist_in *s = arg;

    if (st->stats_type == RIST_STATS_RECEIVER_FLOW) {
        const struct rist_stats_receiver_flow *f = &st->stats.receiver_flow;
        /* The struct has the RTT in whole milliseconds; the JSON beside
         * it has it finer. */
        const char *r = st->stats_json ? strstr(st->stats_json, "\"rtt\":") : NULL;

        pthread_mutex_lock(&s->lock);
        s->rtt_ms = r ? atof(r + 6) : (double)f->rtt;
        s->recovered += f->recovered;   /* librist's figures are per interval */
        s->lost += f->lost;
        pthread_mutex_unlock(&s->lock);
    }
    rist_stats_free(st);
    return 0;
}

static void *rist_in_open(const char *url, char *err, size_t errlen)
{
    struct rist_in *s = calloc(1, sizeof(*s));
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
    s->listener = !strncmp(url, "rist://@", 8);
    q = strchr(url, '?');
    snprintf(s->where, sizeof(s->where), "%.*s", (int)(q ? q - url - 7 : (long)strlen(url) - 7), url + 7);
    pthread_mutex_init(&s->lock, NULL);

    /* Our own parameter is taken out before librist sees the URL. */
    {
        size_t o = 0;
        const char *p = url;
        bool first = true;

        while (*p && *p != '?' && o + 1 < sizeof(clean))
            clean[o++] = *p++;
        while (*p == '?' || *p == '&') {
            const char *e = strpbrk(p + 1, "&");
            size_t len = e ? (size_t)(e - p - 1) : strlen(p + 1);
            bool ours = !strncmp(p + 1, "profile=", 8);

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
        rist_receiver_create(&s->ctx, profile, s->logs) != 0) {
        snprintf(err, errlen, "librist would not start a receiver");
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
    rist_receiver_set_output_fifo_size(s->ctx, 8192);
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

static int rist_in_recv(void *ctx, uint8_t *buf, int size)
{
    struct rist_in *s = ctx;
    struct rist_data_block *b = NULL;
    int rc = rist_receiver_data_read2(s->ctx, &b, RECV_TIMEOUT_MS);
    int n = 0;

    if (rc < 0) {
        atomic_fetch_add(&s->errors, 1);
        return 0;
    }
    if (!b)
        return 0;
    if (b->payload_len > 0 && b->payload_len <= (size_t)size) {
        n = (int)b->payload_len;
        memcpy(buf, b->payload, (size_t)n);
        atomic_fetch_add(&s->bytes, n);
    } else if (b->payload_len > (size_t)size) {
        atomic_fetch_add(&s->errors, 1);    /* bigger than a datagram should be */
    }
    rist_receiver_data_block_free2(&b);
    return n;
}

static void rist_in_stats(void *ctx, struct input_stats *st)
{
    struct rist_in *s = ctx;

    pthread_mutex_lock(&s->lock);
    st->connected = s->connected > 0;
    if (s->listener)
        snprintf(st->state, sizeof(st->state), "%s", s->connected ? "sender connected" : "listening: no sender");
    else
        snprintf(st->state, sizeof(st->state), "%s", s->connected ? "connected" : "connecting");
    if (s->connected) {
        st->rtt_ms = s->rtt_ms;
        st->retransmitted = (int64_t)s->recovered;
        st->lost = (int64_t)s->lost;
    }
    pthread_mutex_unlock(&s->lock);
    st->reconnects = atomic_load(&s->reconnects);
    st->errors = atomic_load(&s->errors);
    st->bytes = atomic_load(&s->bytes);
}

static void rist_in_close(void *ctx)
{
    struct rist_in *s = ctx;

    __atomic_store_n(&s->closing, true, __ATOMIC_RELEASE);
    rist_destroy(s->ctx);
    rist_logging_settings_free2(&s->logs);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

const struct input_ops input_rist_ops = {
    .name = "rist", .takes = rist_in_takes, .open = rist_in_open, .recv = rist_in_recv,
    .stats = rist_in_stats, .close = rist_in_close,
};
