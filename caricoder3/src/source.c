/*
 * caricoder3 - the channel's sources: one in use, the others standing by
 * Copyright (c) 2026 CariTech Solutions
 */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#include "cc3.h"
#include "input.h"
#include "log.h"
#include "sink.h"                       /* SINK_URL_MAX */
#include "source.h"
#include "udp.h"

#define RX_BATCH 64
#define RX_SIZE 16384

enum src_state { SRC_IDLE, SRC_IN_USE, SRC_FAILED, SRC_TRYING };

struct source {
    char url[SINK_URL_MAX];
    const struct input_ops *ops;        /* a link input, or NULL for UDP/RTP */
    void *ctx;
    int fd;
    enum src_state state;
    uint64_t failures;
    uint64_t rx_overflow, rx_truncated;
};

struct sources {
    struct source list[SOURCES_MAX];
    int n;
    int active;
    int switch_ms, retry_s;
    int64_t last_rx_ns;                 /* from the source in use */
    int64_t since_ns;                   /* when the source in use was taken */
    int64_t last_try_ns;                /* the primary last tried, while on a backup */
    uint64_t switches;
    pthread_mutex_t lock;               /* the stats readers, and the probe */

    /* the UDP batch of the source in use */
    uint8_t *bufs;
    struct mmsghdr mm[RX_BATCH];
    struct iovec iov[RX_BATCH];
    char ctl[RX_BATCH][CMSG_SPACE(sizeof(uint32_t))];
    int batch_n, batch_i;

    /* the primary tried again in a thread while a backup is in use:
     * opened there, and handed over once it delivers */
    pthread_t probe_th;
    bool probe_running;
    _Atomic int probe_result;           /* 0 running, 1 delivered, -1 failed */
    const struct input_ops *probe_ops;
    void *probe_ctx;
    int probe_fd;
};

