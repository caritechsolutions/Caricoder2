/*
 * caricoder3 - transcoding one elementary stream
 * Copyright (c) 2026 CariTech Solutions
 *
 * An xcode takes the PES packets of one stream as the demux delivers
 * them, decodes, encodes, and hands back new PES packets as units for
 * the mux, each with its send window. It knows nothing of transport
 * packets, PIDs in use, or clocks: timestamps come in on the output time
 * line and go out on it.
 *
 * In a live run each xcode has a thread of its own, so that neither the
 * input nor the output ever waits on a codec. In a file run the work is
 * done in the caller's thread, which keeps such a run repeatable.
 */
#ifndef CC3_XCODE_H
#define CC3_XCODE_H

#include "cc3.h"
#include "codec.h"

struct xcode;

/* How one kind of stream is to be treated, as given on the command line. */
struct xcode_spec {
    int  pid;                   /* 0: every stream of this kind */
    enum codec_id codec;        /* CODEC_NONE: copy */
    int  bitrate;               /* bits per second, 0: a sensible default */
    int  width, height;         /* video: picture size, 0: as the source; -1 on one
                                 * side: that side follows the source's shape */
    bool fit;                   /* video: inside width x height, the source's shape kept */
};

struct xcode_cfg {
    uint16_t pid;               /* input stream */
    bool first_rendition;       /* the first transcoder of the video: makes the thumbnails */
    uint16_t out_pid;           /* output stream: the same, or another for a
                                 * further rendition of the same input */
    enum codec_id in, out;
    int  bitrate;
    /* video only */
    int  width, height;         /* 0: as the source; -1: follows the shape (see xcode_spec) */
    bool fit;
    enum codec_where decoder, encoder;
    int  gpu;
    int  vbv_ms, gop_ms;
    const char *preset;
    int bframes;                /* -1: the encoder's own choice */
    const char *profile;        /* NULL: the encoder's own choice */
};

struct xcode_stats {
    uint16_t pid, out_pid;
    int      width, height;             /* video out; 0 until the first picture */
    enum codec_id in, out;
    char     decoder[16], encoder[16];
    uint64_t units_in, units_out;
    uint64_t frames_in, frames_out;     /* access units decoded / produced */
    uint64_t errors;                    /* access units that would not decode */
    uint64_t overflow;                  /* input units dropped: the codec fell behind */
    uint64_t skipped_bytes;             /* input that was part of no frame */
    uint64_t silence_ms, cut_ms;        /* audio: holes filled, overlaps removed */
    uint64_t restarts;                  /* time line taken up afresh */
    unsigned queued;                    /* input units waiting right now */
    int64_t  slack_min;                 /* least time to spare on reaching the mux
                                         * since last read, ticks; INT64_MAX: none */
    int64_t  slack_ever;                /* the same since the transcoder started */
    bool     failed;                    /* could not be set up: stream is silent */
    uint8_t  card;                      /* enum xcode_card */
    uint32_t card_lost;                 /* times the card process went away */
};

enum xcode_card {
    XC_CARD_NONE = 0,                   /* transcoded in this process */
    XC_CARD_SHARED,                     /* in the card process */
    XC_CARD_WAITING,                    /* the card process is away: no video for now */
    XC_CARD_OWN,                        /* lost it too often: on the card in this process */
};

typedef void (*xcode_emit_fn)(void *opaque, struct es_unit *u);

/* NULL (with a reason in err) if no backend can do the job. */
struct xcode *xcode_new(const struct xcode_cfg *cfg, bool threaded,
                        xcode_emit_fn emit, void *opaque,
                        char *err, size_t errlen);

/* Stops the thread and frees everything. Units not yet processed are
 * discarded. */
void xcode_free(struct xcode *x);

/* One PES packet of the stream. Ownership passes. */
void xcode_push(struct xcode *x, struct es_unit *u);

/* End of input: everything queued is worked off and what the codecs
 * still hold comes out. In a live run this waits for the thread, but not
 * for long: a codec that is far behind is stopped and what it held is
 * lost. No more may be pushed afterwards. Not with the engine lock held:
 * the units that come out take it. */
void xcode_finish(struct xcode *x);

