/*
 * caricoder3 - following a source whose PIDs change
 * Copyright (c) 2026 CariTech Solutions
 */
#include "follow.h"

#include <stdio.h>
#include <string.h>

#include "log.h"

static void describe(const struct follow_entry *e, char *out, size_t len)
{
    snprintf(out, len, "%s%s%s", e->kind, e->lang[0] ? " " : "", e->lang);
}

static void save(const struct follow *f)
{
    char tmp[600];
    FILE *fp;

    if (!f->file[0])
        return;
    snprintf(tmp, sizeof(tmp), "%s.tmp", f->file);
    fp = fopen(tmp, "w");
    if (!fp) {
        log_warn("streams file %s: cannot write it", tmp);
        return;
    }
    fprintf(fp, "# caricoder3: this channel's streams by the PID they had when first seen\n"
                "# (inside PID, PID in the source now, type, kind, language, place)\n");
    if (f->pmt_pid)
        fprintf(fp, "pmt 0x%04X 0x%04X\n", f->pmt_pid, f->pmt_real);
    if (f->pcr_pid)
        fprintf(fp, "pcr 0x%04X 0x%04X\n", f->pcr_pid, f->pcr_real);
    for (int i = 0; i < f->n; i++) {
        const struct follow_entry *e = &f->s[i];

        fprintf(fp, "es 0x%04X 0x%04X 0x%02X %s %s %u\n", e->pid, e->real, e->stream_type, e->kind,
                e->lang[0] ? e->lang : "-", e->nth);
    }
    if (fclose(fp) != 0 || rename(tmp, f->file) != 0) {
        log_warn("streams file %s: cannot write it", f->file);
        remove(tmp);
    }
}

void follow_init(struct follow *f, const char *file)
{
    FILE *fp;
    char line[256];

    memset(f, 0, sizeof(*f));
    if (!file || !*file)
        return;
    snprintf(f->file, sizeof(f->file), "%s", file);
    fp = fopen(file, "r");
    if (!fp)
        return;
    while (fgets(line, sizeof(line), fp)) {
        unsigned a, b, t, nth;
        char kind[16], lang[8];

        if (sscanf(line, "pmt %x %x", &a, &b) == 2 && a < PID_NULL && b <= PID_NULL) {
            f->pmt_pid = (uint16_t)a;
            f->pmt_real = (uint16_t)b;
        } else if (sscanf(line, "pcr %x %x", &a, &b) == 2 && a < PID_NULL && b <= PID_NULL) {
            f->pcr_pid = (uint16_t)a;
            f->pcr_real = (uint16_t)b;
        } else if (sscanf(line, "es %x %x %x %15s %7s %u", &a, &b, &t, kind, lang, &nth) == 6 &&
                   a < PID_NULL && b <= PID_NULL && t < 256 && nth < 256 && f->n < FOLLOW_MAX) {
            struct follow_entry *e = &f->s[f->n++];

            e->pid = (uint16_t)a;
            e->real = (uint16_t)b;
            e->stream_type = (uint8_t)t;
            e->nth = (uint8_t)nth;
            snprintf(e->kind, sizeof(e->kind), "%.11s", kind);
            snprintf(e->lang, sizeof(e->lang), "%.3s", strcmp(lang, "-") ? lang : "");
        }
    }
    fclose(fp);
    f->loaded = true;
}

/* A free entry: a new one, or the place of the oldest stream no longer in
 * the source. -1 when all are taken by streams that are there. */
static int new_entry(struct follow *f, const bool *taken)
{
    if (f->n < FOLLOW_MAX)
        return f->n++;
    for (int i = 0; i < f->n; i++)
        if (!taken[i] && f->s[i].real == PID_NULL)
            return i;
    return -1;
}