static int64_t now_mono(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

struct sources *sources_new(int switch_ms, int retry_s)
{
    struct sources *s = calloc(1, sizeof(*s));

    if (!s)
        return NULL;
    s->bufs = malloc((size_t)RX_BATCH * RX_SIZE);
    if (!s->bufs) {
        free(s);
        return NULL;
    }
    s->switch_ms = switch_ms > 0 ? switch_ms : 3000;
    s->retry_s = retry_s > 0 ? retry_s : 60;
    s->probe_fd = -1;
    pthread_mutex_init(&s->lock, NULL);
    return s;
}

bool sources_add(struct sources *s, const char *url, char *err, size_t errlen)
{
    if (s->n >= SOURCES_MAX) {
        snprintf(err, errlen, "at most %d sources", SOURCES_MAX);
        return false;
    }
    if (!input_is_live(url)) {
        snprintf(err, errlen, "%s: a source is udp://, rtp://, srt:// or rist://", url);
        return false;
    }
    if (strlen(url) >= SINK_URL_MAX) {
        snprintf(err, errlen, "URL too long");
        return false;
    }
    {
        struct source *src = &s->list[s->n];

        memset(src, 0, sizeof(*src));
        snprintf(src->url, sizeof(src->url), "%s", url);
        src->ops = input_ops_for(url);
        src->fd = -1;
        if (!src->ops && (!strncmp(url, "srt://", 6) || !strncmp(url, "rist://", 7))) {
            snprintf(err, errlen, "this build has no %.4s input (transports: %s)", url, input_transports());
            return false;
        }
        if (!src->ops) {
            struct udp_addr a;

            if (udp_parse(url, &a, err, errlen) < 0)
                return false;
        }
    }
    s->n++;
    return true;
}

int sources_count(const struct sources *s)
{
    return s->n;
}

const char *sources_url(const struct sources *s, int idx)
{
    return idx >= 0 && idx < s->n ? s->list[idx].url : "";
}

/* ---- opening and closing one ----------------------------------------------- */

static bool open_one(struct source *src, const struct input_ops **ops, void **ctx, int *fd,
                     char *err, size_t errlen)
{
    *ops = src->ops;
    *ctx = NULL;
    *fd = -1;
    if (src->ops) {
        *ctx = src->ops->open(src->url, err, errlen);
        return *ctx != NULL;
    }
    {
        struct udp_addr a;

        if (udp_parse(src->url, &a, err, errlen) < 0)
            return false;
        *fd = udp_open_rx(&a, err, errlen);
        return *fd >= 0;
    }
}

static void close_one(const struct input_ops *ops, void *ctx, int fd)
{
    if (ops && ctx)
        ops->close(ctx);
    if (fd >= 0)
        close(fd);
}

static bool take(struct sources *s, int idx, char *err, size_t errlen)
{
    struct source *src = &s->list[idx];

    if (!open_one(src, &src->ops, &src->ctx, &src->fd, err, errlen)) {
        src->state = SRC_FAILED;
        src->failures++;
        return false;
    }
    pthread_mutex_lock(&s->lock);
    src->state = SRC_IN_USE;
    s->active = idx;
    s->since_ns = now_mono();
    s->last_rx_ns = 0;
    s->batch_n = s->batch_i = 0;
    pthread_mutex_unlock(&s->lock);
    return true;
}

static void drop(struct sources *s, int idx, enum src_state st)
{
    struct source *src = &s->list[idx];

    close_one(src->ops, src->ctx, src->fd);
    src->ctx = NULL;
    src->fd = -1;
    pthread_mutex_lock(&s->lock);
    src->state = st;
    pthread_mutex_unlock(&s->lock);
}

bool sources_start(struct sources *s, char *err, size_t errlen)
{
    if (s->n == 0) {
        snprintf(err, errlen, "no source");
        return false;
    }
    for (int i = 0; i < s->n; i++) {
        char e[200];

        if (take(s, i, e, sizeof(e))) {
            if (i > 0)
                log_warn("source: %s could not be opened (%s); using %s", s->list[0].url, err, s->list[i].url);
            return true;
        }
        snprintf(err, errlen, "%s", e);
        log_err("source %d: %s: %s", i + 1, s->list[i].url, e);
    }
    return false;
}

/* ---- the primary tried again in the background ---------------------------- */

static void *probe_thread(void *arg)
{
    struct sources *s = arg;
    struct source *src = &s->list[0];
    const struct input_ops *ops;
    void *ctx;
    int fd;
    char err[200];
    uint8_t *buf = malloc(RX_SIZE);
    int64_t t0 = now_mono();
    int result = -1;

    prctl(PR_SET_NAME, "cc3-src-probe");
    if (!buf || !open_one(src, &ops, &ctx, &fd, err, sizeof(err))) {
        free(buf);
        atomic_store(&s->probe_result, -1);
        return NULL;
    }
    /* up to 5 s for the first datagram */
    while (now_mono() - t0 < 5000000000LL) {
        int n;

        if (ops)
            n = ops->recv(ctx, buf, INPUT_DGRAM_MAX);
        else
            n = (int)recv(fd, buf, RX_SIZE, 0);     /* 200 ms timeout on the socket */
        if (n > 0) {
            result = 1;
            break;
        }
        if (n < 0 && ops)
            break;
    }
    free(buf);
    if (result == 1) {
        s->probe_ops = ops;
        s->probe_ctx = ctx;
        s->probe_fd = fd;
    } else {
        close_one(ops, ctx, fd);
    }
    atomic_store(&s->probe_result, result);
    return NULL;
}

static void probe_start(struct sources *s)
{
    if (s->probe_running)
        return;
    atomic_store(&s->probe_result, 0);
    pthread_mutex_lock(&s->lock);
    s->list[0].state = SRC_TRYING;
    pthread_mutex_unlock(&s->lock);
    if (pthread_create(&s->probe_th, NULL, probe_thread, s) == 0)
        s->probe_running = true;
    else
        s->list[0].state = SRC_FAILED;
}

/* Returns true when the primary is back and has been taken over. */
static bool probe_check(struct sources *s)
{
    int r;

    if (!s->probe_running)
        return false;
    r = atomic_load(&s->probe_result);
    if (r == 0)
        return false;
    pthread_join(s->probe_th, NULL);
    s->probe_running = false;
    if (r < 0) {
        pthread_mutex_lock(&s->lock);
        s->list[0].state = SRC_FAILED;
        s->list[0].failures++;
        pthread_mutex_unlock(&s->lock);
        return false;
    }
    /* delivered: the backup goes, the primary's open input is taken as is */
    drop(s, s->active, SRC_IDLE);
    pthread_mutex_lock(&s->lock);
    s->list[0].ops = s->probe_ops;
    s->list[0].ctx = s->probe_ctx;
    s->list[0].fd = s->probe_fd;
    s->list[0].state = SRC_IN_USE;
    s->probe_ctx = NULL;
    s->probe_fd = -1;
    s->active = 0;
    s->since_ns = now_mono();
    s->last_rx_ns = 0;
    s->batch_n = s->batch_i = 0;
    s->switches++;
    pthread_mutex_unlock(&s->lock);
    log_info("source: back on %s", s->list[0].url);
    return true;
}

/* ---- receiving ------------------------------------------------------------- */

static int udp_batch(struct sources *s, struct source *src)
{
    for (int i = 0; i < RX_BATCH; i++) {
        s->iov[i].iov_base = s->bufs + (size_t)i * RX_SIZE;
        s->iov[i].iov_len = RX_SIZE;
        memset(&s->mm[i], 0, sizeof(s->mm[i]));
        s->mm[i].msg_hdr.msg_iov = &s->iov[i];
        s->mm[i].msg_hdr.msg_iovlen = 1;
        s->mm[i].msg_hdr.msg_control = s->ctl[i];
        s->mm[i].msg_hdr.msg_controllen = sizeof(s->ctl[i]);
    }
    {
        int n = recvmmsg(src->fd, s->mm, RX_BATCH, MSG_WAITFORONE, NULL);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return 0;
            log_err("source: recvmmsg: %s", strerror(errno));
            return -1;
        }
        for (int i = 0; i < n; i++) {
            struct msghdr *h = &s->mm[i].msg_hdr;

            for (struct cmsghdr *cm = CMSG_FIRSTHDR(h); cm; cm = CMSG_NXTHDR(h, cm))
                if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SO_RXQ_OVFL) {
                    uint32_t drops;

                    memcpy(&drops, CMSG_DATA(cm), sizeof(drops));
                    src->rx_overflow = drops;
                }
            if (h->msg_flags & MSG_TRUNC)
                src->rx_truncated++;
        }
        return n;
    }
}

