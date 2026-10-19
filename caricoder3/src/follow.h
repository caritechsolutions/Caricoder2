/*
 * caricoder3 - following a source whose PIDs change
 * Copyright (c) 2026 CariTech Solutions
 *
 * Inside the engine every stream keeps the PID it had when it was first
 * seen (its "inside" PID). When the source moves a stream to another PID
 * (the provider rebuilt the multiplex), the stream is recognised by what
 * it is (kind, language, place among the streams of that kind) and keeps
 * its inside PID: the channel's settings, which name inside PIDs, still
 * apply to it, and the output keeps its PIDs. The PMT and a PCR-only PID
 * are kept the same way. What was learnt is kept in a small text file, so
 * that a channel started after the change still knows.
 */
#ifndef CC3_FOLLOW_H
#define CC3_FOLLOW_H

#include <stdbool.h>
#include <stdint.h>

#include "psi.h"

#define FOLLOW_MAX 48

struct follow_entry {
    uint16_t pid;           /* inside */
    uint16_t real;          /* in the source now (PID_NULL: not in its PMT) */
    uint8_t stream_type;
    uint8_t nth;            /* among the streams of this kind and language */
    char kind[12];
    char lang[4];
};

struct follow {
    char file[512];         /* "" = nothing kept */
    int n;
    struct follow_entry s[FOLLOW_MAX];
    uint16_t pmt_pid, pmt_real;
    uint16_t pcr_pid, pcr_real;     /* a PCR PID that carries no stream */
    bool loaded;
};

/* Reads what an earlier run learnt (a missing file is no error). */
void follow_init(struct follow *f, const char *file);

/*
 * The source's program `p` in, its inside version out (in place): stream,
 * PMT and PCR PIDs renamed. `alias` (8192) is filled: source PID -> inside
 * PID, identity elsewhere; `src` (8192) the reverse. `pinned`: inside PIDs
 * the channel's settings name; a source stream on one of those is taken
 * as that stream, never renamed. Changes are logged and saved.
 */
void follow_apply(struct follow *f, struct program *p, uint16_t *alias, uint16_t *src,
                  const uint16_t *pinned, int n_pinned);

#endif
