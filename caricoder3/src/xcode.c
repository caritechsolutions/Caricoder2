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

#include "clock.h"
#include "log.h"
#include "psi.h"

#define QUEUE_BYTES_MAX (48u * 1024 * 1024) /* input waiting for the codec */
#define IDLE_NS         200000000LL         /* no input this long: flush what is half built */
/*
 * Falling behind. Output that reaches the mux after its send time goes
 * out late, and is dropped once it is later than the mux allows. Every
 * LATE_NS that the output has been late without a break, the thread
 * looks at how late it is now against the worst since it last looked:
 *
 * - getting less (by CATCHING_UP or more): the codec is working off a
 *   backlog, from its start or from a stall. It is left to it.
 * - not getting less, with input piled up (BEHIND of it waiting): the
 *   codec is slower than the stream and will never catch up by itself.
 *   What is waiting is thrown away, so that it works on current input.
 * - not getting less, with nothing piled up: the codec is fast enough,
 *   but --delay is too short for the pictures it holds back. That is
 *   reported (every LATE_WARN_NS) and nothing is thrown away.
 *
 * After input was thrown away a video decoder needs a key frame to start
 * from. Input is skipped until one is seen, but not for longer than
 * SKIP_MAX: a stream whose key frames are not recognised here is better
 * decoded with errors than not at all.
 */
#define LATE_NS         3000000000LL
#define CATCHING_UP     (STC_HZ * 3 / 10)
#define BEHIND          (STC_HZ / 2)
#define LATE_WARN_NS    10000000000LL
#define SKIP_MAX        (5 * STC_HZ)
#define FINISH_WAIT_NS   300000000L          /* longest a finish may hold up its caller */

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
    int64_t late_since;         /* ns; 0: the last unit out was on time */
    int64_t late_last;          /* ns; when the last late unit came out */
    int64_t late_worst;         /* most a unit was late since the last look */
    int64_t late_now;           /* how late the last unit was */
    stc_t skip_from;            /* worker only: send time of the first unit skipped */
    int64_t late_warned;        /* worker only */
    bool skip_to_key;           /* worker only: video input was thrown away */

    bool threaded;
    bool joined;                /* the thread has been told to finish, and has */
    pthread_t th;
    pthread_mutex_t q_lock;
    pthread_cond_t q_cond;
    pthread_cond_t done_cond;   /* the thread has left its loop */
    bool exited;
    struct es_unit *q_head, *q_tail;
    size_t q_bytes;
    unsigned q_units;
    bool stop;
    bool stop_when_empty;
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

/* Does this PES packet hold the start of a picture a decoder can begin
 * with? Looked for by its NAL unit types: an IDR picture or a sequence
 * parameter set in H.264; an IRAP picture or a parameter set in H.265. */
static bool starts_at_key(enum codec_id codec, const uint8_t *d, size_t len)
{
    for (size_t i = 0; i + 4 < len; i++) {
        unsigned type;

        if (d[i] != 0 || d[i + 1] != 0 || d[i + 2] != 1)
            continue;
        if (codec == CODEC_H264) {
            type = d[i + 3] & 0x1F;
            if (type == 5 || type == 7)
                return true;
        } else {
            type = (d[i + 3] >> 1) & 0x3F;
            if ((type >= 16 && type <= 21) || type == 32 || type == 33)
                return true;
        }
        i += 2;
    }
    return false;
}

static void process(struct xcode *x, struct es_unit *u)
{
    if (x->skip_to_key) {
        int off = pes_payload_offset(u->data, u->len);

        if (x->skip_from == NOPTS)
            x->skip_from = u->t0;
        if (u->t0 - x->skip_from > SKIP_MAX || u->t0 < x->skip_from) {
            log_warn("PID 0x%04X: no key frame recognised in %d s of video; decoding "
                     "from where it is", x->cfg.pid, (int)(SKIP_MAX / STC_HZ));
        } else if (off < 0 ||
                   !starts_at_key(x->cfg.in, u->data + off, u->len - (size_t)off)) {
            pthread_mutex_lock(&x->q_lock);
            x->overflow++;
            pthread_mutex_unlock(&x->q_lock);
            free(u);
            return;
        }
        x->skip_to_key = false;
    }
    if (x->video)
        vxc_unit(x->v, u);
    else
        axc_unit(x->a, u);
    free(u);
    publish(x);
}

