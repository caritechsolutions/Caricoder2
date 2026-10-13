/*
 * caricoder3 - codec backends
 * Copyright (c) 2026 CariTech Solutions
 *
 * A backend is one way of decoding or encoding one family of codecs:
 * x264 on the CPU, NVENC on an NVIDIA card, fdk-aac and so on. Each one
 * is compiled in only if its library is present at build time (the
 * Makefile sets HAVE_xxx), and each one is probed at start-up, so the same
 * binary runs on a box with a GPU and on one without: a backend whose
 * driver or hardware is missing reports itself unavailable and the next
 * one is tried.
 *
 * The pipeline never calls a codec library itself. It sees only the four
 * small interfaces below. Timestamps crossing them are 27 MHz ticks
 * (stc_t) on the output time line.
 */
#ifndef CC3_CODEC_H
#define CC3_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "cc3.h"

enum codec_media { CODEC_VIDEO, CODEC_AUDIO };
enum codec_role  { CODEC_DECODER, CODEC_ENCODER };

enum codec_id {
    CODEC_NONE = 0,             /* not one we handle: the stream is copied */
    CODEC_MPA,                  /* MPEG-1/2 audio, layer I, II or III (input) */
    CODEC_MP2,                  /* MPEG audio layer II (output) */
    CODEC_MP3,                  /* MPEG audio layer III (output) */
    CODEC_AAC,                  /* AAC in ADTS */
    CODEC_AAC_LATM,             /* AAC in LOAS/LATM (input) */
    CODEC_AC3,                  /* AC-3 (input) */
    CODEC_H264,
    CODEC_HEVC,
};

const char *codec_name(enum codec_id id);
bool codec_is_video(enum codec_id id);

/* What happens to one elementary stream. */
enum stream_mode {
    STREAM_COPY = 0,            /* passed through untouched */
    STREAM_TRANSCODE,           /* decoded and encoded again */
    STREAM_DROP,
};

/* Where a job may run. */
enum codec_where { WHERE_AUTO = 0, WHERE_CPU, WHERE_NVIDIA };

/* ---- audio ---------------------------------------------------------- */

#define AUDIO_MAX_CH        2           /* everything is mixed down to stereo */
#define AUDIO_MAX_FRAME     4096        /* samples per channel in one access unit */

struct adec_ops {
    void *(*open)(enum codec_id codec, char *err, size_t errlen);

    /*
     * One whole access unit in, interleaved 16-bit samples out.
     * Returns the number of samples per channel (0: nothing yet), or -1
     * if the unit could not be decoded (it is skipped). `pcm` has room
     * for AUDIO_MAX_FRAME * AUDIO_MAX_CH samples. `delay` receives the
     * number of samples by which the output lags the unit just given.
     */
    int (*decode)(void *ctx, const uint8_t *au, size_t len, int16_t *pcm,
                  int *rate, int *channels, int *delay);

    /* The input was interrupted: forget what was half done. */
    void (*reset)(void *ctx);
    void (*close)(void *ctx);
};

struct aenc_ops {
    /*
     * `bitrate` is the rate wanted on the way in and the rate that will
     * be used on the way out: the nearest the coding allows at this
     * sample rate and channel count. `frame` receives the number of
     * samples per channel each encode() call takes, `delay` the number
     * of samples by which a standard decoder's output lags the encoder's
     * input.
     */
    void *(*open)(enum codec_id codec, int rate, int channels, int *bitrate,
                  int *frame, int *delay, char *err, size_t errlen);

    /* Exactly one frame of interleaved samples in. Returns the number of
     * bytes written to `out` (whole access units, perhaps none yet, perhaps
     * more than one), or -1. */
    int (*encode)(void *ctx, const int16_t *pcm, uint8_t *out, size_t max);
    void (*close)(void *ctx);
};

/* ---- video ---------------------------------------------------------- */

enum vframe_mem {
    VMEM_CPU = 0,               /* planar 4:2:0 in ordinary memory */
    VMEM_CUDA,                  /* NV12 / P010 on an NVIDIA card */
};

