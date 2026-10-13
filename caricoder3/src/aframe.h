/*
 * caricoder3 - audio framing: an elementary stream cut into access units
 * Copyright (c) 2026 CariTech Solutions
 *
 * A PES packet carries audio as a run of bytes that need not start or end
 * on a frame boundary, with one time stamp that belongs to the first
 * frame *starting* in it. Decoders want whole frames, each with its own
 * time. This turns the one into the other, and finds its way back after
 * damaged data. It is also used on what the encoders produce, to count
 * the frames they emit.
 */
#ifndef CC3_AFRAME_H
#define CC3_AFRAME_H

#include "cc3.h"
#include "codec.h"

struct aframer;

/* codec: CODEC_MPA, CODEC_MP2, CODEC_MP3, CODEC_AAC, CODEC_AAC_LATM, CODEC_AC3 */
struct aframer *aframer_new(enum codec_id codec);
void aframer_free(struct aframer *f);
void aframer_reset(struct aframer *f);

/* Adds bytes. `pts` (NOPTS: none) is the time of the first access unit
 * that starts in them. */
void aframer_push(struct aframer *f, const uint8_t *data, size_t len, stc_t pts);

/* The next whole access unit, valid until the next call. `pts` is NOPTS
 * unless the stream gave a time for exactly this unit. */
bool aframer_next(struct aframer *f, const uint8_t **au, size_t *len, stc_t *pts);

/* Bytes passed over because they were not part of any frame. */
uint64_t aframer_skipped(const struct aframer *f);

/* Frames recognised as a coding we do not decode (E-AC-3 on an AC-3 PID). */
uint64_t aframer_foreign(const struct aframer *f);

#endif
