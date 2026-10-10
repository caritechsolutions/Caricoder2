/*
 * caricoder3 - clock recovery
 * Copyright (c) 2026 CariTech Solutions
 */
#ifndef CC3_CLOCK_H
#define CC3_CLOCK_H

#include "cc3.h"

/*
 * Software PLL that maps this machine's monotonic clock onto the 27 MHz
 * clock of the source, learned from the arrival times of its PCRs. The
 * output is paced from this, so the transcoder neither over- nor
 * under-runs however far the two crystals drift apart.
 */
struct pll {
    bool    valid;
    int64_t t_ref;          /* ns, CLOCK_MONOTONIC */
    int64_t s_ref;          /* 27 MHz ticks at t_ref ... */
    double  s_frac;         /* ... plus this fraction of a tick */
    double  rate;           /* ticks per ns, nominally 0.027 */
    int64_t t_last;
    int     samples;
    double  jitter;         /* smoothed |error| of the PCR arrivals, ticks */
};

int64_t clock_now_ns(void);

void    pll_init(struct pll *p, int64_t t_ns, stc_t stc);
void    pll_update(struct pll *p, int64_t t_ns, stc_t stc);
stc_t   pll_eval(const struct pll *p, int64_t t_ns);
int64_t pll_inverse(const struct pll *p, stc_t stc);
double  pll_ppm(const struct pll *p);

#endif
