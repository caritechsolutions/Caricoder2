/*
 * caricoder3 - clock recovery
 * Copyright (c) 2026 CariTech Solutions
 *
 * Second-order (proportional + integral) loop. Each PCR arrival gives an
 * error between where the model says the source clock is and what the
 * PCR says. A slice of the error corrects the phase, a smaller slice the
 * frequency. Network jitter is averaged away because the loop is slow
 * (seconds); crystal drift is followed because it is slower still.
 *
 * The loop starts wide to pull in quickly and narrows once it has seen
 * enough samples.
 */
#include "clock.h"

#include <math.h>
#include <time.h>

#define NOMINAL_RATE    0.027           /* 27 MHz in ticks per nanosecond */
#define MAX_PPM         500.0
#define ACQUIRE_SAMPLES 150
#define FREQ_HOLDOFF    20              /* phase only, until it settles */
#define W_ACQUIRE       0.6             /* loop natural frequency, rad/s */
#define W_TRACK         0.06
#define ZETA            1.0             /* critically damped */

int64_t clock_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void pll_init(struct pll *p, int64_t t_ns, stc_t stc)
{
    p->valid = true;
    p->t_ref = t_ns;
    p->s_ref = stc;
    p->s_frac = 0.0;
    p->rate = NOMINAL_RATE;
    p->t_last = t_ns;
    p->samples = 0;
    p->jitter = 0.0;
}

stc_t pll_eval(const struct pll *p, int64_t t_ns)
{
    return p->s_ref + (int64_t)floor((double)(t_ns - p->t_ref) * p->rate + p->s_frac);
}

int64_t pll_inverse(const struct pll *p, stc_t stc)
{
    /* Rounded up: at the time returned, pll_eval() has reached `stc`.
     * Rounded down, a caller that sleeps until then would wake a
     * nanosecond early, find nothing due and have to come round again. */
    return p->t_ref + (int64_t)ceil(((double)(stc - p->s_ref) - p->s_frac) / p->rate);
}

void pll_update(struct pll *p, int64_t t_ns, stc_t stc)
{
    double pred = (double)(t_ns - p->t_ref) * p->rate + p->s_frac;
    double err = (double)(stc - p->s_ref) - pred;
    double dt = (double)(t_ns - p->t_last) / 1e9;
    double w = p->samples < ACQUIRE_SAMPLES ? W_ACQUIRE : W_TRACK;
    double kp, off, whole;

    if (dt < 0.0)
        dt = 0.0;
    if (dt > 0.5)                       /* after a gap, do not over-correct */
        dt = 0.5;

    kp = 2.0 * ZETA * w * dt;
    if (kp > 1.0)
        kp = 1.0;
    off = pred + kp * err;

    if (p->samples >= FREQ_HOLDOFF) {
        p->rate += w * w * dt * err / 1e9;
        if (p->rate > NOMINAL_RATE * (1.0 + MAX_PPM / 1e6))
            p->rate = NOMINAL_RATE * (1.0 + MAX_PPM / 1e6);
        if (p->rate < NOMINAL_RATE * (1.0 - MAX_PPM / 1e6))
            p->rate = NOMINAL_RATE * (1.0 - MAX_PPM / 1e6);
    }

    /* Move the reference point up to now so the numbers stay small. */
    whole = floor(off);
    p->s_ref += (int64_t)whole;
    p->s_frac = off - whole;
    p->t_ref = t_ns;
    p->t_last = t_ns;
    p->samples++;
    p->jitter += (fabs(err) - p->jitter) * 0.02;
}

double pll_ppm(const struct pll *p)
{
    return (p->rate / NOMINAL_RATE - 1.0) * 1e6;
}
