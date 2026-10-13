/*
 * caricoder3 - transcoding one elementary stream: the part common to
 * audio and video (queue, thread, PES packets, command line)
 * Copyright (c) 2026 CariTech Solutions
 */
#include "xcode.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>

#include "log.h"
#include "psi.h"

#define QUEUE_BYTES_MAX (48u * 1024 * 1024) /* input waiting for the codec */
#define IDLE_NS         200000000LL         /* no input this long: flush what is half built */

struct xcode {
    struct xcode_cfg cfg;
    bool video;
    struct axc *a;
    struct vxc *v;

    /* worker's own counters, and the copy others may read */
    struct xcode_stats work;
    pthread_mutex_t st_lock;
    struct xcode_stats pub;
    uint64_t overflow;
    int64_t slack_min, slack_ever;

    bool threaded;
    pthread_t th;
    pthread_mutex_t q_lock;
    pthread_cond_t q_cond;
    struct es_unit *q_head, *q_tail;
    size_t q_bytes;
    unsigned q_units;
    bool stop;
    bool overflow_logged;
};

/* ---- PES packets ---------------------------------------------------- */

int pes_payload_offset(const uint8_t *pes, size_t len)
{
    size_t off;

    if (len < 9 || pes[0] != 0 || pes[1] != 0 || pes[2] != 1 || (pes[6] & 0xC0) != 0x80)
        return -1;
    off = 9u + pes[8];
    return off < len ? (int)off : -1;
}

static void put_ts(uint8_t *b, int prefix, int64_t ts)
{
    b[0] = (uint8_t)((prefix << 4) | 1);
    b[1] = 0;
    b[2] = 1;
    b[3] = 0;
    b[4] = 1;
    pes_patch_ts(b, ts);
}

struct es_unit *pes_unit_new(uint16_t pid, uint8_t stream_id, size_t len,
                             int64_t pts90, int64_t dts90, size_t *payload_off)
{
    bool both = dts90 != NOPTS && dts90 != pts90;
    size_t hdr = 9u + (both ? 10u : 5u);
    size_t plen = hdr - 6 + len;
    struct es_unit *u = calloc(1, sizeof(*u) + hdr + len);

    if (!u)
        return NULL;
    u->pid = pid;
    u->kind = ES_PES;
    u->len = hdr + len;
    u->pts = pts90;
    u->dts = both ? dts90 : pts90;
    u->pts_off = 9;
    u->dts_off = both ? 14 : -1;

    u->data[0] = 0;
    u->data[1] = 0;
    u->data[2] = 1;
    u->data[3] = stream_id;
    /* Video may leave the length open, and must when it exceeds 16 bits. */
    if (plen > 0xFFFF)
        plen = 0;
    u->data[4] = (uint8_t)(plen >> 8);
    u->data[5] = (uint8_t)plen;
    u->data[6] = 0x84;                      /* '10', data_alignment_indicator */
    u->data[7] = both ? 0xC0 : 0x80;
    u->data[8] = (uint8_t)(hdr - 9);
    put_ts(u->data + 9, both ? 3 : 2, pts90);
    if (both)
        put_ts(u->data + 14, 1, dts90);
    *payload_off = hdr;
    return u;
}

/* ---- command line --------------------------------------------------- */

static bool parse_rate(const char *s, int *out)
{
    char *end;
    double v = strtod(s, &end);

    if (end == s || v <= 0)
        return false;
    if (*end == 'k' || *end == 'K') {
        v *= 1e3;
        end++;
    } else if (*end == 'm' || *end == 'M') {
        v *= 1e6;
        end++;
    }
    if (*end || v > 2e9)
        return false;
    *out = (int)(v + 0.5);
    return true;
}

