/*
 * caricoder3 - watching the content: black or frozen picture, silent audio
 * Copyright (c) 2026 CariTech Solutions
 */
#include "watch.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "aframe.h"
#include "log.h"
#include "xcode.h"

#define BLACK_Y         32          /* luma at or below: black (video black is 16) */
#define BLACK_SHARE     0.98        /* of the picture */
#define FROZEN_CHANGE   0.5         /* mean luma difference below: the same picture */
#define WIN_EVERY_NS    3000000000LL
#define WIN_LEN_NS      1000000000LL
#define SILENT_PEAK_DB  (-60.0)
#define FORGET_NS       60000000000LL

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int64_t mono_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ---- the picture ---------------------------------------------------- */

static struct {
    uint8_t *prev;
    int w, h;
    bool have;
    int64_t t;                  /* when the last one was looked at */
    double luma, change;
    int64_t black_since, frozen_since;      /* 0: not */
} pic;

void watch_picture(const uint8_t *y, int w, int h)
{
    size_t n = (size_t)w * h, dark = 0;
    uint64_t sum = 0, diff = 0;
    int64_t now = mono_ns();
    bool same_size;

    if (!y || w <= 0 || h <= 0)
        return;
    pthread_mutex_lock(&g_lock);
    same_size = pic.prev && pic.w == w && pic.h == h;
    for (size_t i = 0; i < n; i++) {
        sum += y[i];
        dark += y[i] <= BLACK_Y;
        if (same_size)
            diff += (uint64_t)abs((int)y[i] - (int)pic.prev[i]);
    }
    pic.luma = (double)sum / (double)n;
    pic.change = same_size ? (double)diff / (double)n : -1;
    if ((double)dark >= BLACK_SHARE * (double)n) {
        if (!pic.black_since)
            pic.black_since = now;
    } else {
        pic.black_since = 0;
    }
    /* still: the same picture as the one before, and not black (that is
     * reported as black) */
    if (same_size && pic.change < FROZEN_CHANGE && !pic.black_since) {
        if (!pic.frozen_since)
            pic.frozen_since = pic.t;
    } else {
        pic.frozen_since = 0;
    }
    if (!same_size) {
        free(pic.prev);
        pic.prev = malloc(n);
        pic.w = w;
        pic.h = h;
    }
    if (pic.prev)
        memcpy(pic.prev, y, n);
    pic.have = true;
    pic.t = now;
    pthread_mutex_unlock(&g_lock);
}

/* ---- audio ------------------------------------------------------------ */

struct astate {
    bool used;
    uint16_t pid;
    enum codec_id codec;
    struct aframer *fr;
    const struct codec_backend *be;
    void *dec;
    bool failed;
    int64_t last_unit;
    int64_t win_start;          /* 0: no window open */
    int64_t next_win;
    double sumsq, peak;
    uint64_t samples;
    /* results */
    bool measured;
    double level_db, peak_db;
    int64_t silent_since, last_win_end;
    int channels, rate;
};

static struct astate aud[WATCH_AUDIO_MAX];

static void astate_free(struct astate *a)
{
    if (a->dec)
        a->be->adec->close(a->dec);
    aframer_free(a->fr);
    memset(a, 0, sizeof(*a));
}

static struct astate *astate_for(uint16_t pid, enum codec_id codec, int64_t now)
{
    struct astate *free_one = NULL;

    for (int i = 0; i < WATCH_AUDIO_MAX; i++) {
        struct astate *a = &aud[i];

        if (a->used && now - a->last_unit > FORGET_NS)
            astate_free(a);
        if (a->used && a->pid == pid) {
            if (a->codec == codec)
                return a;
            astate_free(a);             /* the stream changed coding */
        }
        if (!a->used && !free_one)
            free_one = a;
    }
    if (!free_one)
        return NULL;
    free_one->used = true;
    free_one->pid = pid;
    free_one->codec = codec;
    free_one->next_win = now;
    free_one->fr = aframer_new(codec);
    if (!free_one->fr)
        free_one->failed = true;
    return free_one;
}

static double db(double v)
{
    return v > 0 ? 20.0 * log10(v) : -120.0;
}

