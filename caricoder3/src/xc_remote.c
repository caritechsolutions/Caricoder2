/*
 * caricoder3 - a channel's video transcoding done in the card process
 * Copyright (c) 2026 CariTech Solutions
 *
 * Stands where the video half (xc_video.c) stands in xcode.c, and hands
 * the work to the card process over its socket (card.h): PES packets go
 * there, the new PES packets come back on a thread of their own and go
 * to the mux as the video half's would.
 *
 * When the card process goes away, the video stops for the moment: input
 * is dropped, and every half second the socket is tried again. Once it
 * answers, the session is set up afresh, told where the old output left
 * off (so decode times still only move forward), and fed from the next
 * key frame. A channel that has lost the card process CARD_FAILS times in
 * CARD_FAIL_WINDOW_S, or cannot reach it for WAIT_MAX_NS, does the work
 * itself from then on, on the card, as before there was a card process.
 */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "card.h"
#include "clock.h"
#include "log.h"
#include "thumb.h"
#include "xcode.h"

#define RETRY_NS        500000000LL     /* the socket tried again this often */
#define WAIT_MAX_NS     30000000000LL   /* not reachable this long: do the work here */
#define HANDSHAKE_S     5               /* the card process answers a session's start in this */
#define SEND_TIMEOUT_S  10              /* a card process that takes no input this long is stuck */
#define FINISH_WAIT_NS  1500000000LL
#define THUMB_STALE_NS  10000000000LL

static char g_dir[512];
static char g_name[64];
static bool g_all;

void xcode_use_card(const char *run_dir, const char *name, bool all)
{
    snprintf(g_dir, sizeof(g_dir), "%s", run_dir ? run_dir : "");
    snprintf(g_name, sizeof(g_name), "%s", name ? name : "");
    g_all = all;
}

void card_socket_path(const char *run_dir, int card, char *out, size_t len)
{
    snprintf(out, len, "%.400s/card-%d.sock", run_dir, card);
}

/* ---- messages -------------------------------------------------------- */