bool xcode_parse_spec(const char *s, bool video, struct xcode_spec *out,
                      char *err, size_t errlen)
{
    char buf[64];
    char *codec = buf, *rate, *eq;

    memset(out, 0, sizeof(*out));
    if (strlen(s) >= sizeof(buf)) {
        snprintf(err, errlen, "'%s' is too long", s);
        return false;
    }
    strcpy(buf, s);
    if ((eq = strchr(buf, '=')) != NULL) {
        char *end;
        long pid = strtol(buf, &end, 0);

        if (end != eq || pid < 16 || pid > 8190) {
            snprintf(err, errlen, "'%s': the part before '=' must be a PID (16 to 8190)", s);
            return false;
        }
        out->pid = (int)pid;
        codec = eq + 1;
    }
    if ((rate = strchr(codec, ':')) != NULL)
        *rate++ = 0;

    if (!strcmp(codec, "copy"))
        out->codec = CODEC_NONE;
    else if (video && (!strcmp(codec, "h264") || !strcmp(codec, "avc")))
        out->codec = CODEC_H264;
    else if (video && (!strcmp(codec, "hevc") || !strcmp(codec, "h265")))
        out->codec = CODEC_HEVC;
    else if (!video && !strcmp(codec, "aac"))
        out->codec = CODEC_AAC;
    else if (!video && !strcmp(codec, "mp2"))
        out->codec = CODEC_MP2;
    else if (!video && !strcmp(codec, "mp3"))
        out->codec = CODEC_MP3;
    else {
        snprintf(err, errlen, "'%s': unknown %s codec (use %s or copy)", s,
                 video ? "video" : "audio", video ? "h264, hevc" : "aac, mp2, mp3");
        return false;
    }
    if (rate) {
        int lo = video ? 100000 : 8000, hi = video ? 200000000 : 640000;

        if (out->codec == CODEC_NONE) {
            snprintf(err, errlen, "'%s': a copy has no bit rate", s);
            return false;
        }
        if (!parse_rate(rate, &out->bitrate) || out->bitrate < lo || out->bitrate > hi) {
            snprintf(err, errlen, "'%s': bit rate must be between %d and %d", s, lo, hi);
            return false;
        }
    }
    return true;
}

enum codec_id xcode_input_codec(const struct program *p, int idx)
{
    switch (p->es[idx].stream_type) {
    case 0x03:
    case 0x04:
        return CODEC_MPA;
    case 0x0F:
        return CODEC_AAC;
    case 0x11:
        return CODEC_AAC_LATM;
    case 0x1B:
        return CODEC_H264;
    case 0x24:
        return CODEC_HEVC;
    case 0x81:                              /* AC-3 the ATSC way */
        return CODEC_AC3;
    case 0x06:                              /* AC-3 the DVB way: private data
                                             * plus a descriptor that says so */
        if (program_es_has_desc(p, idx, 0x7A, NULL))
            return CODEC_NONE;              /* E-AC-3 */
        if (program_es_has_desc(p, idx, 0x6A, NULL) ||
            program_es_has_desc(p, idx, 0x05, "AC-3"))
            return CODEC_AC3;
        return CODEC_NONE;
    default:
        return CODEC_NONE;
    }
}

/* ---- running -------------------------------------------------------- */

static void publish(struct xcode *x)
{
    pthread_mutex_lock(&x->st_lock);
    x->pub = x->work;
    pthread_mutex_unlock(&x->st_lock);
}

static void process(struct xcode *x, struct es_unit *u)
{
    if (x->video)
        vxc_unit(x->v, u);
    else
        axc_unit(x->a, u);
    free(u);
    publish(x);
}

static void *worker(void *arg)
{
    struct xcode *x = arg;
    char name[16];

    snprintf(name, sizeof(name), "cc3-%s-%u", x->video ? "v" : "a", x->cfg.pid);
    prctl(PR_SET_NAME, name);

    pthread_mutex_lock(&x->q_lock);
    while (!x->stop) {
        struct es_unit *u = x->q_head;

        if (!u) {
            struct timespec ts;
            int rc;

            clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_nsec += IDLE_NS;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            rc = pthread_cond_timedwait(&x->q_cond, &x->q_lock, &ts);
            if (rc == ETIMEDOUT && !x->q_head && !x->stop && !x->video) {
                /* The input has gone quiet. A packet that is part full
                 * goes out now rather than when the input returns. */
                pthread_mutex_unlock(&x->q_lock);
                axc_idle(x->a);
                publish(x);
                pthread_mutex_lock(&x->q_lock);
            }
            continue;
        }
        x->q_head = u->next;
        if (!x->q_head)
            x->q_tail = NULL;
        x->q_bytes -= u->len;
        x->q_units--;
        pthread_mutex_unlock(&x->q_lock);

        u->next = NULL;
        process(x, u);

        pthread_mutex_lock(&x->q_lock);
    }
    pthread_mutex_unlock(&x->q_lock);
    return NULL;
}

