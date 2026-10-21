/*
 * caricoder3 - watching a process's memory for a leak (memwatch.h)
 * Copyright (c) 2026 CariTech Solutions
 */
#include "memwatch.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

double mw_rss_mb(pid_t pid)
{
    char path[64];
    FILE *f;
    long pages = -1, size;

    snprintf(path, sizeof(path), "/proc/%d/statm", (int)pid);
    f = fopen(path, "r");
    if (!f)
        return -1;
    if (fscanf(f, "%ld %ld", &size, &pages) != 2)
        pages = -1;
    fclose(f);
    return pages < 0 ? -1 : (double)pages * (double)sysconf(_SC_PAGESIZE) / (1024.0 * 1024.0);
}

void mw_add(struct memtrack *m, pid_t pid, int64_t now, double rss_mb, double card_mb)
{
    if (m->pid != pid) {
        m->pid = pid;
        m->n = m->head = 0;
    }
    m->t[m->head] = now;
    m->rss[m->head] = (float)rss_mb;
    m->card[m->head] = (float)card_mb;
    m->head = (m->head + 1) % MW_SAMPLES;
    if (m->n < MW_SAMPLES)
        m->n++;
}

void mw_last(const struct memtrack *m, double *rss, double *card)
{
    int at = (m->head + MW_SAMPLES - 1) % MW_SAMPLES;

    *rss = m->n ? m->rss[at] : -1;
    *card = m->n ? m->card[at] : -1;
}

bool mw_growing(const struct memtrack *m, int64_t now, int window_s, int min_mb, int pct, bool card,
                double *from, double *to)
{
    double low[6];
    bool have[6] = { false };
    int64_t start = now - window_s, oldest = 0;
    int rises = 0;

    if (window_s < 12 || m->n < 12)
        return false;
    for (int i = 0; i < m->n; i++) {
        int at = (m->head + MW_SAMPLES - m->n + i) % MW_SAMPLES;
        double v = card ? m->card[at] : m->rss[at];
        int part;

        if (i == 0)
            oldest = m->t[at];
        if (m->t[at] < start || v < 0)
            continue;
        part = (int)((m->t[at] - start) * 6 / window_s);
        if (part > 5)
            part = 5;
        if (!have[part] || v < low[part])
            low[part] = v;
        have[part] = true;
    }
    /* the whole window must be covered: a process younger than it is not judged yet */
    if (oldest > start + window_s / 12)
        return false;
    for (int k = 0; k < 6; k++)
        if (!have[k])
            return false;
    for (int k = 1; k < 6; k++) {
        if (low[k] < low[k - 1] * 0.98 - 1.0)
            return false;               /* it came down: not a leak */
        rises += low[k] > low[k - 1] + 0.5;
    }
    *from = low[0];
    *to = low[5];
    return rises >= 3 && low[5] - low[0] >= min_mb && low[5] - low[0] >= low[0] * pct / 100.0;
}
