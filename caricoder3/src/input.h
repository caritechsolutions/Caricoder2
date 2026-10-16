/*
 * caricoder3 - link inputs: transports that are not a plain UDP socket
 * Copyright (c) 2026 CariTech Solutions
 *
 * A UDP (or RTP) source is read by main.c with recvmmsg straight into
 * the engine. SRT and RIST sources go through a library that does the
 * connecting and the recovery; this is the small interface main.c reads
 * them through: open, receive one datagram at a time (with a timeout,
 * so the thread stays responsive), statistics, close.
 */
#ifndef CC3_INPUT_H
#define CC3_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INPUT_DGRAM_MAX 1500            /* the libraries deliver whole datagrams: 7 x 188 + headers */

struct input_stats {
    char state[48];                     /* "connected", "listening: no sender" ... */
    bool connected;
    double rtt_ms;                      /* 0 when not known */
    int64_t lost;                       /* packets the link could not recover */
    int64_t retransmitted;              /* packets recovered by retransmission */
    uint64_t reconnects;
    uint64_t errors;
    int64_t bytes;                      /* received so far */
};

struct input_ops {
    const char *name;                   /* "srt", "rist" */
    bool (*takes)(const char *url);
    void *(*open)(const char *url, char *err, size_t errlen);
    /* One datagram into buf. Returns its length, 0 when none arrived
     * within about 200 ms (keep polling), -1 when the input is finished
     * for good (the thread should stop). */
    int (*recv)(void *ctx, uint8_t *buf, int size);
    void (*stats)(void *ctx, struct input_stats *st);
    void (*close)(void *ctx);
};

/* The transport for this URL, or NULL when it is UDP/RTP/a file. */
const struct input_ops *input_ops_for(const char *url);

/* true for every live transport: udp, rtp, srt, rist */
bool input_is_live(const char *url);

/* Transports compiled in, as a comma list ("udp, rtp, srt, rist"). */
const char *input_transports(void);

#endif
