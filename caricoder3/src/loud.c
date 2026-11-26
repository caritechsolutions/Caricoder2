/*
 * caricoder3 - audio loudness: loud.h
 * Copyright (c) 2026 CariTech Solutions
 */
#include "loud.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CH_MAX      2
#define HOP_MS      100             /* a block is 4 hops (400 ms), 75 % overlap: BS.1770 */
#define WINDOW_S    60              /* what "the loudness" is taken over: long enough that a
                                     * programme's own quiet and loud scenes stay as they are */
#define BLOCKS      (WINDOW_S * 1000 / HOP_MS)
#define MIN_BLOCKS  97              /* 10 s of blocks over the gate before the figure counts
                                     * (a channel that starts in a quiet scene is not turned
                                     * right up for it) */
#define ABS_GATE    (-70.0)
#define REL_GATE    (-10.0)
#define UP_DB_S     0.5             /* the gain goes up this fast at most ... */
#define DOWN_DB_S   2.0             /* ... and down this fast: too loud is put right sooner */
#define DEAD_DB     1.0             /* a difference under this is left alone: the gain does
                                     * not follow every scene */
#define RELEASE_MS  200.0           /* the limiter lets go over this long */

struct biquad { double b0, b1, b2, a1, a2, z1[CH_MAX], z2[CH_MAX]; };

struct meter {                      /* BS.1770 over the last WINDOW_S */
    struct biquad shelf, hp;
    double hop_sum;                 /* mean-square sum of this hop, all channels */
    int hop_n, hop_len;
    double hops[4];                 /* the last four hops' energies */
    int n_hops;
    float blocks[BLOCKS];           /* block energies (mean square, channels summed) */
    int n_blocks, next;
    double lufs;                    /* NAN: not enough yet */
    bool fresh;                     /* the last block passed the gates */
};

struct loud {
    struct loud_cfg cfg;
    int rate, ch, delay;
    struct meter in, out;
    double gain_db, gain;           /* now */
    bool moving;                    /* the gain is on its way to a new value */
    /* limiter: the reduction each sample needs, its minimum over the look-ahead, smoothed */
    float *dly;                     /* delay line, (delay + 1) frames */
    float *need;                    /* ring of needed gains, for the minimum */
    int *dq;                        /* indices into need: a queue whose head is the minimum */
    int dq_head, dq_n;
    float *mins;                    /* the last delay + 1 minimums: averaged */
    double mins_sum;
    int pos;                        /* ring position, 0..delay */
    int64_t t;                      /* frames so far */
    double rel, release_step;       /* released reduction */
    double ceiling;
    uint64_t limited_frames;
};

static void biquad_shelf(struct biquad *q, double fs)
{
    double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
    double K = tan(M_PI * f0 / fs), Vh = pow(10.0, G / 20.0), Vb = pow(Vh, 0.4996667741545416);
    double a0 = 1.0 + K / Q + K * K;

    q->b0 = (Vh + Vb * K / Q + K * K) / a0;
    q->b1 = 2.0 * (K * K - Vh) / a0;
    q->b2 = (Vh - Vb * K / Q + K * K) / a0;
    q->a1 = 2.0 * (K * K - 1.0) / a0;
    q->a2 = (1.0 - K / Q + K * K) / a0;
}

static void biquad_hp(struct biquad *q, double fs)
{
    double f0 = 38.13547087602444, Q = 0.5003270373238773;
    double K = tan(M_PI * f0 / fs), a0 = 1.0 + K / Q + K * K;

    q->b0 = 1.0;
    q->b1 = -2.0;
    q->b2 = 1.0;
    q->a1 = 2.0 * (K * K - 1.0) / a0;
    q->a2 = (1.0 - K / Q + K * K) / a0;
}

static inline double biquad_run(struct biquad *q, int c, double x)
{
    double y = q->b0 * x + q->z1[c];

    q->z1[c] = q->b1 * x - q->a1 * y + q->z2[c];
    q->z2[c] = q->b2 * x - q->a2 * y;
    return y;
}

static void meter_init(struct meter *m, int rate)
{
    memset(m, 0, sizeof(*m));
    biquad_shelf(&m->shelf, rate);
    biquad_hp(&m->hp, rate);
    m->hop_len = rate * HOP_MS / 1000;
    m->lufs = NAN;
}

static double to_lufs(double ms)
{
    return ms > 0 ? -0.691 + 10.0 * log10(ms) : -200.0;
}