void xcode_push(struct xcode *x, struct es_unit *u)
{
    bool drop = false, say = false;

    u->next = NULL;
    if (!x->threaded) {
        process(x, u);
        return;
    }
    pthread_mutex_lock(&x->q_lock);
    if (x->q_bytes + u->len > QUEUE_BYTES_MAX) {
        /* The codec cannot keep up. Old input is worth less than new,
         * but throwing away what is queued would cost a decoder its
         * reference pictures all the same; so the newcomer goes, whole. */
        drop = true;
        x->overflow++;
        say = !x->overflow_logged;
        x->overflow_logged = true;
    } else {
        if (x->q_tail)
            x->q_tail->next = u;
        else
            x->q_head = u;
        x->q_tail = u;
        x->q_bytes += u->len;
        x->q_units++;
        x->overflow_logged = false;
        pthread_cond_signal(&x->q_cond);
    }
    pthread_mutex_unlock(&x->q_lock);
    if (drop)
        free(u);
    if (say)
        log_warn("PID 0x%04X: the %s cannot keep up, input is being dropped",
                 x->cfg.pid, x->video ? "video transcoder" : "audio transcoder");
}

void xcode_finish(struct xcode *x)
{
    if (x->threaded)
        return;                             /* a live run has no end */
    if (x->video)
        vxc_finish(x->v);
    else
        axc_finish(x->a);
    publish(x);
}

void xcode_note_slack(struct xcode *x, int64_t slack)
{
    pthread_mutex_lock(&x->st_lock);
    if (slack < x->slack_min)
        x->slack_min = slack;
    if (slack < x->slack_ever)
        x->slack_ever = slack;
    pthread_mutex_unlock(&x->st_lock);
}

void xcode_get_stats(struct xcode *x, struct xcode_stats *st)
{
    pthread_mutex_lock(&x->st_lock);
    *st = x->pub;
    st->slack_min = x->slack_min;
    st->slack_ever = x->slack_ever;
    x->slack_min = INT64_MAX;
    pthread_mutex_unlock(&x->st_lock);

    st->pid = x->cfg.pid;
    st->in = x->cfg.in;
    st->out = x->cfg.out;
    if (x->threaded) {
        pthread_mutex_lock(&x->q_lock);
        st->queued = x->q_units;
        st->overflow = x->overflow;
        pthread_mutex_unlock(&x->q_lock);
    }
}

struct xcode *xcode_new(const struct xcode_cfg *cfg, bool threaded,
                        xcode_emit_fn emit, void *opaque,
                        char *err, size_t errlen)
{
    struct xcode *x = calloc(1, sizeof(*x));

    if (!x)
        return NULL;
    x->cfg = *cfg;
    x->video = codec_is_video(cfg->in);
    x->slack_min = x->slack_ever = INT64_MAX;
    pthread_mutex_init(&x->st_lock, NULL);
    pthread_mutex_init(&x->q_lock, NULL);
    {
        pthread_condattr_t ca;

        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        pthread_cond_init(&x->q_cond, &ca);
        pthread_condattr_destroy(&ca);
    }
    if (x->video)
        x->v = vxc_new(cfg, emit, opaque, &x->work, err, errlen);
    else
        x->a = axc_new(cfg, emit, opaque, &x->work, err, errlen);
    if (!x->a && !x->v) {
        xcode_free(x);
        return NULL;
    }
    publish(x);
    if (threaded) {
        if (pthread_create(&x->th, NULL, worker, x) != 0) {
            snprintf(err, errlen, "cannot start a thread");
            xcode_free(x);
            return NULL;
        }
        x->threaded = true;
    }
    return x;
}

void xcode_free(struct xcode *x)
{
    if (!x)
        return;
    if (x->threaded) {
        pthread_mutex_lock(&x->q_lock);
        x->stop = true;
        pthread_cond_signal(&x->q_cond);
        pthread_mutex_unlock(&x->q_lock);
        pthread_join(x->th, NULL);
    }
    while (x->q_head) {
        struct es_unit *u = x->q_head;

        x->q_head = u->next;
        free(u);
    }
    axc_free(x->a);
    vxc_free(x->v);
    pthread_cond_destroy(&x->q_cond);
    pthread_mutex_destroy(&x->q_lock);
    pthread_mutex_destroy(&x->st_lock);
    free(x);
}