static void switch_next(struct sources *s, int64_t now_ns, const char *why)
{
    int from = s->active;
    char err[200];

    log_warn("source: %s: %s; switching", s->list[from].url, why);
    drop(s, from, SRC_FAILED);
    s->list[from].failures++;
    for (int k = 1; k <= s->n; k++) {
        int idx = (from + k) % s->n;

        if (idx == 0 && s->probe_running)
            continue;                   /* the primary is being tried already */
        if (take(s, idx, err, sizeof(err))) {
            pthread_mutex_lock(&s->lock);
            s->switches++;
            s->last_rx_ns = now_ns;     /* the new one gets a full grace period */
            pthread_mutex_unlock(&s->lock);
            log_info("source: now %s (%d of %d)", s->list[idx].url, idx + 1, s->n);
            return;
        }
        log_warn("source: %s: %s", s->list[idx].url, err);
    }
    /* none would open: the old one is tried again next time round */
    if (take(s, from, err, sizeof(err)))
        s->last_rx_ns = now_ns;
}

int sources_recv(struct sources *s, uint8_t *buf, int size, int64_t now_ns)
{
    struct source *src;
    int n = 0;

    if (s->active != 0)
        probe_check(s);                 /* back on the primary when it delivered */
    src = &s->list[s->active];
    if (src->state != SRC_IN_USE) {
        switch_next(s, now_ns, "not open");
        return 0;
    }

    if (src->ops) {
        n = src->ops->recv(src->ctx, buf, size < INPUT_DGRAM_MAX ? size : INPUT_DGRAM_MAX);
        if (n < 0) {
            switch_next(s, now_ns, "finished");
            return 0;
        }
    } else {
        if (s->batch_i >= s->batch_n) {
            s->batch_n = udp_batch(s, src);
            s->batch_i = 0;
            if (s->batch_n < 0) {
                switch_next(s, now_ns, "socket error");
                return 0;
            }
        }
        if (s->batch_i < s->batch_n) {
            int i = s->batch_i++;
            int len = (int)s->mm[i].msg_len;

            if (len > size)
                len = size;
            memcpy(buf, s->iov[i].iov_base, (size_t)len);
            n = len;
        }
    }

    if (n > 0) {
        s->last_rx_ns = now_ns;
        return n;
    }
    /* nothing: is it time to give up on this one? */
    if (s->n > 1) {
        int64_t idle_since = s->last_rx_ns ? s->last_rx_ns : s->since_ns;

        if (now_ns - idle_since > (int64_t)s->switch_ms * 1000000LL)
            switch_next(s, now_ns, "no packets");
    }
    /* on a backup: try the primary now and then */
    if (s->active != 0 && !s->probe_running && s->list[0].state != SRC_IN_USE &&
        now_ns - s->last_try_ns > (int64_t)s->retry_s * 1000000000LL) {
        s->last_try_ns = now_ns;
        probe_start(s);
    }
    return 0;
}

