/*
 * caricoder3 - the last minute and a half of the channel's input (keep.h)
 * Copyright (c) 2026 CariTech Solutions
 *
 * A ring of 188-byte packets with the time each arrived. It starts small
 * and doubles while it holds less than KEEP_SECONDS, up to KEEP_MAX_BYTES;
 * from then on the newest packet takes the oldest one's place.
 *
 * Which packets: the PAT, the SDT, the PMT of the program, and the PIDs
 * that PMT lists (and its PCR PID). Until the PAT and PMT have been seen,
 * all of them. PAT and PMT are read from single-packet sections, which is
 * what they are in practice; if one is ever longer, nothing is filtered.
 */
#include "keep.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cc3.h"
#include "clock.h"
#include "json.h"
#include "log.h"
#include "psi.h"

#define START_PACKETS 8192

struct keep {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    uint8_t *pk;
    int64_t *t;
    size_t cap, head, n;                /* head: where the next one goes */

    int program;
    uint16_t pmt_pid;                   /* 0: not known */
    bool have_pmt;
    uint8_t want[8192 / 8];
    struct program prog;                /* scratch for the PMT */

    /* the save under way */
    bool saving, stop;
    pthread_t th;
    bool joinable;
    char path[600];
    int64_t at_ns;
    int before_s, after_s;
    uint64_t saves;
};

static bool want(const struct keep *k, int pid)
{
    return (k->want[pid >> 3] >> (pid & 7)) & 1;
}

/* The section that starts in this packet, if it ends there too. */
static const uint8_t *section_in(const uint8_t *p, int *len)
{
    int off = 4;
    const uint8_t *sec;

    if (!(p[1] & 0x40) || !(p[3] & 0x10))
        return NULL;
    if (p[3] & 0x20)
        off += 1 + p[4];
    if (off >= TS_SIZE - 1)
        return NULL;
    off += 1 + p[off];                  /* pointer field */
    if (off + 3 > TS_SIZE)
        return NULL;
    sec = p + off;
    *len = 3 + (((sec[1] & 0x0F) << 8) | sec[2]);
    return off + *len <= TS_SIZE ? sec : NULL;
}

static void learn(struct keep *k, const uint8_t *p, int pid)
{
    const uint8_t *sec;
    int len;

    if ((pid != 0 && (!k->pmt_pid || pid != k->pmt_pid)) || !(sec = section_in(p, &len)))
        return;
    if (pid == 0 && psi_section_ok(sec, len, 0x00)) {
        uint16_t ts_id, number, pmt;
        uint8_t ver;

        if (psi_pat_find(sec, len, k->program, &ts_id, &ver, &number, &pmt) && pmt != k->pmt_pid) {
            k->pmt_pid = pmt;
            k->have_pmt = false;
        }
    } else if (pid == k->pmt_pid && psi_section_ok(sec, len, 0x02)) {
        memset(&k->prog, 0, sizeof(k->prog));
        if (psi_parse_pmt(sec, len, &k->prog) && (!k->program || k->prog.number == k->program)) {
            memset(k->want, 0, sizeof(k->want));
            for (int i = 0; i < k->prog.n_es; i++)
                k->want[k->prog.es[i].pid >> 3] |= (uint8_t)(1u << (k->prog.es[i].pid & 7));
            k->want[k->prog.pcr_pid >> 3] |= (uint8_t)(1u << (k->prog.pcr_pid & 7));
            k->have_pmt = true;
        }
    }
}

/* Twice the room, the packets in order from the oldest. */
static bool grow(struct keep *k)
{
    size_t ncap = k->cap ? k->cap * 2 : START_PACKETS;
    uint8_t *pk;
    int64_t *t;

    if (ncap * TS_SIZE > KEEP_MAX_BYTES)
        ncap = KEEP_MAX_BYTES / TS_SIZE;
    if (ncap <= k->cap)
        return false;
    pk = malloc(ncap * TS_SIZE);
    t = malloc(ncap * sizeof(*t));
    if (!pk || !t) {
        free(pk);
        free(t);
        return false;
    }
    for (size_t i = 0; i < k->n; i++) {
        size_t from = (k->head + k->cap - k->n + i) % k->cap;

        memcpy(pk + i * TS_SIZE, k->pk + from * TS_SIZE, TS_SIZE);
        t[i] = k->t[from];
    }
    free(k->pk);
    free(k->t);
    k->pk = pk;
    k->t = t;
    k->cap = ncap;
    k->head = k->n;
    return true;
}