struct vframe {
    enum vframe_mem mem;
    int      width, height;
    int      depth;             /* bits per sample: 8 or 10 */
    uint8_t *plane[3];          /* CPU: Y, U, V. CUDA: plane[0] is the device
                                 * pointer of the whole NV12/P010 surface */
    int      stride[3];
    stc_t    pts;
    bool     key;               /* decoder: it was a random access point */
    bool     interlaced, tff;
    int      sar_num, sar_den;  /* 0 = unknown */
    int      fps_num, fps_den;  /* 0 = unknown */
    int      primaries, transfer, matrix, full_range;   /* H.273 code points */
    void    *cuda_ctx;          /* CUDA: the context the surface lives in */
};

struct vdec_cfg {
    enum codec_id codec;
    int  gpu;                   /* card index for a hardware decoder */
    bool want_cuda;             /* leave frames on the card if possible */
};

struct vdec_ops {
    void *(*open)(const struct vdec_cfg *cfg, char *err, size_t errlen);

    /* Elementary stream bytes as they come out of PES packets, cut
     * anywhere. pts/dts (90 kHz, NOPTS if absent) belong to the first
     * access unit that starts in this piece. Returns 0, or -1. */
    int (*push)(void *ctx, const uint8_t *es, size_t len, int64_t pts90,
                int64_t dts90);

    /* Next picture in display order. 1: `f` is filled and stays valid
     * until the next call into this decoder; 0: needs more input. */
    int (*pull)(void *ctx, struct vframe *f);

    /* End of input: after this, pull() hands out what is still inside. */
    void (*drain)(void *ctx);
    void (*reset)(void *ctx);
    void (*close)(void *ctx);
};

struct venc_cfg {
    enum codec_id codec;
    int  width, height, depth;
    int  fps_num, fps_den;
    int  sar_num, sar_den;
    bool interlaced, tff;
    int  primaries, transfer, matrix, full_range;
    int  bitrate;               /* bits per second */
    int  vbv_ms;                /* decoder buffer, as time at `bitrate` */
    int  gop_ms;                /* longest distance between key frames */
    const char *preset;         /* backend's own name for speed, or NULL */
    int  gpu;
    void *cuda_ctx;             /* frames will arrive in this CUDA context */
};

struct vpacket {
    const uint8_t *data;        /* one access unit, Annex B, valid until the
                                 * next call into this encoder */
    size_t  len;
    stc_t   pts, dts;
    bool    key;
};

struct venc_ops {
    void *(*open)(const struct venc_cfg *cfg, char *err, size_t errlen);

    /* One picture in (NULL: end of input). Returns 0, or -1. */
    int (*push)(void *ctx, const struct vframe *f);

    /* 1: `p` is filled; 0: nothing ready. */
    int (*pull)(void *ctx, struct vpacket *p);

    /* Most pictures the encoder may hold back before the first comes out. */
    int (*delay_frames)(void *ctx);
    void (*close)(void *ctx);
};

/* ---- the registry --------------------------------------------------- */

struct codec_backend {
    const char *name;           /* "x264", "nvenc", "fdk-aac", ... */
    const char *codecs;         /* for the listing: "h264", "aac", ... */
    enum codec_media media;
    enum codec_role role;
    enum codec_where where;     /* WHERE_CPU or WHERE_NVIDIA */
    int priority;               /* higher wins when several can do the job */

    bool (*handles)(enum codec_id codec);

    /* Is the hardware / driver / library usable on this machine right now?
     * On failure a short reason goes into `why`. NULL: always. */
    bool (*probe)(char *why, size_t len);

    const struct adec_ops *adec;
    const struct aenc_ops *aenc;
    const struct vdec_ops *vdec;
    const struct venc_ops *venc;
};

const struct codec_backend *const *codec_backends(int *count);
void codec_list(FILE *f);

/*
 * The best usable backend for a job, or NULL. `where` narrows the choice
 * (WHERE_AUTO: hardware first, then CPU). `skip` backends are passed over,
 * so that the caller can ask for the next best after one failed to open.
 */
const struct codec_backend *codec_find(enum codec_media media, enum codec_role role,
                                       enum codec_id codec, enum codec_where where,
                                       int skip);

#endif
