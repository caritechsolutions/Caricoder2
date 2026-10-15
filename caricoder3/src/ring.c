/*
 * caricoder3 - a ring of transport packets between the mux and the outputs
 * Copyright (c) 2026 CariTech Solutions
 */
#include "ring.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

bool ring_init(struct ring *r)
{
    pthread_condattr_t ca;

    memset(r, 0, sizeof(*r));
    r->pkt = malloc((size_t)RING_PACKETS * TS_SIZE);
    if (!r->pkt)
        return false;
    pthread_mutex_init(&r->lock, NULL);
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&r->cond, &ca);
    pthread_condattr_destroy(&ca);
    return true;
}

void ring_destroy(struct ring *r)
{
    free(r->pkt);
    r->pkt = NULL;
    pthread_cond_destroy(&r->cond);
    pthread_mutex_destroy(&r->lock);
}

void ring_push(struct ring *r, const uint8_t *pkt)
{
    /* The slot is rewritten under the lock so that a reader copying it
     * never sees half of an old packet and half of a new one. */
    pthread_mutex_lock(&r->lock);
    memcpy(r->pkt[r->head % RING_PACKETS], pkt, TS_SIZE);
    r->head++;
    pthread_mutex_unlock(&r->lock);
}

void ring_publish(struct ring *r, bool flush)
{
    pthread_mutex_lock(&r->lock);
    if (flush)
        r->flush_at = r->head;
    pthread_cond_broadcast(&r->cond);
    pthread_mutex_unlock(&r->lock);
}

void ring_close(struct ring *r)
{
    pthread_mutex_lock(&r->lock);
    r->closed = true;
    pthread_cond_broadcast(&r->cond);
    pthread_mutex_unlock(&r->lock);
}

uint64_t ring_now(struct ring *r)
{
    uint64_t h;

    pthread_mutex_lock(&r->lock);
    h = r->head;
    pthread_mutex_unlock(&r->lock);
    return h;
}

int ring_read(struct ring *r, uint64_t *cursor, uint8_t (*out)[TS_SIZE], int max,
              int64_t timeout_ns, uint64_t flushed, uint64_t *lost, bool *flush,
              bool *closed)
{
    int n = 0;

    *lost = 0;
    *flush = false;
    *closed = false;
    pthread_mutex_lock(&r->lock);
    if (r->head == *cursor && !r->closed && timeout_ns > 0 &&
        !(r->flush_at == r->head && flushed != r->head)) {
        struct timespec ts;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec += timeout_ns / 1000000000LL;
        ts.tv_nsec += timeout_ns % 1000000000LL;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        while (r->head == *cursor && !r->closed)
            if (pthread_cond_timedwait(&r->cond, &r->lock, &ts) == ETIMEDOUT)
                break;
    }
    if (r->head - *cursor > RING_PACKETS) {
        /* Overrun: the oldest packets this reader wanted are gone. It
         * takes up again a little behind the writer, not right at it,
         * so that one slow moment does not become a second overrun. */
        uint64_t from = r->head - RING_PACKETS / 2;

        *lost = from - *cursor;
        *cursor = from;
    }
    while (*cursor < r->head && n < max) {
        memcpy(out[n++], r->pkt[*cursor % RING_PACKETS], TS_SIZE);
        (*cursor)++;
    }
    if (*cursor == r->head) {
        *flush = r->flush_at == r->head && flushed != r->head;
        *closed = r->closed;
    }
    pthread_mutex_unlock(&r->lock);
    return n;
}
