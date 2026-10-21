/*
 * caricoder3 - the card process (see card.h)
 * Copyright (c) 2026 CariTech Solutions
 *
 *   caricoder3 card --card N --socket PATH [--stats-file FILE]
 *
 * Listens on PATH. Each connection is one transcoding session, run on a
 * thread of its own by the same code a channel runs in itself
 * (xc_video.c), on card N. All sessions share the process's one CUDA
 * context for the card (nv_common.c).
 */
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "card.h"
#include "clock.h"
#include "log.h"
#include "thumb.h"
#include "xcode.h"

#define SEND_TIMEOUT_S 5        /* a channel that takes nothing back this long is stuck */
#define STATS_NS       1000000000LL

struct sess {
    struct sess *next;
    int fd;
    struct card_hello hi;
    struct vxc *v;
    struct xcode_stats st;      /* the session's thread's */
    struct xcode_stats pub;     /* a copy for the stats file, under g_lock */
    bool thumb_want;
    bool dead;                  /* a send failed */
    struct card_stats sent;
    int64_t sent_ns;
};

static int g_card;
static atomic_int g_stop;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct sess *g_sessions;
static unsigned long long g_started, g_ended;

static void on_signal(int sig)
{
    (void)sig;
    atomic_store(&g_stop, 1);
}

static void stats_of(const struct sess *s, struct card_stats *c)
{
    memset(c, 0, sizeof(*c));
    c->frames_in = s->st.frames_in;
    c->frames_out = s->st.frames_out;
    c->units_out = s->st.units_out;
    c->errors = s->st.errors;
    c->restarts = s->st.restarts;
    c->width = s->st.width;
    c->height = s->st.height;
    c->failed = s->st.failed;
    snprintf(c->decoder, sizeof(c->decoder), "%s", s->st.decoder);
    snprintf(c->encoder, sizeof(c->encoder), "%s", s->st.encoder);
}

static void send_or_die(struct sess *s, uint32_t type, const void *a, size_t alen,
                        const void *b, size_t blen)
{
    if (s->dead)
        return;
    if (!card_send(s->fd, type, a, alen, b, blen)) {
        /* the channel is gone or stuck: the session ends at its next read */
        s->dead = true;
        shutdown(s->fd, SHUT_RDWR);
    }
}

static void send_stats(struct sess *s, bool force)
{
    struct card_stats c;
    int64_t now = clock_now_ns();

    stats_of(s, &c);
    pthread_mutex_lock(&g_lock);
    s->pub = s->st;
    pthread_mutex_unlock(&g_lock);
    if (!force && !memcmp(&c, &s->sent, sizeof(c)) && now - s->sent_ns < STATS_NS)
        return;
    if (!force && now - s->sent_ns < STATS_NS / 5 && c.failed == s->sent.failed &&
        c.width == s->sent.width)
        return;                 /* five a second is plenty */
    s->sent = c;
    s->sent_ns = now;
    send_or_die(s, CM_STATS, &c, sizeof(c), NULL, 0);
}

static void on_out(void *opaque, struct es_unit *u)
{
    struct sess *s = opaque;
    struct card_unit cu;

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
    send_or_die(s, CM_OUT, &cu, sizeof(cu), u->data, u->len);
    free(u);
}

static void on_picture(void *opaque, const struct vframe *f)
{
    struct sess *s = opaque;
    struct card_thumb t;
    uint8_t *yuv = NULL;
    int w, h;

    if (!s->thumb_want)
        return;
    s->thumb_want = false;
    if (thumb_small(f, &yuv, &w, &h)) {
        t.width = w;
        t.height = h;
        send_or_die(s, CM_THUMB, &t, sizeof(t), yuv, (size_t)w * h * 3 / 2);
    } else {
        t.width = t.height = 0;
        send_or_die(s, CM_THUMB, &t, sizeof(t), NULL, 0);
    }
    free(yuv);
}

static void sess_unlink(struct sess *s)
{
    pthread_mutex_lock(&g_lock);
    for (struct sess **pp = &g_sessions; *pp; pp = &(*pp)->next)
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    g_ended++;
    pthread_mutex_unlock(&g_lock);
}