/* The gated loudness of the blocks held: BS.1770's two gates. */
static void meter_gate(struct meter *m)
{
    double sum = 0, rel, sum2 = 0;
    int n = 0, n2 = 0;
    double abs_ms = pow(10.0, (ABS_GATE + 0.691) / 10.0);

    for (int i = 0; i < m->n_blocks; i++)
        if (m->blocks[i] > abs_ms) {
            sum += m->blocks[i];
            n++;
        }
    if (n < MIN_BLOCKS) {
        if (n == 0)
            m->lufs = NAN;          /* the window is all silence now: nothing known */
        return;                     /* else: what was known stays */
    }
    rel = sum / n * pow(10.0, REL_GATE / 10.0);
    for (int i = 0; i < m->n_blocks; i++)
        if (m->blocks[i] > abs_ms && m->blocks[i] > rel) {
            sum2 += m->blocks[i];
            n2++;
        }
    if (n2)
        m->lufs = to_lufs(sum2 / n2);
}

/* One frame (all its channels, as -1..1) into the meter. */
static inline void meter_frame(struct meter *m, const double *x, int ch)
{
    for (int c = 0; c < ch; c++) {
        double y = biquad_run(&m->hp, c, biquad_run(&m->shelf, c, x[c]));

        m->hop_sum += y * y;
    }
    if (++m->hop_n < m->hop_len)
        return;
    memmove(m->hops, m->hops + 1, sizeof(m->hops[0]) * 3);
    m->hops[3] = m->hop_sum / m->hop_n;
    m->hop_sum = 0;
    m->hop_n = 0;
    if (m->n_hops < 4)
        m->n_hops++;
    if (m->n_hops == 4) {
        double e = (m->hops[0] + m->hops[1] + m->hops[2] + m->hops[3]) / 4;

        m->blocks[m->next] = (float)e;
        m->next = (m->next + 1) % BLOCKS;
        if (m->n_blocks < BLOCKS)
            m->n_blocks++;
        m->fresh = to_lufs(e) > ABS_GATE && (isnan(m->lufs) || to_lufs(e) > m->lufs + REL_GATE);
        meter_gate(m);
    }
}

bool loud_parse(const char *s, struct loud_cfg *c)
{
    char *end;
    double v;

    memset(c, 0, sizeof(*c));
    c->target = LOUD_TARGET_DEFAULT;
    if (!s || !*s || !strcmp(s, "off")) {
        c->mode = LOUD_OFF;
        return true;
    }
    if (!strcmp(s, "auto")) {
        c->mode = LOUD_AUTO;
        return true;
    }
    v = strtod(s, &end);
    if (end == s || *end || v < -40 || v > 40)
        return false;
    c->mode = LOUD_FIXED;
    c->gain_db = v;
    return true;
}

struct loud *loud_new(const struct loud_cfg *c, int rate, int ch)
{
    struct loud *l;

    if (!c || c->mode == LOUD_OFF || rate < 8000 || ch < 1 || ch > CH_MAX)
        return NULL;
    l = calloc(1, sizeof(*l));
    if (!l)
        return NULL;
    l->cfg = *c;
    l->rate = rate;
    l->ch = ch;
    l->delay = rate * LOUD_DELAY_MS / 1000;
    meter_init(&l->in, rate);
    meter_init(&l->out, rate);
    l->gain_db = c->mode == LOUD_FIXED ? c->gain_db : 0;
    l->gain = pow(10.0, l->gain_db / 20.0);
    l->ceiling = pow(10.0, LOUD_CEILING_DB / 20.0);
    l->rel = 1.0;
    l->release_step = 1.0 / (RELEASE_MS / 1000.0 * rate);
    l->dly = calloc((size_t)(l->delay + 1) * (size_t)ch, sizeof(float));
    l->need = malloc(sizeof(float) * (size_t)(l->delay + 1));
    l->dq = malloc(sizeof(int) * (size_t)(l->delay + 1));
    l->mins = malloc(sizeof(float) * (size_t)(l->delay + 1));
    if (!l->dly || !l->need || !l->dq || !l->mins) {
        loud_free(l);
        return NULL;
    }
    for (int i = 0; i <= l->delay; i++)
        l->need[i] = l->mins[i] = 1.0f;
    l->mins_sum = l->delay + 1;
    return l;
}

void loud_free(struct loud *l)
{
    if (!l)
        return;
    free(l->dly);
    free(l->need);
    free(l->dq);
    free(l->mins);
    free(l);
}

int loud_delay(const struct loud *l)
{
    return l ? l->delay : 0;
}

