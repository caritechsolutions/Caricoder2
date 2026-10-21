/*
 * caricoder3 - the card process: video transcoding for many channels in
 * one process per NVIDIA card
 * Copyright (c) 2026 CariTech Solutions
 *
 * Every process that uses a card pays for a workspace on it (a CUDA
 * context, about 150 MB) before it decodes a single picture. With a
 * process per channel that is paid per channel. So the supervisor runs
 * one card process per card (caricoder3 card --card N --socket PATH) and
 * the channels hand their video transcoding to it: the channel sends the
 * source's video as PES packets over a Unix socket, the card process
 * decodes and encodes it on the card exactly as the channel would have
 * (xc_video.c), and sends the new PES packets back with their send
 * windows. Pictures never leave the card; only compressed video crosses.
 *
 * One connection per transcoding session (a rendition of a channel), and
 * one thread per connection in the card process. The channel keeps
 * everything else: input, timing, audio, multiplexing, outputs.
 *
 * If the card process ends, the channels keep running without video for
 * the moment, connect again once the supervisor has started it anew, and
 * pick up at the next key frame. A channel that has lost it CARD_FAILS
 * times in CARD_FAIL_WINDOW_S goes back to using the card on its own.
 *
 * The messages: a header (type, payload length, both 32-bit host order:
 * both ends are this very program on this very machine) and the payload.
 */
#ifndef CC3_CARD_H
#define CC3_CARD_H

#include <stdint.h>

#include "cc3.h"

#define CARD_PROTO          3
#define CARD_FAILS          3
#define CARD_FAIL_WINDOW_S  600
#define CARD_MSG_MAX        (16u * 1024 * 1024)

enum card_msg {
    /* channel -> card */
    CM_HELLO = 1,           /* struct card_hello */
    CM_UNIT,                /* struct card_unit + PES bytes */
    CM_RESET,               /* input was thrown away */
    CM_START_AFTER,         /* struct card_after */
    CM_FINISH,              /* end of input: let out what is held */
    CM_THUMB_REQ,           /* a small picture, please, from the next one decoded */
    /* card -> channel */
    CM_READY = 64,          /* struct card_ready */
    CM_OUT,                 /* struct card_unit + PES bytes */
    CM_STATS,               /* struct card_stats */
    CM_THUMB,               /* struct card_thumb + 8-bit Y, U, V */
    CM_FINISHED,
};

struct card_hdr {
    uint32_t type, len;
};

struct card_hello {
    uint32_t proto;
    uint32_t size;          /* sizeof(struct card_hello): the same program at both ends */
    uint16_t pid, out_pid;
    int32_t  in, out, bitrate, width, height, fit, decoder, encoder, gpu;
    int32_t  vbv_ms, gop_ms, bframes, first_rendition;
    char     preset[32], profile[32];
    char     channel[64];   /* for the card process's log */
};

struct card_ready {
    int32_t ok;
    char    decoder[16], encoder[16];
    char    err[200];
};

struct card_unit {
    uint16_t pid;
    uint8_t  kind, rai;
    int32_t  pts_off, dts_off;
    int64_t  pts, dts, t0, t1;
    uint32_t len;
};

struct card_after {
    int64_t dts90, t1;
};

struct card_stats {
    uint64_t frames_in, frames_out, units_out, errors, restarts;
    int32_t  width, height, failed;
    char     decoder[16], encoder[16];
};

struct card_thumb {
    int32_t width, height;
};

/* The socket of card N under the run folder. */
void card_socket_path(const char *run_dir, int card, char *out, size_t len);

/* caricoder3 card --card N --socket PATH [--cpu] */
int card_main(int argc, char **argv);

/* Sends one message whole; false when the other end is gone. */
bool card_send(int fd, uint32_t type, const void *a, size_t alen, const void *b, size_t blen);
/* Reads one message; the payload is malloc'd (NULL when empty). False at
 * the end of the connection or on garbage. */
bool card_recv(int fd, struct card_hdr *h, uint8_t **payload);

#endif