static void put(struct keep *k, const uint8_t *p, int64_t now)
{
    int pid = ((p[1] & 0x1F) << 8) | p[2];

    learn(k, p, pid);
    if (k->have_pmt && pid != 0 && pid != 0x11 && pid != k->pmt_pid && !want(k, pid))
        return;
    if (k->n == k->cap) {
        int64_t oldest = k->cap ? k->t[k->head] : now;  /* full: the oldest is where head is */

        if ((!k->cap || now - oldest < (int64_t)KEEP_SECONDS * 1000000000LL) && grow(k)) {
            /* room made */
        } else if (!k->cap) {
            return;
        } else {
            k->n--;                         /* the oldest goes */
        }
    }
    memcpy(k->pk + k->head * TS_SIZE, p, TS_SIZE);
    k->t[k->head] = now;
    k->head = (k->head + 1) % k->cap;
    k->n++;
}

void keep_datagram(struct keep *k, const uint8_t *buf, size_t len, int64_t now_ns)
{
    const uint8_t *p = buf;
    size_t n = len;

    if (!k)
        return;
    /* TS in RTP: past the header (as the engine does) */
    if (n >= 12 + TS_SIZE && (p[0] & 0xC0) == 0x80) {
        size_t hdr = 12 + 4u * (p[0] & 0x0F);

        if ((p[0] & 0x10) && n >= hdr + 4)
            hdr += 4 + 4u * (((size_t)p[hdr + 2] << 8) | p[hdr + 3]);
        if (hdr < n && p[hdr] == TS_SYNC) {
            p += hdr;
            n -= hdr;
        }
    }
    pthread_mutex_lock(&k->lock);
    while (n >= TS_SIZE) {
        if (p[0] == TS_SYNC) {
            put(k, p, now_ns);
            p += TS_SIZE;
            n -= TS_SIZE;
        } else {
            p++;
            n--;
        }
    }
    pthread_mutex_unlock(&k->lock);
}

static void *saver(void *arg)
{
    struct keep *k = arg;
    struct timespec ts;
    int64_t from, to, first = 0, last = 0;
    uint8_t *out = NULL;
    size_t cnt = 0;
    char tmp[640], side[640];
    FILE *f;
    bool ok = false;

    /* until `after` has passed, or the channel ends (then what there is) */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += k->after_s;
    pthread_mutex_lock(&k->lock);
    while (!k->stop)
        if (pthread_cond_timedwait(&k->cond, &k->lock, &ts) == ETIMEDOUT)
            break;
    from = k->at_ns - (int64_t)k->before_s * 1000000000LL;
    to = k->at_ns + (int64_t)k->after_s * 1000000000LL;
    out = malloc(k->n * TS_SIZE + 1);
    for (size_t i = 0; out && i < k->n; i++) {
        size_t at = (k->head + k->cap - k->n + i) % k->cap;

        if (k->t[at] < from || k->t[at] > to)
            continue;
        if (!cnt)
            first = k->t[at];
        last = k->t[at];
        memcpy(out + cnt * TS_SIZE, k->pk + at * TS_SIZE, TS_SIZE);
        cnt++;
    }
    pthread_mutex_unlock(&k->lock);

    snprintf(tmp, sizeof(tmp), "%s.tmp", k->path);
    f = fopen(tmp, "wb");
    if (f && fwrite(out, TS_SIZE, cnt, f) == cnt && fclose(f) == 0) {
        ok = rename(tmp, k->path) == 0;
        f = NULL;
    }
    if (f)
        fclose(f);
    if (!ok)
        unlink(tmp);
    free(out);
    /* beside it, what it holds: from how long before the moment asked for
     * to how long after (fewer seconds than asked: the channel had not
     * kept that much, or had not been running that long) */
    snprintf(side, sizeof(side), "%s.json", k->path);
    f = fopen(side, "w");
    if (f) {
        fprintf(f, "{\"ok\": %s, \"packets\": %zu, \"bytes\": %zu, \"from_s\": %.1f, \"to_s\": %.1f}\n",
                ok ? "true" : "false", cnt, cnt * TS_SIZE,
                cnt ? (double)(first - k->at_ns) / 1e9 : 0.0, cnt ? (double)(last - k->at_ns) / 1e9 : 0.0);
        fclose(f);
    }
    if (ok)
        log_info("kept input saved: %s (%.1f s before to %.1f s after, %zu packets)", k->path,
                 cnt ? (double)(k->at_ns - first) / 1e9 : 0.0, cnt ? (double)(last - k->at_ns) / 1e9 : 0.0, cnt);
    else
        log_warn("kept input: could not write %s: %s", k->path, strerror(errno));
    pthread_mutex_lock(&k->lock);
    k->saving = false;
    k->saves++;
    pthread_mutex_unlock(&k->lock);
    return NULL;
}