bool card_send(int fd, uint32_t type, const void *a, size_t alen, const void *b, size_t blen)
{
    struct card_hdr h = { type, (uint32_t)(alen + blen) };
    struct iovec iov[3] = { { &h, sizeof(h) }, { (void *)a, alen }, { (void *)b, blen } };
    struct msghdr m;
    size_t left = sizeof(h) + alen + blen;
    int at = 0;

    if (alen + blen > CARD_MSG_MAX)
        return false;
    while (left > 0) {
        ssize_t n;

        memset(&m, 0, sizeof(m));
        m.msg_iov = iov + at;
        m.msg_iovlen = (size_t)(3 - at);
        n = sendmsg(fd, &m, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        left -= (size_t)n;
        while (at < 3 && (size_t)n >= iov[at].iov_len) {
            n -= (ssize_t)iov[at].iov_len;
            iov[at].iov_len = 0;
            at++;
        }
        if (at < 3) {
            iov[at].iov_base = (uint8_t *)iov[at].iov_base + n;
            iov[at].iov_len -= (size_t)n;
        }
    }
    return true;
}

static bool read_all(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;

    while (len > 0) {
        ssize_t n = read(fd, p, len);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

bool card_recv(int fd, struct card_hdr *h, uint8_t **payload)
{
    *payload = NULL;
    if (!read_all(fd, h, sizeof(*h)) || h->len > CARD_MSG_MAX)
        return false;
    if (!h->len)
        return true;
    *payload = malloc(h->len);
    if (!*payload)
        return false;
    if (!read_all(fd, *payload, h->len)) {
        free(*payload);
        *payload = NULL;
        return false;
    }
    return true;
}

/* ---- the session ------------------------------------------------------ */

struct rvxc {
    struct xcode_cfg cfg;
    xcode_emit_fn emit;
    void *opaque;
    struct xcode_stats *st;             /* the worker's own (xcode.c) */
    char sock[600];

    /* the worker's */
    int fd;                             /* -1: not connected */
    pthread_t rd;
    int64_t fails[CARD_FAILS];          /* when the last connections broke */
    int64_t waiting_since;              /* ns; 0: not waiting */
    int64_t next_try;
    int64_t thumb_asked;
    bool skip_to_key;
    struct vxc *local;                  /* doing the work here after all */
    struct card_stats base;             /* what earlier sessions counted */

    pthread_mutex_t lock;
    pthread_cond_t cond;
    /* under the lock: the reader's */
    bool broken;                        /* the connection is at its end */
    bool finished;
    struct card_stats rs;
    bool have_last;
    int64_t last_dts90;
    stc_t last_t1;
    bool thumb_pending;
};

static void add_stats(struct card_stats *to, const struct card_stats *from)
{
    to->frames_in += from->frames_in;
    to->frames_out += from->frames_out;
    to->units_out += from->units_out;
    to->errors += from->errors;
    to->restarts += from->restarts;
}

static void *reader(void *arg)
{
    struct rvxc *r = arg;
    struct card_hdr h;
    uint8_t *p;

    while (card_recv(r->fd, &h, &p)) {
        if (h.type == CM_OUT && h.len >= sizeof(struct card_unit)) {
            struct card_unit cu;
            struct es_unit *u;

            memcpy(&cu, p, sizeof(cu));
            if (cu.len == h.len - sizeof(cu) && (u = malloc(sizeof(*u) + cu.len)) != NULL) {
                memset(u, 0, sizeof(*u));
                u->pid = cu.pid;
                u->kind = cu.kind;
                u->rai = cu.rai;
                u->pts_off = cu.pts_off;
                u->dts_off = cu.dts_off;
                u->pts = cu.pts;
                u->dts = cu.dts;
                u->t0 = cu.t0;
                u->t1 = cu.t1;
                u->len = cu.len;
                memcpy(u->data, p + sizeof(cu), cu.len);
                pthread_mutex_lock(&r->lock);
                r->have_last = true;
                r->last_dts90 = cu.dts;
                r->last_t1 = cu.t1;
                pthread_mutex_unlock(&r->lock);
                r->emit(r->opaque, u);
            }
        } else if (h.type == CM_STATS && h.len == sizeof(struct card_stats)) {
            pthread_mutex_lock(&r->lock);
            memcpy(&r->rs, p, sizeof(r->rs));
            pthread_mutex_unlock(&r->lock);
        } else if (h.type == CM_THUMB && h.len >= sizeof(struct card_thumb)) {
            struct card_thumb t;

            memcpy(&t, p, sizeof(t));
            if (t.width >= 16 && t.height >= 16 && t.width <= 4096 && t.height <= 4096 &&
                h.len - sizeof(t) == (size_t)t.width * t.height * 3 / 2)
                thumb_from_small(p + sizeof(t), t.width, t.height);
            pthread_mutex_lock(&r->lock);
            r->thumb_pending = false;
            pthread_mutex_unlock(&r->lock);
        } else if (h.type == CM_FINISHED) {
            pthread_mutex_lock(&r->lock);
            r->finished = true;
            pthread_cond_broadcast(&r->cond);
            pthread_mutex_unlock(&r->lock);
        }
        free(p);
    }
    pthread_mutex_lock(&r->lock);
    r->broken = true;
    pthread_cond_broadcast(&r->cond);
    pthread_mutex_unlock(&r->lock);
    return NULL;
}

/* 1: connected and the session set up; 0: the card process is not there
 * (or broke off); -1: it is there and cannot do this job (err says why). */
static int connect_card(struct rvxc *r, char *err, size_t errlen)
{
    struct sockaddr_un sa;
    struct card_hello hi;
    struct card_ready rd;
    struct card_hdr h;
    struct timeval tv = { HANDSHAKE_S, 0 };
    uint8_t *p = NULL;
    int fd;

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return 0;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(r->sock) >= sizeof(sa.sun_path)) {
        close(fd);
        return 0;
    }
    memcpy(sa.sun_path, r->sock, strlen(r->sock) + 1);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return 0;
    }
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tv.tv_sec = SEND_TIMEOUT_S;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    memset(&hi, 0, sizeof(hi));
    hi.proto = CARD_PROTO;
    hi.size = sizeof(hi);
    hi.pid = r->cfg.pid;
    hi.out_pid = r->cfg.out_pid;
    hi.in = r->cfg.in;
    hi.out = r->cfg.out;
    hi.bitrate = r->cfg.bitrate;
    hi.width = r->cfg.width;
    hi.height = r->cfg.height;
    hi.fit = r->cfg.fit;
    hi.decoder = r->cfg.decoder;
    hi.encoder = r->cfg.encoder;
    hi.gpu = r->cfg.gpu;
    hi.vbv_ms = r->cfg.vbv_ms;
    hi.gop_ms = r->cfg.gop_ms;
    hi.bframes = r->cfg.bframes;
    hi.first_rendition = r->cfg.first_rendition;
    snprintf(hi.preset, sizeof(hi.preset), "%s", r->cfg.preset ? r->cfg.preset : "");
    snprintf(hi.profile, sizeof(hi.profile), "%s", r->cfg.profile ? r->cfg.profile : "");
    snprintf(hi.channel, sizeof(hi.channel), "%s", g_name);
    if (!card_send(fd, CM_HELLO, &hi, sizeof(hi), NULL, 0) || !card_recv(fd, &h, &p) ||
        h.type != CM_READY || h.len != sizeof(rd)) {
        free(p);
        close(fd);
        return 0;
    }
    memcpy(&rd, p, sizeof(rd));
    free(p);
    rd.err[sizeof(rd.err) - 1] = 0;
    rd.decoder[sizeof(rd.decoder) - 1] = 0;
    rd.encoder[sizeof(rd.encoder) - 1] = 0;
    if (!rd.ok) {
        snprintf(err, errlen, "%s", rd.err);
        close(fd);
        return -1;
    }
    tv.tv_sec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    snprintf(r->st->decoder, sizeof(r->st->decoder), "%s", rd.decoder);
    snprintf(r->st->encoder, sizeof(r->st->encoder), "%s", rd.encoder);

    r->fd = fd;
    pthread_mutex_lock(&r->lock);
    r->broken = false;
    r->finished = false;
    r->thumb_pending = false;
    memset(&r->rs, 0, sizeof(r->rs));
    pthread_mutex_unlock(&r->lock);
    if (pthread_create(&r->rd, NULL, reader, r) != 0) {
        close(fd);
        r->fd = -1;
        return 0;
    }
    return 1;
}

/* The connection is over: the reader is gone, what it counted is kept. */
static void disconnect(struct rvxc *r)
{
    if (r->fd < 0)
        return;
    shutdown(r->fd, SHUT_RDWR);
    pthread_join(r->rd, NULL);
    close(r->fd);
    r->fd = -1;
    pthread_mutex_lock(&r->lock);
    add_stats(&r->base, &r->rs);
    memset(&r->rs, 0, sizeof(r->rs));
    r->thumb_pending = false;
    pthread_mutex_unlock(&r->lock);
}

static void pull_stats(struct rvxc *r)
{
    struct card_stats s;

    pthread_mutex_lock(&r->lock);
    s = r->rs;
    pthread_mutex_unlock(&r->lock);
    r->st->frames_in = r->base.frames_in + s.frames_in;
    r->st->frames_out = r->base.frames_out + s.frames_out;
    r->st->units_out = r->base.units_out + s.units_out;
    r->st->errors = r->base.errors + s.errors;
    r->st->restarts = r->base.restarts + s.restarts;
    if (s.width > 0) {
        r->st->width = s.width;
        r->st->height = s.height;
    }
    if (s.encoder[0]) {
        s.encoder[sizeof(s.encoder) - 1] = 0;
        s.decoder[sizeof(s.decoder) - 1] = 0;
        snprintf(r->st->encoder, sizeof(r->st->encoder), "%s", s.encoder);
        snprintf(r->st->decoder, sizeof(r->st->decoder), "%s", s.decoder);
    }
    r->st->failed = s.failed;
}

/* From now on the work is done here. */
static void go_local(struct rvxc *r, const char *why)
{
    char err[200] = "";
    struct xcode_cfg c = r->cfg;
    bool have;
    int64_t dts90;
    stc_t t1;

    disconnect(r);
    log_err("PID 0x%04X: %s; this channel transcodes its video itself from now on "
            "(restart the channel to use the card process again)", r->cfg.pid, why);
    r->local = vxc_new(&c, r->emit, r->opaque, r->st, err, sizeof(err));
    r->st->card = XC_CARD_OWN;
    r->waiting_since = 0;
    if (!r->local) {
        log_err("PID 0x%04X: cannot transcode here either (%s); the stream is silent", r->cfg.pid, err);
        r->st->failed = true;
        return;
    }
    pthread_mutex_lock(&r->lock);
    have = r->have_last;
    dts90 = r->last_dts90;
    t1 = r->last_t1;
    pthread_mutex_unlock(&r->lock);
    if (have)
        vxc_start_after(r->local, dts90, t1);
    r->skip_to_key = true;
}

static void lost(struct rvxc *r, int64_t now)
{
    int n = 0;

    disconnect(r);
    r->st->card_lost++;
    memmove(r->fails + 1, r->fails, sizeof(r->fails[0]) * (CARD_FAILS - 1));
    r->fails[0] = now;
    for (int k = 0; k < CARD_FAILS; k++)
        n += r->fails[k] && now - r->fails[k] <= (int64_t)CARD_FAIL_WINDOW_S * 1000000000LL;
    if (n >= CARD_FAILS) {
        char why[120];

        snprintf(why, sizeof(why), "the card process went away %d times in %d minutes",
                 CARD_FAILS, CARD_FAIL_WINDOW_S / 60);
        go_local(r, why);
        return;
    }
    log_warn("PID 0x%04X: the card process went away; no video until it is back", r->cfg.pid);
    r->st->card = XC_CARD_WAITING;
    r->waiting_since = now;
    r->next_try = now + RETRY_NS;
}

/* Connected again: the session goes on where the last left off. */
static void resumed(struct rvxc *r)
{
    struct card_after a;
    bool have;

    pthread_mutex_lock(&r->lock);
    have = r->have_last;
    a.dts90 = r->last_dts90;
    a.t1 = r->last_t1;
    pthread_mutex_unlock(&r->lock);
    if (have)
        card_send(r->fd, CM_START_AFTER, &a, sizeof(a), NULL, 0);
    r->skip_to_key = true;
    r->waiting_since = 0;
    r->st->card = XC_CARD_SHARED;
    log_info("PID 0x%04X: the card process is back; video from the next key frame", r->cfg.pid);
}

bool rvxc_wanted(const struct xcode_cfg *cfg)
{
    if (!g_dir[0] || !codec_is_video(cfg->in))
        return false;
    if (g_all)
        return true;
    if (cfg->encoder == WHERE_CPU)
        return false;
    return codec_find(CODEC_VIDEO, CODEC_ENCODER, cfg->out, WHERE_NVIDIA, 0) != NULL;
}

struct rvxc *rvxc_new(const struct xcode_cfg *cfg, xcode_emit_fn emit, void *opaque,
                      struct xcode_stats *st, char *err, size_t errlen)
{
    struct rvxc *r = calloc(1, sizeof(*r));
    int rc;

    if (!r)
        return NULL;
    r->cfg = *cfg;
    r->emit = emit;
    r->opaque = opaque;
    r->st = st;
    r->fd = -1;
    r->last_dts90 = NOPTS;
    pthread_mutex_init(&r->lock, NULL);
    {
        pthread_condattr_t ca;

        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        pthread_cond_init(&r->cond, &ca);
        pthread_condattr_destroy(&ca);
    }
    card_socket_path(g_dir, cfg->gpu, r->sock, sizeof(r->sock));
    snprintf(st->decoder, sizeof(st->decoder), "card");
    snprintf(st->encoder, sizeof(st->encoder), "card");
    rc = connect_card(r, err, errlen);
    if (rc < 0) {
        rvxc_free(r);
        return NULL;
    }
    if (rc == 0) {
        log_warn("PID 0x%04X: the card process (%s) is not answering yet; no video until it does",
                 cfg->pid, r->sock);
        st->card = XC_CARD_WAITING;
        r->waiting_since = clock_now_ns();
        r->next_try = r->waiting_since + RETRY_NS;
    } else {
        st->card = XC_CARD_SHARED;
        log_info("PID 0x%04X: video transcoded in the card process (card %d)", cfg->pid, cfg->gpu);
    }
    return r;
}

void rvxc_unit(struct rvxc *r, const struct es_unit *u)
{
    struct card_unit cu;
    int64_t now;
    bool broken;
    int off;

    if (r->local) {
        if (r->skip_to_key) {
            off = pes_payload_offset(u->data, u->len);
            if (off < 0 || !codec_es_has_key(r->cfg.in, u->data + off, u->len - (size_t)off, true)) {
                r->st->units_in++;
                return;
            }
            r->skip_to_key = false;
        }
        vxc_unit(r->local, u);
        return;
    }
    r->st->units_in++;
    now = clock_now_ns();
    if (r->fd >= 0) {
        pthread_mutex_lock(&r->lock);
        broken = r->broken;
        pthread_mutex_unlock(&r->lock);
        pull_stats(r);
        if (broken) {
            lost(r, now);
            if (r->local) {
                r->st->units_in--;          /* counted again there */
                rvxc_unit(r, u);
                return;
            }
        }
    }
    if (r->fd < 0) {
        char err[200] = "";
        int rc;

        if (r->local || now < r->next_try)
            return;
        rc = connect_card(r, err, sizeof(err));
        if (rc > 0) {
            resumed(r);
        } else if (rc < 0 || now - r->waiting_since > WAIT_MAX_NS) {
            char why[300];

            if (rc < 0)
                snprintf(why, sizeof(why), "the card process cannot take this video (%s)", err);
            else
                snprintf(why, sizeof(why), "the card process has not answered for %d s",
                         (int)(WAIT_MAX_NS / 1000000000LL));
            go_local(r, why);
            if (r->local) {
                r->st->units_in--;
                rvxc_unit(r, u);
            }
            return;
        } else {
            r->next_try = now + RETRY_NS;
            return;
        }
    }
    if (r->skip_to_key) {
        off = pes_payload_offset(u->data, u->len);
        if (off < 0 || !codec_es_has_key(r->cfg.in, u->data + off, u->len - (size_t)off, true))
            return;
        r->skip_to_key = false;
    }
    if (r->cfg.first_rendition && thumb_enabled()) {
        bool ask;

        pthread_mutex_lock(&r->lock);
        if (r->thumb_pending && now - r->thumb_asked > THUMB_STALE_NS)
            r->thumb_pending = false;
        ask = !r->thumb_pending;
        pthread_mutex_unlock(&r->lock);
        if (ask && thumb_due()) {
            pthread_mutex_lock(&r->lock);
            r->thumb_pending = true;
            pthread_mutex_unlock(&r->lock);
            r->thumb_asked = now;
            card_send(r->fd, CM_THUMB_REQ, NULL, 0, NULL, 0);
        }
    }
    memset(&cu, 0, sizeof(cu));
    cu.pid = u->pid;
    cu.kind = u->kind;
    cu.rai = u->rai;
    cu.pts_off = u->pts_off;
    cu.dts_off = u->dts_off;
    cu.pts = u->pts;
    cu.dts = u->dts;
    cu.t0 = u->t0;
    cu.t1 = u->t1;
    cu.len = (uint32_t)u->len;
    if (!card_send(r->fd, CM_UNIT, &cu, sizeof(cu), u->data, u->len)) {
        /* gone, or stuck: either way this connection is done with */
        shutdown(r->fd, SHUT_RDWR);
    }
}

void rvxc_reset(struct rvxc *r)
{
    if (r->local)
        vxc_reset(r->local);
    else if (r->fd >= 0)
        card_send(r->fd, CM_RESET, NULL, 0, NULL, 0);
}

void rvxc_finish(struct rvxc *r)
{
    struct timespec ts;

    if (r->local) {
        vxc_finish(r->local);
        return;
    }
    if (r->fd < 0 || !card_send(r->fd, CM_FINISH, NULL, 0, NULL, 0))
        return;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += FINISH_WAIT_NS / 1000000000LL;
    ts.tv_nsec += FINISH_WAIT_NS % 1000000000LL;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&r->lock);
    while (!r->finished && !r->broken)
        if (pthread_cond_timedwait(&r->cond, &r->lock, &ts) == ETIMEDOUT)
            break;
    pthread_mutex_unlock(&r->lock);
    pull_stats(r);
}

bool rvxc_last_out(const struct rvxc *r, int64_t *dts90, stc_t *t1)
{
    bool have;

    if (r->local)
        return vxc_last_out(r->local, dts90, t1);
    pthread_mutex_lock((pthread_mutex_t *)&r->lock);
    have = r->have_last;
    *dts90 = r->last_dts90;
    *t1 = r->last_t1;
    pthread_mutex_unlock((pthread_mutex_t *)&r->lock);
    return have;
}

void rvxc_start_after(struct rvxc *r, int64_t dts90, stc_t t1)
{
    struct card_after a = { dts90, t1 };

    if (r->local) {
        vxc_start_after(r->local, dts90, t1);
        return;
    }
    pthread_mutex_lock(&r->lock);
    r->have_last = true;
    r->last_dts90 = dts90;
    r->last_t1 = t1;
    pthread_mutex_unlock(&r->lock);
    if (r->fd >= 0)
        card_send(r->fd, CM_START_AFTER, &a, sizeof(a), NULL, 0);
}

void rvxc_free(struct rvxc *r)
{
    if (!r)
        return;
    disconnect(r);
    vxc_free(r->local);
    pthread_cond_destroy(&r->cond);
    pthread_mutex_destroy(&r->lock);
    free(r);
}