/*
 * When one transcoder takes over a PID from another (the source changed
 * its coding): the decode time of the last unit the old one gave out and
 * the time that unit has all been sent, asked after its xcode_finish(),
 * are handed to the new one before its first unit. Decode times on the
 * PID then still only move forward, and the new stream is sent after the
 * end of the old one, not on top of it. False if there is nothing to
 * hand on (audio; a transcoder that gave nothing out).
 */
bool xcode_last_out(const struct xcode *x, int64_t *dts90, stc_t *t1);
void xcode_start_after(struct xcode *x, int64_t dts90, stc_t t1);

void xcode_get_stats(struct xcode *x, struct xcode_stats *st);

/* For the engine, when it measures how much time a unit had to spare. */
void xcode_note_slack(struct xcode *x, int64_t slack, int64_t pts90);

/* "[PID=]codec[:bitrate]", e.g. "aac", "mp2:192k", "1653=aac:128k", "copy".
 * Returns false with a message in err. */
/* "[PID=][WxH:]codec[:bitrate]"; WxH only for video. */
bool xcode_parse_spec(const char *s, bool video, struct xcode_spec *out,
                      char *err, size_t errlen);

/* The codec a stream of the input is in, as far as we can transcode from
 * it: CODEC_NONE for anything else. */
struct program;
enum codec_id xcode_input_codec(const struct program *p, int idx);

/* ---- shared by the audio and video halves --------------------------- */

/* Start of the elementary stream data inside a PES packet, or -1. */
int pes_payload_offset(const uint8_t *pes, size_t len);

/*
 * Builds a PES packet around `len` payload bytes (to be filled in by the
 * caller at the returned unit's data + *payload_off). dts90 == pts90 or
 * NOPTS writes a PTS only.
 */
struct es_unit *pes_unit_new(uint16_t pid, uint8_t stream_id, size_t len,
                             int64_t pts90, int64_t dts90, size_t *payload_off);

/* The two halves, used only by xcode.c. */
struct axc;
struct axc *axc_new(const struct xcode_cfg *cfg, xcode_emit_fn emit, void *opaque,
                    struct xcode_stats *st, char *err, size_t errlen);
void axc_free(struct axc *a);
void axc_unit(struct axc *a, const struct es_unit *u);
void axc_idle(struct axc *a);
void axc_finish(struct axc *a);

struct vxc;
struct vxc *vxc_new(const struct xcode_cfg *cfg, xcode_emit_fn emit, void *opaque,
                    struct xcode_stats *st, char *err, size_t errlen);
void vxc_free(struct vxc *v);
void vxc_unit(struct vxc *v, const struct es_unit *u);
void vxc_finish(struct vxc *v);
void vxc_reset(struct vxc *v);
bool vxc_last_out(const struct vxc *v, int64_t *dts90, stc_t *t1);
void vxc_start_after(struct vxc *v, int64_t dts90, stc_t t1);
typedef void (*vxc_tap_fn)(void *opaque, const struct vframe *f);
void vxc_set_tap(struct vxc *v, vxc_tap_fn fn, void *opaque);
/* Decoder sharing (card.c): every picture this one decodes is shown to fn
 * too; and a picture decoded elsewhere taken in as if decoded here. */
void vxc_set_fanout(struct vxc *v, vxc_tap_fn fn, void *opaque);
void vxc_picture(struct vxc *v, const struct vframe *f);

/* The video half run in the card process instead (card.h, xc_remote.c).
 * xcode_use_card() is told once, at the start, where the card processes'
 * sockets are; "all" also sends transcoding that runs on the CPU there
 * (for tests on a machine without a card). */
void xcode_use_card(const char *run_dir, const char *channel, bool all);
struct rvxc;
struct rvxc *rvxc_new(const struct xcode_cfg *cfg, xcode_emit_fn emit, void *opaque,
                      struct xcode_stats *st, char *err, size_t errlen);
bool rvxc_wanted(const struct xcode_cfg *cfg);
void rvxc_free(struct rvxc *r);
void rvxc_unit(struct rvxc *r, const struct es_unit *u);
void rvxc_finish(struct rvxc *r);
void rvxc_reset(struct rvxc *r);
bool rvxc_last_out(const struct rvxc *r, int64_t *dts90, stc_t *t1);
void rvxc_start_after(struct rvxc *r, int64_t dts90, stc_t t1);

#endif
