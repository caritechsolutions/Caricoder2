/*
 * caricoder3 - outputs: the set of sinks and their threads
 * Copyright (c) 2026 CariTech Solutions
 */
#include "sink.h"

#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>

#include "log.h"

extern const struct sink_ops sink_udp_ops;
#ifdef HAVE_SRT
extern const struct sink_ops sink_srt_ops;
#endif
#ifdef HAVE_RIST
extern const struct sink_ops sink_rist_ops;
#endif

static const struct sink_ops *const types[] = {
    &sink_udp_ops,
#ifdef HAVE_SRT
    &sink_srt_ops,
#endif
#ifdef HAVE_RIST
    &sink_rist_ops,
#endif
    NULL
};

#define WAIT_NS         20000000LL          /* how long a sink waits for packets before looking at `stop` */

const struct sink_ops *sink_ops_for(const char *url)
{
    for (int i = 0; types[i]; i++)
        if (types[i]->takes(url))
            return types[i];
    return NULL;
}

bool sinks_init(struct sinks *s, struct ring *ring, bool live)
{
    memset(s, 0, sizeof(*s));
    s->ring = ring;
    s->live = live;
    pthread_mutex_init(&s->lock, NULL);
    return true;
}

/* The sink's thread: packets from the ring to the transport, and a flush
 * when the mux says nothing more is coming for a moment (a VBR output
 * must not sit on a part-built datagram; a CBR one always fills it). */
static void *sink_thread(void *arg)
{
    struct sink *k = arg;
    uint8_t (*buf)[TS_SIZE] = malloc((size_t)SINK_BATCH * TS_SIZE);
    char name[16];
    uint64_t flushed = k->cursor;

    snprintf(name, sizeof(name), "cc3-o-%.9s", k->id);
    prctl(PR_SET_NAME, name);
    if (!buf)
        return NULL;
    while (!__atomic_load_n(&k->stop, __ATOMIC_ACQUIRE)) {
        uint64_t lost = 0;
        bool flush = false, closed = false;
        int n = ring_read(k->ring, &k->cursor, buf, SINK_BATCH, WAIT_NS, flushed,
                          &lost, &flush, &closed);

        if (lost)
            __atomic_add_fetch(&k->overruns, lost, __ATOMIC_RELAXED);
        if (n > 0) {
            k->ops->write(k->ctx, (const uint8_t (*)[TS_SIZE])buf, n);
        } else if (flush || closed) {
            k->ops->write(k->ctx, NULL, 0);     /* send what is part-built */
            flushed = k->cursor;
            if (closed)
                break;
        }
    }
    k->ops->write(k->ctx, NULL, 0);
    free(buf);
    return NULL;
}

static struct sink *find(struct sinks *s, const char *id)
{
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->list[i]->id, id))
            return s->list[i];
    return NULL;
}