void follow_apply(struct follow *f, struct program *p, uint16_t *alias, uint16_t *src,
                  const uint16_t *pinned, int n_pinned)
{
    int n = p->n_es;
    char kind[MAX_ES][12], lang[MAX_ES][8];
    uint8_t nth[MAX_ES];
    int who[MAX_ES];                /* stream -> entry, -1 none yet */
    bool taken[FOLLOW_MAX] = { 0 };
    bool changed = false;
    char what[32];

    for (int i = 0; i < n; i++) {
        snprintf(kind[i], sizeof(kind[i]), "%s", program_es_kind(p, i, lang[i], sizeof(lang[i])));
        lang[i][3] = 0;
        nth[i] = 0;
        for (int j = 0; j < i; j++)
            if (!strcmp(kind[j], kind[i]) && !strcmp(lang[j], lang[i]))
                nth[i]++;
        who[i] = -1;
    }
    /* 1. where it was: the usual case */
    for (int i = 0; i < n; i++)
        for (int k = 0; k < f->n && who[i] < 0; k++)
            if (!taken[k] && f->s[k].real == p->es[i].pid && !strcmp(f->s[k].kind, kind[i])) {
                who[i] = k;
                taken[k] = true;
            }
    /* 2. on its own inside PID again (the source moved back), or on a PID
     *    the settings name: that is the stream */
    for (int i = 0; i < n; i++)
        for (int k = 0; k < f->n && who[i] < 0; k++)
            if (!taken[k] && f->s[k].pid == p->es[i].pid && !strcmp(f->s[k].kind, kind[i])) {
                who[i] = k;
                taken[k] = true;
            }
    /* 3. moved: a stream not yet placed that is what a missing one was
     *    (kind, language and place; the same coding preferred), and whose
     *    inside PID no other stream of the source is on */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < n; i++) {
            bool pin = false;

            if (who[i] >= 0)
                continue;
            for (int j = 0; j < n_pinned; j++)
                pin |= pinned[j] == p->es[i].pid;
            if (pin)
                continue;
            for (int k = 0; k < f->n && who[i] < 0; k++) {
                const struct follow_entry *e = &f->s[k];
                bool clash = false;

                if (taken[k] || strcmp(e->kind, kind[i]) || strcmp(e->lang, lang[i]) || e->nth != nth[i] ||
                    (pass == 0 && e->stream_type != p->es[i].stream_type))
                    continue;
                for (int j = 0; j < n; j++)
                    clash |= j != i && p->es[j].pid == e->pid;
                if (clash)
                    continue;
                describe(e, what, sizeof(what));
                if (e->real != PID_NULL)
                    log_warn("source PIDs changed: %s moved from PID 0x%04X to 0x%04X; kept as 0x%04X "
                             "(settings and output unchanged)", what, e->real, p->es[i].pid, e->pid);
                else
                    log_warn("source PIDs changed: %s is back, now on PID 0x%04X; kept as 0x%04X "
                             "(settings and output unchanged)", what, p->es[i].pid, e->pid);
                who[i] = k;
                taken[k] = true;
            }
        }
    /* 4. new streams: their own PID inside, unless a stream already placed
     *    has that inside PID (then the next free one) */
    for (int i = 0; i < n; i++) {
        uint16_t pid = p->es[i].pid;
        int k;
        bool busy;

        if (who[i] >= 0)
            continue;
        do {
            busy = false;
            for (int j = 0; j < n; j++)
                busy |= who[j] >= 0 && f->s[who[j]].pid == pid;
            if (busy)
                pid = pid + 1 >= PID_NULL ? 0x20 : (uint16_t)(pid + 1);
        } while (busy);
        /* an entry for a stream that is gone, on this inside PID: forgotten */
        for (k = 0; k < f->n; k++)
            if (!taken[k] && f->s[k].pid == pid)
                break;
        if (k == f->n)
            k = new_entry(f, taken);
        if (k < 0) {
            who[i] = -2;            /* no room to remember it: as it is */
            continue;
        }
        if (k < f->n && f->s[k].pid == pid && f->s[k].kind[0] && strcmp(f->s[k].kind, kind[i]) && f->loaded) {
            describe(&f->s[k], what, sizeof(what));
            log_info("PID 0x%04X now carries %s; the %s it had is forgotten", pid, kind[i], what);
        }
        f->s[k].pid = pid;
        f->s[k].real = p->es[i].pid;
        f->s[k].stream_type = p->es[i].stream_type;
        f->s[k].nth = nth[i];
        snprintf(f->s[k].kind, sizeof(f->s[k].kind), "%.11s", kind[i]);
        snprintf(f->s[k].lang, sizeof(f->s[k].lang), "%.3s", lang[i]);
        if (pid != p->es[i].pid)
            log_warn("source PID 0x%04X (%s) is kept as 0x%04X: its own is in use", p->es[i].pid,
                     kind[i], pid);
        who[i] = k;
        taken[k] = true;
        changed = true;
    }
    /* what is gone */
    for (int k = 0; k < f->n; k++)
        if (!taken[k] && f->s[k].real != PID_NULL) {
            describe(&f->s[k], what, sizeof(what));
            log_warn("source PIDs changed: %s (PID 0x%04X, kept as 0x%04X) is no longer in the "
                     "source's PMT", what, f->s[k].real, f->s[k].pid);
            f->s[k].real = PID_NULL;
            changed = true;
        }

    for (int i = 0; i < 8192; i++)
        alias[i] = src[i] = (uint16_t)i;
    for (int i = 0; i < n; i++) {
        if (who[i] < 0)
            continue;
        struct follow_entry *e = &f->s[who[i]];

        if (e->real != p->es[i].pid || e->stream_type != p->es[i].stream_type || e->nth != nth[i] ||
            strcmp(e->lang, lang[i]))
            changed = true;
        e->real = p->es[i].pid;
        e->stream_type = p->es[i].stream_type;
        e->nth = nth[i];
        snprintf(e->lang, sizeof(e->lang), "%.3s", lang[i]);
        alias[p->es[i].pid] = e->pid;
        src[e->pid] = p->es[i].pid;
    }

    /* the PCR: on a stream, that stream's; on a PID of its own, kept */
    uint16_t pcr = p->pcr_pid;
    bool on_stream = false;

    for (int i = 0; i < n; i++)
        if (p->es[i].pid == pcr) {
            on_stream = true;
            pcr = alias[pcr];
        }
    if (!on_stream && pcr != PID_NULL) {
        if (!f->pcr_pid) {
            f->pcr_pid = f->pcr_real = pcr;
            changed = true;
        } else if (f->pcr_real != pcr) {
            log_warn("source PIDs changed: the PCR moved from PID 0x%04X to 0x%04X; kept as 0x%04X",
                     f->pcr_real, pcr, f->pcr_pid);
            f->pcr_real = pcr;
            changed = true;
        }
        bool clash = false;

        for (int i = 0; i < n; i++)
            clash |= alias[p->es[i].pid] == f->pcr_pid;
        if (!clash) {
            src[f->pcr_pid] = pcr;
            pcr = f->pcr_pid;
        }
    }
    /* the PMT */
    if (!f->pmt_pid) {
        f->pmt_pid = f->pmt_real = p->pmt_pid;
        changed = true;
    } else if (f->pmt_real != p->pmt_pid) {
        log_warn("source PIDs changed: the PMT moved from PID 0x%04X to 0x%04X; kept as 0x%04X",
                 f->pmt_real, p->pmt_pid, f->pmt_pid);
        f->pmt_real = p->pmt_pid;
        changed = true;
    }
    bool clash = f->pmt_pid == pcr;

    for (int i = 0; i < n; i++)
        clash |= alias[p->es[i].pid] == f->pmt_pid;
    for (int i = 0; i < p->n_other; i++)
        clash |= p->other_pmt_pid[i] == f->pmt_pid && f->pmt_pid != p->pmt_pid;
    uint16_t pmt = clash ? p->pmt_pid : f->pmt_pid;

    for (int i = 0; i < n; i++)
        p->es[i].pid = alias[p->es[i].pid];
    p->pcr_pid = pcr;
    p->pmt_pid = pmt;
    if (changed)
        save(f);
}