/* Worker, after each unit: is the output late, and what is to be done? */
static void check_late(struct xcode *x)
{
    int64_t now, since, worst, late;
    struct es_unit *drop = NULL;
    unsigned n = 0;

    pthread_mutex_lock(&x->st_lock);
    since = x->late_since;
    worst = x->late_worst;
    late = x->late_now;
    pthread_mutex_unlock(&x->st_lock);
    if (!since)
        return;
    now = clock_now_ns();
    if (now - since < LATE_NS)
        return;
    if (worst - late >= CATCHING_UP) {
        /* Catching up. Look again in a while, from where it is now. */
        pthread_mutex_lock(&x->st_lock);
        x->late_since = now;
        x->late_worst = late;
        pthread_mutex_unlock(&x->st_lock);
        return;
    }

    pthread_mutex_lock(&x->q_lock);
    if (x->q_head && x->q_tail->t0 - x->q_head->t0 > BEHIND) {
        drop = x->q_head;
        n = x->q_units;
        x->q_head = x->q_tail = NULL;
        x->q_bytes = 0;
        x->q_units = 0;
        x->overflow += n;
    }
    pthread_mutex_unlock(&x->q_lock);

    if (drop) {
        while (drop) {
            struct es_unit *u = drop;

            drop = u->next;
            free(u);
        }
        if (x->video) {
            vxc_reset(x->v);
            x->skip_to_key = true;
            x->skip_from = NOPTS;
        }
        pthread_mutex_lock(&x->st_lock);
        x->late_since = 0;
        x->late_worst = 0;
        pthread_mutex_unlock(&x->st_lock);
        x->late_warned = now;
        log_warn("PID 0x%04X: the %s transcoder is slower than the stream (output %lld ms "
                 "late); %u packets of input skipped to catch up", x->cfg.pid,
                 x->video ? "video" : "audio", (long long)(worst / STC_PER_MS), n);
    } else {
        /* Late, steadily, with the codec keeping up. */
        pthread_mutex_lock(&x->st_lock);
        x->late_since = now;
        x->late_worst = late;
        pthread_mutex_unlock(&x->st_lock);
        if (now - x->late_warned > LATE_WARN_NS) {
            x->late_warned = now;
            log_warn("PID 0x%04X: transcoded %s reaches the output up to %lld ms after "
                     "its send time: --delay is too short for what this codec holds back",
                     x->cfg.pid, x->video ? "video" : "audio",
                     (long long)(worst / STC_PER_MS));
        }
    }
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

            if (x->stop_when_empty)
                break;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_nsec += IDLE_NS;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            rc = pthread_cond_timedwait(&x->q_cond, &x->q_lock, &ts);
            if (rc == ETIMEDOUT && !x->q_head && !x->stop && !x->stop_when_empty &&
                !x->video) {
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
        check_late(x);

        pthread_mutex_lock(&x->q_lock);
    }
    x->exited = true;
    pthread_cond_signal(&x->done_cond);
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
    if (x->threaded && !x->joined) {
        struct timespec ts;
        bool in_time;

        /* The thread works off what is queued, then goes. The caller is
         * the input thread of a live channel and cannot wait long: a
         * codec that is not through in FINISH_WAIT_NS is one that was
         * behind anyway, and what it holds is given up. */
        pthread_mutex_lock(&x->q_lock);
        x->stop_when_empty = true;
        pthread_cond_signal(&x->q_cond);
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_nsec += FINISH_WAIT_NS;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        while (!x->exited)
            if (pthread_cond_timedwait(&x->done_cond, &x->q_lock, &ts) == ETIMEDOUT)
                break;
        in_time = x->exited;
        if (!in_time) {
            x->stop = true;
            pthread_cond_signal(&x->q_cond);
        }
        pthread_mutex_unlock(&x->q_lock);
        pthread_join(x->th, NULL);
        x->joined = true;
        if (!in_time) {
            log_warn("PID 0x%04X: the %s transcoder was behind when its stream ended; "
                     "what it still held is lost", x->cfg.pid, x->video ? "video" : "audio");
            publish(x);
            return;
        }
    }
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
    if (slack >= 0) {
        x->late_since = 0;
        x->late_worst = 0;
        x->late_now = 0;
    } else {
        int64_t now = clock_now_ns();

        /* Lateness from before a pause in the output is not this one's. */
        if (!x->late_since || now - x->late_last > 1000000000LL)
            x->late_since = now;
        x->late_last = now;
        x->late_now = -slack;
        if (-slack > x->late_worst)
            x->late_worst = -slack;
    }
    pthread_mutex_unlock(&x->st_lock);
}

bool xcode_last_out(const struct xcode *x, int64_t *dts90, stc_t *t1)
{
    return x->video && vxc_last_out(x->v, dts90, t1);
}

void xcode_start_after(struct xcode *x, int64_t dts90, stc_t t1)
{
    if (x->video)
        vxc_start_after(x->v, dts90, t1);
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
        pthread_cond_init(&x->done_cond, &ca);
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
    if (x->threaded && !x->joined) {
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
    pthread_cond_destroy(&x->done_cond);
    pthread_mutex_destroy(&x->q_lock);
    pthread_mutex_destroy(&x->st_lock);
    free(x);
}