/* The gain, once a hop: toward target - measured, slowly; held while what
 * comes in is under the gate (a pause, a quiet scene) or not yet known. */
static void steer(struct loud *l)
{
    double want, up = UP_DB_S * HOP_MS / 1000.0, down = DOWN_DB_S * HOP_MS / 1000.0;

    if (l->cfg.mode != LOUD_AUTO || isnan(l->in.lufs) || !l->in.fresh)
        return;
    want = l->cfg.target - l->in.lufs;
    if (want > LOUD_RANGE_DB)
        want = LOUD_RANGE_DB;
    if (want < -LOUD_RANGE_DB)
        want = -LOUD_RANGE_DB;
    /* a move starts once the gain is DEAD_DB out, and then goes all the way */
    if (!l->moving && fabs(want - l->gain_db) < DEAD_DB)
        return;
    l->moving = true;
    if (want > l->gain_db + up)
        l->gain_db += up;
    else if (want < l->gain_db - down)
        l->gain_db -= down;
    else {
        l->gain_db = want;
        l->moving = false;
    }
    l->gain = pow(10.0, l->gain_db / 20.0);
}

void loud_process(struct loud *l, int16_t *pcm, int n)
{
    int ch, W;

    if (!l)
        return;
    ch = l->ch;
    W = l->delay + 1;
    for (int i = 0; i < n; i++, pcm += ch) {
        double x[CH_MAX], y[CH_MAX], peak = 0, g, m, lim;
        int p = l->pos;

        for (int c = 0; c < ch; c++)
            x[c] = pcm[c] / 32768.0;
        meter_frame(&l->in, x, ch);
        if (l->in.hop_n == 0)           /* a hop has just ended */
            steer(l);
        for (int c = 0; c < ch; c++) {
            x[c] *= l->gain;
            if (fabs(x[c]) > peak)
                peak = fabs(x[c]);
        }
        /* the reduction this frame needs, let go slowly after a peak */
        g = peak > l->ceiling ? l->ceiling / peak : 1.0;
        l->rel += l->release_step;
        if (l->rel > 1.0)
            l->rel = 1.0;
        if (g < l->rel)
            l->rel = g;
        g = l->rel;
        /* its minimum over the last W frames (a queue whose head is the least) */
        while (l->dq_n && l->need[l->dq[(l->dq_head + l->dq_n - 1) % W]] >= (float)g)
            l->dq_n--;
        if (l->dq_n && l->dq[l->dq_head] == p) {    /* the oldest leaves the window */
            l->dq_head = (l->dq_head + 1) % W;
            l->dq_n--;
        }
        l->need[p] = (float)g;
        l->dq[(l->dq_head + l->dq_n) % W] = p;
        l->dq_n++;
        m = l->need[l->dq[l->dq_head]];
        /* ... averaged over W frames: the gain glides down before the peak comes out */
        l->mins_sum += m - l->mins[p];
        l->mins[p] = (float)m;
        lim = l->mins_sum / W;
        if (lim > 1.0)
            lim = 1.0;
        if (lim < 0.999)
            l->limited_frames++;
        /* out: the frame that came in `delay` frames ago */
        {
            float *slot = l->dly + (size_t)((p + 1) % W) * (size_t)ch;   /* the oldest */

            for (int c = 0; c < ch; c++) {
                double v = slot[c] * lim;

                y[c] = v;
                v *= 32768.0;
                pcm[c] = (int16_t)(v > 32767.0 ? 32767 : v < -32768.0 ? -32768 : lrint(v));
            }
            slot = l->dly + (size_t)p * (size_t)ch;
            for (int c = 0; c < ch; c++)
                slot[c] = (float)x[c];
        }
        meter_frame(&l->out, y, ch);
        l->pos = (p + 1) % W;
        l->t++;
    }
    /* the queue's sums drift in floating point over days: put right now and then */
    if ((l->t & 0xFFFFF) < (int64_t)n) {
        double s = 0;

        for (int i = 0; i < W; i++)
            s += l->mins[i];
        l->mins_sum = s;
    }
}

void loud_get(const struct loud *l, struct loud_stats *st)
{
    memset(st, 0, sizeof(*st));
    st->in_lufs = st->out_lufs = NAN;
    if (!l)
        return;
    st->on = true;
    st->in_lufs = l->in.lufs;
    st->out_lufs = l->out.lufs;
    st->gain_db = l->gain_db;
    st->limited_ms = l->limited_frames * 1000 / (uint64_t)l->rate;
}