bool sinks_add(struct sinks *s, const char *id, const char *url, bool from_config,
               char *err, size_t errlen)
{
    const struct sink_ops *ops = sink_ops_for(url);
    struct sink *k;
    char made[SINK_ID_MAX];

    if (!ops) {
        snprintf(err, errlen, "no output type takes '%s' (udp://, rtp://, a file path"
#ifdef HAVE_SRT
                 ", srt://"
#endif
#ifdef HAVE_RIST
                 ", rist://"
#endif
                 ")", url);
        return false;
    }
    if (strlen(url) >= SINK_URL_MAX) {
        snprintf(err, errlen, "the URL is too long");
        return false;
    }
    pthread_mutex_lock(&s->lock);
    if (!id || !id[0]) {
        do {
            snprintf(made, sizeof(made), "out%u", ++s->seq);
        } while (find(s, made));
        id = made;
    } else if (strlen(id) >= SINK_ID_MAX) {
        pthread_mutex_unlock(&s->lock);
        snprintf(err, errlen, "the id is too long (%d at most)", SINK_ID_MAX - 1);
        return false;
    } else if (find(s, id)) {
        pthread_mutex_unlock(&s->lock);
        snprintf(err, errlen, "an output with id '%s' exists", id);
        return false;
    }
    if (s->n == SINKS_MAX) {
        pthread_mutex_unlock(&s->lock);
        snprintf(err, errlen, "no more than %d outputs", SINKS_MAX);
        return false;
    }
    k = calloc(1, sizeof(*k));
    if (!k) {
        pthread_mutex_unlock(&s->lock);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    snprintf(k->id, sizeof(k->id), "%s", id);
    snprintf(k->url, sizeof(k->url), "%s", url);
    k->ops = ops;
    k->ring = s->ring;
    k->in_config = from_config;
    k->ctx = ops->open(url, err, errlen);
    if (!k->ctx) {
        pthread_mutex_unlock(&s->lock);
        free(k);
        return false;
    }
    if (s->live) {
        k->cursor = ring_now(s->ring);
        if (pthread_create(&k->th, NULL, sink_thread, k) != 0) {
            pthread_mutex_unlock(&s->lock);
            ops->close(k->ctx);
            free(k);
            snprintf(err, errlen, "cannot start a thread");
            return false;
        }
        k->threaded = true;
    }
    s->list[s->n++] = k;
    snprintf(s->last_id, sizeof(s->last_id), "%s", k->id);
    pthread_mutex_unlock(&s->lock);
    log_info("output %s: %s (%s)", k->id, k->url, ops->name);
    return true;
}

static void sink_stop(struct sink *k)
{
    if (k->threaded) {
        __atomic_store_n(&k->stop, true, __ATOMIC_RELEASE);
        ring_publish(k->ring, false);       /* wake it, if it waits */
        pthread_join(k->th, NULL);
        k->threaded = false;
    } else {
        k->ops->write(k->ctx, NULL, 0);
    }
    k->ops->close(k->ctx);
    free(k);
}

bool sinks_remove(struct sinks *s, const char *id, char *err, size_t errlen)
{
    struct sink *k = NULL;

    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->list[i]->id, id)) {
            k = s->list[i];
            s->list[i] = s->list[--s->n];
            break;
        }
    pthread_mutex_unlock(&s->lock);
    if (!k) {
        snprintf(err, errlen, "no output with id '%s'", id);
        return false;
    }
    log_info("output %s: removed (%s)", k->id, k->url);
    sink_stop(k);                           /* outside the lock: waits for its thread */
    return true;
}

void sinks_write_sync(struct sinks *s, const uint8_t *pkt)
{
    for (int i = 0; i < s->n; i++)
        s->list[i]->ops->write(s->list[i]->ctx, (const uint8_t (*)[TS_SIZE])pkt, 1);
}

void sinks_finish(struct sinks *s)
{
    if (s->live)
        ring_close(s->ring);                /* the threads send the rest and end */
    for (int i = 0; i < s->n; i++) {
        struct sink *k = s->list[i];

        if (k->threaded) {
            pthread_join(k->th, NULL);
            k->threaded = false;
        } else {
            k->ops->write(k->ctx, NULL, 0);
        }
    }
}

void sinks_destroy(struct sinks *s)
{
    for (int i = 0; i < s->n; i++) {
        struct sink *k = s->list[i];

        if (k->threaded) {
            __atomic_store_n(&k->stop, true, __ATOMIC_RELEASE);
            ring_publish(k->ring, false);
            pthread_join(k->th, NULL);
        }
        k->ops->close(k->ctx);
        free(k);
    }
    s->n = 0;
    pthread_mutex_destroy(&s->lock);
}

static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            fprintf(f, "\\%c", *s);
        else if ((unsigned char)*s < 0x20)
            fprintf(f, "\\u%04x", (unsigned char)*s);
        else
            fputc(*s, f);
    }
    fputc('"', f);
}

