/*
 * caricoder3 - watching a process's memory for a leak
 * Copyright (c) 2026 CariTech Solutions
 *
 * A leak shows as memory that only ever goes up. A process's memory moves
 * all the time (buffers filling and emptying, a picture larger than the
 * last), so the test is on the low points: the window (6 hours by default)
 * is cut into six parts, and the least the process used in each part is
 * taken. A leak: those low points never go down by more than a little,
 * they rise in at least three of the five steps, and the last is above the
 * first by at least `min_mb` and `pct` per cent. A process that fills a
 * buffer once and stays there rises in one step, not three.
 *
 * Two measures: the process's own memory (resident) and its memory on the
 * cards (nvidia-smi), each judged on its own.
 */
#ifndef CC3_MEMWATCH_H
#define CC3_MEMWATCH_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#define MW_SAMPLES 1500                 /* a sample a minute: a day */

struct memtrack {
    pid_t pid;                          /* the series is that process's; another starts afresh */
    int n, head;
    int64_t t[MW_SAMPLES];              /* wall clock, s */
    float rss[MW_SAMPLES], card[MW_SAMPLES];    /* MB; card < 0: not known */
};

/* The resident memory of a process (MB), -1 if it is gone. */
double mw_rss_mb(pid_t pid);

void mw_add(struct memtrack *m, pid_t pid, int64_t now, double rss_mb, double card_mb);

/* Over the last window_s: is it growing like a leak? card: the card series.
 * from/to: the first and last low points. */
bool mw_growing(const struct memtrack *m, int64_t now, int window_s, int min_mb, int pct, bool card,
                double *from, double *to);

/* The latest sample (MB; -1 none). */
void mw_last(const struct memtrack *m, double *rss, double *card);

#endif
