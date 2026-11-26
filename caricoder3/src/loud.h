/*
 * caricoder3 - audio loudness: measured (ITU-R BS.1770, as EBU R128 and
 * ATSC A/85 use it), levelled to a target, and limited
 * Copyright (c) 2026 CariTech Solutions
 *
 * Channels arrive at very different levels. Where a channel's audio is
 * transcoded, the samples pass through here between decoder and encoder:
 *
 *   measure   K-weighted loudness of what comes in, over the last 60 s,
 *             gated as BS.1770 gates (silence and quiet passages do not
 *             pull the figure down); nothing before 10 s of sound
 *   gain      auto: toward target - measured, within +/- LOUD_RANGE_DB:
 *             up 0.5 dB a second at most, down 2; a difference under 1 dB
 *             is left alone, and the gain stays where it is while nothing
 *             passes the gate (a pause is not turned up). The aim is
 *             channels at one level, not scenes: a programme keeps its own
 *             quiet and loud. fixed: the given dB
 *   limit     a look-ahead limiter after the gain: no sample over
 *             LOUD_CEILING_DB, so a channel turned up does not clip. It
 *             delays the audio LOUD_DELAY_MS, which the caller takes off
 *             the time stamps (loud_delay)
 *
 * What goes out is measured the same way, for the statistics.
 */
#ifndef CC3_LOUD_H
#define CC3_LOUD_H

#include <stdbool.h>
#include <stdint.h>

#define LOUD_TARGET_DEFAULT (-24.0)     /* LKFS */
#define LOUD_RANGE_DB       12.0
#define LOUD_CEILING_DB     (-2.0)      /* dBFS, of samples */
#define LOUD_DELAY_MS       5

enum loud_mode { LOUD_OFF = 0, LOUD_AUTO, LOUD_FIXED };

struct loud_cfg {
    int mode;                   /* enum loud_mode */
    double target;              /* auto: LKFS */
    double gain_db;             /* fixed */
};

struct loud_stats {
    bool on;
    double in_lufs, out_lufs;   /* NAN: not known yet (under 10 s of sound) */
    double gain_db;             /* applied now */
    uint64_t limited_ms;        /* how long the limiter has had to hold peaks down */
};

struct loud;

/* "auto", "off", or a gain in dB ("+3", "-4.5"); false: none of those */
bool loud_parse(const char *s, struct loud_cfg *c);

struct loud *loud_new(const struct loud_cfg *c, int rate, int ch);
void loud_free(struct loud *l);
/* samples the output lags the input by */
int loud_delay(const struct loud *l);
/* n frames of `ch` interleaved samples, in place */
void loud_process(struct loud *l, int16_t *pcm, int n);
void loud_get(const struct loud *l, struct loud_stats *st);

#endif