bool keep_save(struct keep *k, const char *path, int before_s, int after_s, char *err, size_t errlen)
{
    if (!k) {
        snprintf(err, errlen, "this channel keeps no input");
        return false;
    }
    if (before_s < 0 || before_s > KEEP_SECONDS || after_s < 0 || before_s + after_s > KEEP_SECONDS) {
        snprintf(err, errlen, "before and after: 0 to %d s together", KEEP_SECONDS);
        return false;
    }
    pthread_mutex_lock(&k->lock);
    if (k->saving) {
        pthread_mutex_unlock(&k->lock);
        snprintf(err, errlen, "a save is under way already");
        return false;
    }
    if (!k->n) {
        pthread_mutex_unlock(&k->lock);
        snprintf(err, errlen, "nothing received yet");
        return false;
    }
    if (k->joinable) {
        pthread_join(k->th, NULL);      /* the last one, done */
        k->joinable = false;
    }
    snprintf(k->path, sizeof(k->path), "%s", path);
    k->at_ns = clock_now_ns();
    k->before_s = before_s;
    k->after_s = after_s;
    k->saving = true;
    if (pthread_create(&k->th, NULL, saver, k) != 0) {
        k->saving = false;
        pthread_mutex_unlock(&k->lock);
        snprintf(err, errlen, "cannot start a thread");
        return false;
    }
    k->joinable = true;
    pthread_mutex_unlock(&k->lock);
    return true;
}

void keep_json(struct keep *k, FILE *f)
{
    double secs = 0;
    size_t bytes;

    pthread_mutex_lock(&k->lock);
    if (k->n)
        secs = (double)(k->t[(k->head + k->cap - 1) % k->cap] - k->t[(k->head + k->cap - k->n) % k->cap]) / 1e9;
    bytes = k->cap * TS_SIZE;
    fprintf(f, "{\"seconds\": %.0f, \"memory_bytes\": %zu, \"saves\": %llu, \"saving\": %s}", secs, bytes,
            (unsigned long long)k->saves, k->saving ? "true" : "false");
    pthread_mutex_unlock(&k->lock);
}

struct keep *keep_new(int program)
{
    struct keep *k = calloc(1, sizeof(*k));

    if (!k)
        return NULL;
    k->program = program;
    pthread_mutex_init(&k->lock, NULL);
    {
        pthread_condattr_t ca;

        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        pthread_cond_init(&k->cond, &ca);
        pthread_condattr_destroy(&ca);
    }
    return k;
}

void keep_free(struct keep *k)
{
    if (!k)
        return;
    pthread_mutex_lock(&k->lock);
    k->stop = true;
    pthread_cond_broadcast(&k->cond);
    pthread_mutex_unlock(&k->lock);
    if (k->joinable)
        pthread_join(k->th, NULL);
    free(k->pk);
    free(k->t);
    pthread_cond_destroy(&k->cond);
    pthread_mutex_destroy(&k->lock);
    free(k);
}