int sinks_json(struct sinks *s, FILE *f, int indent)
{
    int n;

    pthread_mutex_lock(&s->lock);
    n = s->n;
    fprintf(f, "[");
    for (int i = 0; i < s->n; i++) {
        struct sink *k = s->list[i];
        struct sink_stats st;

        memset(&st, 0, sizeof(st));
        st.rtt_ms = -1;
        st.retransmitted = st.lost = -1;
        k->ops->stats(k->ctx, &st);
        fprintf(f, "%s\n%*s{\"id\": ", i ? "," : "", indent, "");
        json_str(f, k->id);
        fprintf(f, ", \"type\": \"%s\", \"url\": ", k->ops->name);
        json_str(f, k->url);
        fprintf(f, ", \"state\": ");
        json_str(f, st.state);
        fprintf(f, ",\n%*s \"packets\": %llu, \"bytes\": %llu, \"errors\": %llu, "
                   "\"overruns\": %llu, \"reconnects\": %llu, \"saved\": %s",
                indent, "", (unsigned long long)st.packets, (unsigned long long)st.bytes,
                (unsigned long long)st.errors,
                (unsigned long long)__atomic_load_n(&k->overruns, __ATOMIC_RELAXED),
                (unsigned long long)st.reconnects, k->in_config ? "true" : "false");
        if (st.rtt_ms >= 0)
            fprintf(f, ", \"rtt_ms\": %.1f", st.rtt_ms);
        if (st.retransmitted >= 0)
            fprintf(f, ", \"retransmitted\": %lld", (long long)st.retransmitted);
        if (st.lost >= 0)
            fprintf(f, ", \"lost\": %lld", (long long)st.lost);
        fprintf(f, "}");
    }
    fprintf(f, "\n%*s]", indent > 2 ? indent - 2 : 0, "");
    pthread_mutex_unlock(&s->lock);
    return n;
}

uint64_t sinks_errors(struct sinks *s)
{
    uint64_t total = 0;

    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < s->n; i++) {
        struct sink_stats st;

        memset(&st, 0, sizeof(st));
        s->list[i]->ops->stats(s->list[i]->ctx, &st);
        total += st.errors;
    }
    pthread_mutex_unlock(&s->lock);
    return total;
}

int sinks_count(struct sinks *s)
{
    int n;

    pthread_mutex_lock(&s->lock);
    n = s->n;
    pthread_mutex_unlock(&s->lock);
    return n;
}

const char *sinks_first_url(struct sinks *s, char *buf, size_t len)
{
    pthread_mutex_lock(&s->lock);
    snprintf(buf, len, "%s", s->n ? s->list[0]->url : "(no output)");
    pthread_mutex_unlock(&s->lock);
    return buf;
}

void sinks_last_id(struct sinks *s, char *buf, size_t len)
{
    pthread_mutex_lock(&s->lock);
    snprintf(buf, len, "%s", s->last_id);
    pthread_mutex_unlock(&s->lock);
}

bool sinks_has(struct sinks *s, const char *id)
{
    bool r;

    pthread_mutex_lock(&s->lock);
    r = find(s, id) != NULL;
    pthread_mutex_unlock(&s->lock);
    return r;
}

bool sinks_is_saved(struct sinks *s, const char *id)
{
    struct sink *k;
    bool r;

    pthread_mutex_lock(&s->lock);
    k = find(s, id);
    r = k && k->in_config;
    pthread_mutex_unlock(&s->lock);
    return r;
}

bool sinks_saved_not_in(struct sinks *s, const char *const *keep, int n_keep,
                        char *id, size_t len)
{
    bool r = false;

    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < s->n && !r; i++) {
        struct sink *k = s->list[i];
        bool kept = false;

        if (!k->in_config)
            continue;
        for (int j = 0; j < n_keep; j++)
            kept |= !strcmp(keep[j], k->id);
        if (!kept) {
            snprintf(id, len, "%s", k->id);
            r = true;
        }
    }
    pthread_mutex_unlock(&s->lock);
    return r;
}

