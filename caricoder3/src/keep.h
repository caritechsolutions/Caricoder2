/*
 * caricoder3 - the last minute and a half of the channel's input, kept
 * Copyright (c) 2026 CariTech Solutions
 *
 * An incident opens only once a problem has lasted (a silence 30 s, say),
 * and by then it may be over: a recording started at that moment shows
 * the stream after the fault, not the fault. So the channel keeps what it
 * received, its own program only (PAT, SDT, its PMT, the PMT's PIDs), for
 * the last KEEP_SECONDS, in memory. Asked to save, it waits out `after`
 * seconds and writes from `before` seconds back to `after` seconds on, to
 * a TS file: the start of the fault is in it.
 *
 * At most KEEP_MAX_BYTES per channel; a channel at a high rate keeps fewer
 * seconds (the save says how many it had).
 */
#ifndef CC3_KEEP_H
#define CC3_KEEP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define KEEP_SECONDS   90
#define KEEP_MAX_BYTES (128u * 1024 * 1024)

struct keep;

/* program: the service to keep (0: the first in the PAT). */
struct keep *keep_new(int program);
void keep_free(struct keep *k);

/* A datagram as received (TS, or TS in RTP), on the input thread. */
void keep_datagram(struct keep *k, const uint8_t *buf, size_t len, int64_t now_ns);

/* From another thread: saves to `path` in the background, once `after_s`
 * have passed; false (with a reason) if a save is already under way or
 * nothing is kept yet. */
bool keep_save(struct keep *k, const char *path, int before_s, int after_s, char *err, size_t errlen);

/* How much is kept now: seconds and bytes, and how much the oldest saves
 * took (for the stats). */
void keep_json(struct keep *k, FILE *f);

#endif
