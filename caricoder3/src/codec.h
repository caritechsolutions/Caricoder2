/*
 * caricoder3 - codec backends
 * Copyright (c) 2026 CariTech Solutions
 *
 * A backend is one way of decoding or encoding one family of codecs:
 * x264 on the CPU, NVENC on an NVIDIA card, Intel VPL, fdk-aac and so on.
 * Each one is compiled in only if its library is present at build time,
 * and each one is probed at start-up, so the same binary runs on a box
 * with a GPU and on one without: a backend whose driver or hardware is
 * missing simply reports itself unavailable.
 *
 * Stage 1 ships no backends. Every stream is copied. This file fixes the
 * shape of the registry so stages 2 and 3 add backends without touching
 * the pipeline; the frame-level calls are added with the first backend
 * that needs them, not guessed at here.
 */
#ifndef CC3_CODEC_H
#define CC3_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

enum codec_media { CODEC_VIDEO, CODEC_AUDIO };
enum codec_role  { CODEC_DECODER, CODEC_ENCODER };

struct codec_backend {
    const char *name;           /* "x264", "nvenc", "vpl", "fdk-aac", ... */
    const char *codecs;         /* "h264", "h264,hevc", "aac", ... */
    enum codec_media media;
    enum codec_role role;
    int priority;               /* higher wins when several can do the job */

    /* Is the hardware / driver / library usable on this machine right now?
     * On failure a short reason goes into `why`. */
    bool (*probe)(char *why, size_t len);
};

/* What happens to one elementary stream. */
enum stream_mode {
    STREAM_COPY = 0,            /* passed through untouched */
    STREAM_TRANSCODE,           /* decode + encode (stage 2 onwards) */
    STREAM_DROP,
};

const struct codec_backend *const *codec_backends(int *count);
void codec_list(FILE *f);

#endif