/* The primary is also retried while data flows on a backup: the check
 * above only runs on an idle moment, so it is repeated here. */
void sources_tick(struct sources *s, int64_t now_ns)
{
    if (s->active != 0 && !s->probe_running && s->list[0].state != SRC_IN_USE &&
        now_ns - s->last_try_ns > (int64_t)s->retry_s * 1000000000LL) {
        s->last_try_ns = now_ns;
        probe_start(s);
    }
}

/* ---- statistics ------------------------------------------------------------- */

void sources_stats(struct sources *s, struct source_stats *st)
{
    struct source *src;

    memset(st, 0, sizeof(*st));
    pthread_mutex_lock(&s->lock);
    st->active = s->active;
    st->switches = s->switches;
    st->last_rx_ns = s->last_rx_ns;
    src = &s->list[s->active];
    st->rx_overflow = src->rx_overflow;
    st->rx_truncated = src->rx_truncated;
    if (src->ops && src->ctx && src->state == SRC_IN_USE) {
        struct input_stats is;

        memset(&is, 0, sizeof(is));
        src->ops->stats(src->ctx, &is);
        st->link = true;
        snprintf(st->link_transport, sizeof(st->link_transport), "%s", src->ops->name);
        snprintf(st->link_state, sizeof(st->link_state), "%s", is.state);
        st->link_connected = is.connected;
        st->rtt_ms = is.rtt_ms;
        st->lost = is.lost;
        st->recovered = is.retransmitted;
        st->reconnects = is.reconnects;
        st->link_errors = is.errors;
    }
    pthread_mutex_unlock(&s->lock);
}

void sources_json(struct sources *s, FILE *f, void (*json_str)(FILE *, const char *))
{
    static const char *const names[] = { "standing by", "in use", "failed", "trying" };

    pthread_mutex_lock(&s->lock);
    fprintf(f, "[");
    for (int i = 0; i < s->n; i++) {
        fprintf(f, "%s{\"url\": ", i ? ", " : "");
        json_str(f, s->list[i].url);
        fprintf(f, ", \"state\": \"%s\", \"failures\": %llu}", names[s->list[i].state],
                (unsigned long long)s->list[i].failures);
    }
    fprintf(f, "]");
    pthread_mutex_unlock(&s->lock);
}

void sources_free(struct sources *s)
{
    if (!s)
        return;
    if (s->probe_running) {
        pthread_join(s->probe_th, NULL);
        if (atomic_load(&s->probe_result) == 1)
            close_one(s->probe_ops, s->probe_ctx, s->probe_fd);
    }
    for (int i = 0; i < s->n; i++)
        close_one(s->list[i].ops, s->list[i].ctx, s->list[i].fd);
    pthread_mutex_destroy(&s->lock);
    free(s->bufs);
    free(s);
}
