/*
 * caricoder3 - watching the content: black or frozen picture, silent audio
 * Copyright (c) 2026 CariTech Solutions
 *
 * The picture is looked at when a thumbnail is made (every few seconds,
 * thumb.c): how dark it is, and how much it differs from the last one.
 * Each audio stream is decoded for one second in every three, on the CPU,
 * and its level measured. What is found is read by the statistics, which
 * say when a picture has been black or still, or a stream silent, for a
 * while. A PID's state is forgotten when nothing of it comes for a minute.
 */
#ifndef CC3_WATCH_H
#define CC3_WATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "codec.h"

#define WATCH_AUDIO_MAX 16

/* The picture at thumbnail size: 8-bit luma, `w` x `h`. */
void watch_picture(const uint8_t *y, int w, int h);

/* An audio PES packet of inside PID `pid` (the whole PES, header included). */
void watch_audio_unit(uint16_t pid, enum codec_id codec, const uint8_t *pes, size_t len);

struct watch_audio {
    uint16_t pid;
    bool measured;              /* a window has been decoded */
    double level_db;            /* RMS of the last window, dBFS */
    double peak_db;
    int64_t silent_ms;          /* silent this long (0: not silent) */
    int channels, rate;
};

struct watch_stats {
    bool have_picture;
    int64_t picture_age_ms;     /* since the last picture was looked at */
    double luma;                /* mean, 0..255 */
    double change;              /* mean difference from the picture before, 0..255 */
    int64_t black_ms, frozen_ms;    /* black / unchanged this long (0: not) */
    int n_audio;
    struct watch_audio audio[WATCH_AUDIO_MAX];
};

void watch_get(struct watch_stats *out);

#endif
