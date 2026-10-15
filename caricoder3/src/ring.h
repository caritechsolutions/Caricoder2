/*
 * caricoder3 - a ring of transport packets between the mux and the outputs
 * Copyright (c) 2026 CariTech Solutions
 *
 * One writer (the mux, in the output thread), any number of readers (the
 * sinks, each in its own thread). A reader keeps a cursor: the sequence
 * number of the next packet it wants. The writer never waits for a
 * reader: one that has fallen a whole ring behind has its cursor moved
 * up and the packets it missed counted for it alone, so no sink can slow
 * another or the pipeline.
 */
#ifndef CC3_RING_H
#define CC3_RING_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "cc3.h"

#define RING_PACKETS    16384               /* 3 MB; 0.6 s at 40 Mb/s */

struct ring {
    uint8_t (*pkt)[TS_SIZE];
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    uint64_t head;                          /* sequence number of the next packet written */
    uint64_t flush_at;                      /* head when the writer last asked for a flush */
    bool     closed;                        /* no more will come: readers may leave */
};

bool ring_init(struct ring *r);
void ring_destroy(struct ring *r);

/* Writer. push() is cheap and does not wake anyone; publish() wakes the
 * readers (call it once a batch is in, so they are not woken per packet).
 * `flush` tells them no more is coming for a while, so a part-built
 * datagram should go now rather than wait. */
void ring_push(struct ring *r, const uint8_t *pkt);
void ring_publish(struct ring *r, bool flush);
void ring_close(struct ring *r);

/*
 * Reader. Copies up to `max` packets from `*cursor` on into `out`, waits
 * up to `timeout_ns` for the first if none is there. Returns the number
 * copied (0 on timeout, flush or close). If the reader was overrun,
 * `*cursor` is moved to the oldest packet still held and `*lost` receives
 * how many it missed. `*flush` is set when the reader has everything and
 * the writer asked for a flush since `flushed` (the reader's cursor at
 * its last flush; the reader does not wait in that case); `*closed` when
 * it has everything and no more will come.
 */
int ring_read(struct ring *r, uint64_t *cursor, uint8_t (*out)[TS_SIZE], int max,
              int64_t timeout_ns, uint64_t flushed, uint64_t *lost, bool *flush,
              bool *closed);

/* Where a new reader starts: the next packet to be written. */
uint64_t ring_now(struct ring *r);

#endif