void sinks_json_saved(struct sinks *s, FILE *f, int indent)
{
    int n = 0;

    pthread_mutex_lock(&s->lock);
    fprintf(f, "[");
    for (int i = 0; i < s->n; i++) {
        struct sink *k = s->list[i];

        if (!k->in_config)
            continue;
        fprintf(f, "%s\n%*s{\"id\": ", n++ ? "," : "", indent, "");
        json_str(f, k->id);
        fprintf(f, ", \"url\": ");
        json_str(f, k->url);
        fprintf(f, "}");
    }
    fprintf(f, "\n%*s]", indent > 2 ? indent - 2 : 0, "");
    pthread_mutex_unlock(&s->lock);
}

void sink_list(FILE *f)
{
    fprintf(f, "Output types:\n");
    fprintf(f, "  udp        udp://host:port  rtp://host:port  (?ttl= iface= pkt= buf=), or a file\n");
#ifdef HAVE_SRT
    fprintf(f, "  srt        srt://host:port (caller) or srt://:port (listener)\n"
               "             ?mode=caller|listener latency=120 passphrase= pbkeylen=16|24|32 streamid= pkt=7\n");
#else
    fprintf(f, "  srt        not in this build (libsrt was not found)\n");
#endif
#ifdef HAVE_RIST
    fprintf(f, "  rist       rist://host:port (caller) or rist://@:port (listener)\n"
               "             ?profile=main|simple buffer=ms secret= aes-type=128|256 bandwidth=kbps cname= pkt=7\n");
#else
    fprintf(f, "  rist       not in this build (librist was not found)\n");
#endif
}

bool sink_url_param(const char *url, const char *name, char *buf, size_t len)
{
    const char *q = strchr(url, '?');
    size_t nl = strlen(name);

    while (q) {
        q++;
        if (!strncmp(q, name, nl) && q[nl] == '=') {
            const char *v = q + nl + 1;
            const char *e = strchr(v, '&');
            size_t vl = e ? (size_t)(e - v) : strlen(v);

            if (vl >= len)
                vl = len - 1;
            memcpy(buf, v, vl);
            buf[vl] = 0;
            return true;
        }
        q = strchr(q, '&');
    }
    return false;
}

bool sink_url_hostport(const char *url, char *host, size_t hostlen, int *port)
{
    const char *p = strstr(url, "://");
    const char *end, *colon;

    if (!p)
        return false;
    p += 3;
    end = strchr(p, '?');
    if (!end)
        end = p + strlen(p);
    if (*p == '[') {                        /* [v6]:port */
        const char *close = memchr(p, ']', (size_t)(end - p));

        if (!close || close[1] != ':')
            return false;
        if ((size_t)(close - p - 1) >= hostlen)
            return false;
        memcpy(host, p + 1, (size_t)(close - p - 1));
        host[close - p - 1] = 0;
        colon = close + 1;
    } else {
        colon = memrchr(p, ':', (size_t)(end - p));
        if (!colon)
            return false;
        if ((size_t)(colon - p) >= hostlen)
            return false;
        memcpy(host, p, (size_t)(colon - p));
        host[colon - p] = 0;
    }
    *port = atoi(colon + 1);
    return *port > 0 && *port < 65536;
}

void packer_init(struct packer *p, int pkt)
{
    p->pkt = pkt > 0 && pkt <= PACKER_MAX_PKT ? pkt : PACKER_MAX_PKT;
    p->fill = 0;
}

void packer_feed(struct packer *p, const uint8_t (*pkts)[TS_SIZE], int n, packer_fn fn, void *ctx)
{
    for (int i = 0; i < n; i++) {
        memcpy(p->buf + (size_t)p->fill * TS_SIZE, pkts[i], TS_SIZE);
        if (++p->fill == p->pkt) {
            fn(ctx, p->buf, p->fill * TS_SIZE);
            p->fill = 0;
        }
    }
}

void packer_flush(struct packer *p, packer_fn fn, void *ctx)
{
    if (p->fill) {
        fn(ctx, p->buf, p->fill * TS_SIZE);
        p->fill = 0;
    }
}