static void window_end(struct astate *a, int64_t now)
{
    if (a->samples) {
        a->level_db = db(sqrt(a->sumsq / (double)a->samples) / 32768.0);
        a->peak_db = db(a->peak / 32768.0);
        a->measured = true;
        if (a->peak_db < SILENT_PEAK_DB) {
            if (!a->silent_since)
                a->silent_since = a->win_start;
        } else {
            a->silent_since = 0;
        }
        a->last_win_end = now;
    }
    a->win_start = 0;
    a->next_win = now + WIN_EVERY_NS - WIN_LEN_NS;
}

void watch_audio_unit(uint16_t pid, enum codec_id codec, const uint8_t *pes, size_t len)
{
    static int16_t pcm[AUDIO_MAX_FRAME * AUDIO_MAX_CH];
    int64_t now = mono_ns();
    struct astate *a;
    int off;

    if (codec == CODEC_NONE)
        return;
    pthread_mutex_lock(&g_lock);
    a = astate_for(pid, codec, now);
    if (!a || a->failed) {
        if (a)
            a->last_unit = now;
        pthread_mutex_unlock(&g_lock);
        return;
    }
    a->last_unit = now;
    if (!a->win_start) {
        if (now < a->next_win) {
            pthread_mutex_unlock(&g_lock);
            return;
        }
        /* a new window: decoding starts afresh */
        if (!a->dec) {
            char err[160];

            a->be = codec_find(CODEC_AUDIO, CODEC_DECODER, codec, WHERE_CPU, 0);
            a->dec = a->be ? a->be->adec->open(codec, err, sizeof(err)) : NULL;
            if (!a->dec) {
                a->failed = true;
                pthread_mutex_unlock(&g_lock);
                return;
            }
        } else {
            a->be->adec->reset(a->dec);
        }
        aframer_reset(a->fr);
        a->win_start = now;
        a->sumsq = a->peak = 0;
        a->samples = 0;
    }
    off = pes_payload_offset(pes, len);
    if (off >= 0) {
        const uint8_t *au;
        size_t aulen;
        stc_t pts;

        aframer_push(a->fr, pes + off, len - (size_t)off, NOPTS);
        while (aframer_next(a->fr, &au, &aulen, &pts)) {
            int rate = 0, ch = 0, delay = 0;
            int n = a->be->adec->decode(a->dec, au, aulen, pcm, &rate, &ch, &delay);

            if (n <= 0 || ch <= 0)
                continue;
            a->rate = rate;
            a->channels = ch;
            for (int i = 0; i < n * ch; i++) {
                double v = pcm[i];

                a->sumsq += v * v;
                if (fabs(v) > a->peak)
                    a->peak = fabs(v);
            }
            a->samples += (uint64_t)n * (uint64_t)ch;
        }
    }
    if (now - a->win_start >= WIN_LEN_NS)
        window_end(a, now);
    pthread_mutex_unlock(&g_lock);
}

void watch_get(struct watch_stats *out)
{
    int64_t now = mono_ns();

    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&g_lock);
    out->have_picture = pic.have;
    if (pic.have) {
        out->picture_age_ms = (now - pic.t) / 1000000;
        out->luma = pic.luma;
        out->change = pic.change;
        out->black_ms = pic.black_since ? (pic.t - pic.black_since) / 1000000 : 0;
        out->frozen_ms = pic.frozen_since ? (pic.t - pic.frozen_since) / 1000000 : 0;
    }
    for (int i = 0; i < WATCH_AUDIO_MAX; i++) {
        struct astate *a = &aud[i];

        if (!a->used || now - a->last_unit > FORGET_NS)
            continue;
        struct watch_audio *w = &out->audio[out->n_audio++];

        w->pid = a->pid;
        w->measured = a->measured;
        w->level_db = a->level_db;
        w->peak_db = a->peak_db;
        w->silent_ms = a->silent_since ? (a->last_win_end - a->silent_since) / 1000000 : 0;
        w->channels = a->channels;
        w->rate = a->rate;
    }
    pthread_mutex_unlock(&g_lock);
}