static void *session(void *arg)
{
    struct sess *s = arg;
    struct card_hdr h;
    struct card_ready rd;
    struct xcode_cfg cfg;
    struct timeval tv = { SEND_TIMEOUT_S, 0 };
    uint8_t *p = NULL;
    char name[16], prefix[96];

    setsockopt(s->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    memset(&rd, 0, sizeof(rd));
    if (!card_recv(s->fd, &h, &p) || h.type != CM_HELLO || h.len != sizeof(s->hi)) {
        free(p);
        goto out;
    }
    memcpy(&s->hi, p, sizeof(s->hi));
    free(p);
    p = NULL;
    s->hi.preset[sizeof(s->hi.preset) - 1] = 0;
    s->hi.profile[sizeof(s->hi.profile) - 1] = 0;
    s->hi.channel[sizeof(s->hi.channel) - 1] = 0;
    snprintf(prefix, sizeof(prefix), "%s: ", s->hi.channel[0] ? s->hi.channel : "?");
    log_thread_prefix(prefix);
    snprintf(name, sizeof(name), "cc3-card-%u", s->hi.out_pid);
    prctl(PR_SET_NAME, name);
    if (s->hi.proto != CARD_PROTO || s->hi.size != sizeof(s->hi)) {
        snprintf(rd.err, sizeof(rd.err), "the card process is another version; restart the channel");
        card_send(s->fd, CM_READY, &rd, sizeof(rd), NULL, 0);
        goto out;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.pid = s->hi.pid;
    cfg.out_pid = s->hi.out_pid;
    cfg.first_rendition = s->hi.first_rendition;
    cfg.in = (enum codec_id)s->hi.in;
    cfg.out = (enum codec_id)s->hi.out;
    cfg.bitrate = s->hi.bitrate;
    cfg.width = s->hi.width;
    cfg.height = s->hi.height;
    cfg.fit = s->hi.fit;
    cfg.decoder = (enum codec_where)s->hi.decoder;
    cfg.encoder = (enum codec_where)s->hi.encoder;
    cfg.gpu = g_card;
    cfg.vbv_ms = s->hi.vbv_ms;
    cfg.gop_ms = s->hi.gop_ms;
    cfg.bframes = s->hi.bframes;
    cfg.preset = s->hi.preset[0] ? s->hi.preset : NULL;
    cfg.profile = s->hi.profile[0] ? s->hi.profile : NULL;
    if (!codec_is_video(cfg.in) || !codec_is_video(cfg.out)) {
        snprintf(rd.err, sizeof(rd.err), "the card process does video only");
        card_send(s->fd, CM_READY, &rd, sizeof(rd), NULL, 0);
        goto out;
    }
    s->v = vxc_new(&cfg, on_out, s, &s->st, rd.err, sizeof(rd.err));
    if (!s->v) {
        card_send(s->fd, CM_READY, &rd, sizeof(rd), NULL, 0);
        goto out;
    }
    if (cfg.first_rendition)
        vxc_set_tap(s->v, on_picture, s);
    rd.ok = 1;
    snprintf(rd.decoder, sizeof(rd.decoder), "%s", s->st.decoder);
    snprintf(rd.encoder, sizeof(rd.encoder), "%s", s->st.encoder);
    if (!card_send(s->fd, CM_READY, &rd, sizeof(rd), NULL, 0))
        goto out;
    pthread_mutex_lock(&g_lock);
    s->next = g_sessions;
    g_sessions = s;
    g_started++;
    pthread_mutex_unlock(&g_lock);
    log_info("PID 0x%04X -> 0x%04X: session started (%s -> %s)", cfg.pid, cfg.out_pid,
             codec_name(cfg.in), codec_name(cfg.out));

    while (!s->dead && card_recv(s->fd, &h, &p)) {
        switch (h.type) {
        case CM_UNIT:
            if (h.len >= sizeof(struct card_unit)) {
                struct card_unit cu;
                struct es_unit *u;

                memcpy(&cu, p, sizeof(cu));
                if (cu.len != h.len - sizeof(cu) || !(u = malloc(sizeof(*u) + cu.len)))
                    break;
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
                vxc_unit(s->v, u);
                free(u);
            }
            break;
        case CM_RESET:
            vxc_reset(s->v);
            break;
        case CM_START_AFTER:
            if (h.len == sizeof(struct card_after)) {
                struct card_after a;

                memcpy(&a, p, sizeof(a));
                vxc_start_after(s->v, a.dts90, a.t1);
            }
            break;
        case CM_FINISH:
            vxc_finish(s->v);
            send_stats(s, true);
            send_or_die(s, CM_FINISHED, NULL, 0, NULL, 0);
            break;
        case CM_THUMB_REQ:
            s->thumb_want = true;
            break;
        default:
            break;
        }
        free(p);
        p = NULL;
        send_stats(s, false);
    }
    free(p);
    log_info("PID 0x%04X -> 0x%04X: session ended", s->hi.pid, s->hi.out_pid);
    vxc_free(s->v);
    s->v = NULL;
    sess_unlink(s);                         /* last: the process waits for this at its end */
out:
    vxc_free(s->v);
    close(s->fd);
    free(s);
    return NULL;
}

static void write_stats(const char *path)
{
    char tmp[600];
    FILE *f;
    int n = 0;

    snprintf(tmp, sizeof(tmp), "%.580s.tmp", path);
    f = fopen(tmp, "w");
    if (!f)
        return;
    pthread_mutex_lock(&g_lock);
    fprintf(f, "{\"card\": %d, \"pid\": %d, \"started\": %llu, \"ended\": %llu, \"sessions\": [",
            g_card, (int)getpid(), g_started, g_ended);
    for (struct sess *s = g_sessions; s; s = s->next)
        fprintf(f, "%s\n  {\"channel\": \"%s\", \"pid\": %u, \"out_pid\": %u, \"width\": %d, "
                   "\"height\": %d, \"decoder\": \"%s\", \"encoder\": \"%s\", \"frames_out\": %llu}",
                n++ ? "," : "", s->hi.channel, s->hi.pid, s->hi.out_pid, s->pub.width, s->pub.height,
                s->pub.decoder, s->pub.encoder, (unsigned long long)s->pub.frames_out);
    pthread_mutex_unlock(&g_lock);
    fprintf(f, "\n]}\n");
    fclose(f);
    rename(tmp, path);
}

int card_main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "card", required_argument, 0, 'c' },
        { "socket", required_argument, 0, 's' },
        { "stats-file", required_argument, 0, 'f' },
        { 0, 0, 0, 0 },
    };
    const char *path = NULL, *stats = NULL;
    struct sockaddr_un sa;
    struct sigaction act;
    int64_t last_stats = 0;
    int lfd, opt;

    optind = 2;
    while ((opt = getopt_long(argc, argv, "c:s:f:", opts, NULL)) != -1) {
        if (opt == 'c')
            g_card = atoi(optarg);
        else if (opt == 's')
            path = optarg;
        else if (opt == 'f')
            stats = optarg;
        else
            return 2;
    }
    if (!path || g_card < 0 || g_card > 63) {
        fprintf(stderr, "usage: caricoder3 card --card N --socket PATH [--stats-file FILE]\n");
        return 2;
    }
    if (strlen(path) >= sizeof(sa.sun_path)) {
        log_err("card %d: socket path too long: %s", g_card, path);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    memset(&act, 0, sizeof(act));
    act.sa_handler = on_signal;             /* no SA_RESTART: accept() returns */
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGINT, &act, NULL);

    lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    unlink(path);
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(lfd, 256) < 0) {
        log_err("card %d: cannot listen on %s: %s", g_card, path, strerror(errno));
        return 1;
    }
    chmod(path, 0600);
    log_info("card %d: ready for the channels' video (%s)", g_card, path);

    while (!atomic_load(&g_stop)) {
        struct timeval tv = { 1, 0 };
        fd_set rf;
        int64_t now;

        FD_ZERO(&rf);
        FD_SET(lfd, &rf);
        if (select(lfd + 1, &rf, NULL, NULL, &tv) > 0) {
            int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);

            if (fd >= 0) {
                struct sess *s = calloc(1, sizeof(*s));
                pthread_attr_t at;
                pthread_t th;

                if (!s) {
                    close(fd);
                    continue;
                }
                s->fd = fd;
                pthread_attr_init(&at);
                pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
                if (pthread_create(&th, &at, session, s) != 0) {
                    close(fd);
                    free(s);
                }
                pthread_attr_destroy(&at);
            }
        }
        now = clock_now_ns();
        if (stats && now - last_stats >= 2000000000LL) {
            write_stats(stats);
            last_stats = now;
        }
    }
    unlink(path);
    close(lfd);
    /* The sessions end (their channels connect to the next card process),
     * and what they hold on the card is given back before the process goes. */
    pthread_mutex_lock(&g_lock);
    for (struct sess *s = g_sessions; s; s = s->next)
        shutdown(s->fd, SHUT_RDWR);
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < 60; i++) {
        bool none;

        pthread_mutex_lock(&g_lock);
        none = !g_sessions;
        pthread_mutex_unlock(&g_lock);
        if (none)
            break;
        usleep(50000);
    }
    if (stats)
        unlink(stats);
    log_info("card %d: stopped", g_card);
    return 0;
}
