/*
 * caricoder3 - a ring of transport packets between the mux and the outputs
 * Copyright (c) 2026 CariTech Solutions
 *
 * The packet path takes no lock. The writer copies a packet into its slot
 * and then publishes the new head with a release store; a reader loads
 * the head with an acquire load, copies what is new, and then checks it
 * was not lapped by the writer while copying (the head moved past its
 * cursor by a whole ring): if it was, what it copied may be mixed and it
 * starts again from the oldest packet still safe. The mutex and the
 * condition variable serve only to sleep until the writer wakes readers,
 * and the writer makes that system call only when someone is asleep.
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
    uint64_t h = atomic_load_explicit(&r->head, memory_order_relaxed);

    memcpy(r->pkt[h % RING_PACKETS], pkt, TS_SIZE);
    atomic_store_explicit(&r->head, h + 1, memory_order_release);
}

void ring_publish(struct ring *r, bool flush)
{
    if (flush)
        atomic_store_explicit(&r->flush_at,
                              atomic_load_explicit(&r->head, memory_order_relaxed),
                              memory_order_release);
    /* The lock orders this against a reader that has just decided to
     * sleep: it either sees the new head before sleeping, or sleeps
     * before we broadcast and is woken. Without a sleeper, no system
     * call at all. */
    if (atomic_load_explicit(&r->waiters, memory_order_acquire) > 0) {
        pthread_mutex_lock(&r->lock);
        pthread_cond_broadcast(&r->cond);
        pthread_mutex_unlock(&r->lock);
    }
}

void ring_close(struct ring *r)
{
    pthread_mutex_lock(&r->lock);
    atomic_store_explicit(&r->closed, true, memory_order_release);
    pthread_cond_broadcast(&r->cond);
    pthread_mutex_unlock(&r->lock);
}

uint64_t ring_now(struct ring *r)
{
    return atomic_load_explicit(&r->head, memory_order_acquire);
}

/* Sleeps until the head moves past `cursor`, the ring closes, or the
 * timeout. Returns whether anything changed. */
static void wait_for(struct ring *r, uint64_t cursor, int64_t timeout_ns)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += timeout_ns / 1000000000LL;
    ts.tv_nsec += timeout_ns % 1000000000LL;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&r->lock);
    atomic_fetch_add_explicit(&r->waiters, 1, memory_order_acq_rel);
    while (atomic_load_explicit(&r->head, memory_order_acquire) == cursor &&
           !atomic_load_explicit(&r->closed, memory_order_acquire))
        if (pthread_cond_timedwait(&r->cond, &r->lock, &ts) == ETIMEDOUT)
            break;
    atomic_fetch_sub_explicit(&r->waiters, 1, memory_order_acq_rel);
    pthread_mutex_unlock(&r->lock);
}

int ring_read(struct ring *r, uint64_t *cursor, uint8_t (*out)[TS_SIZE], int max,
              int64_t timeout_ns, uint64_t flushed, uint64_t *lost, bool *flush,
              bool *closed)
{
    uint64_t head, flush_at;
    int n;

    *lost = 0;
    *flush = false;
    *closed = false;
    head = atomic_load_explicit(&r->head, memory_order_acquire);
    if (head == *cursor && timeout_ns > 0 &&
        !atomic_load_explicit(&r->closed, memory_order_acquire) &&
        !(atomic_load_explicit(&r->flush_at, memory_order_acquire) == head && flushed != head)) {
        wait_for(r, *cursor, timeout_ns);
        head = atomic_load_explicit(&r->head, memory_order_acquire);
    }
    for (;;) {
        uint64_t from = *cursor;

        if (head - from >= RING_PACKETS) {    /* the slot of `from` may be being rewritten */
            /* Overrun: the oldest packets this reader wanted are gone. It
             * takes up again a little behind the writer, not right at it,
             * so that one slow moment does not become a second overrun. */
            from = head - RING_PACKETS / 2;
            *lost += from - *cursor;
            *cursor = from;
        }
        n = 0;
        while (from + (uint64_t)n < head && n < max) {
            memcpy(out[n], r->pkt[(from + (uint64_t)n) % RING_PACKETS], TS_SIZE);
            n++;
        }
        /* Lapped while copying? Then some of `out` may be mixed: again. */
        head = atomic_load_explicit(&r->head, memory_order_acquire);
        if (head - from < RING_PACKETS) {
            *cursor = from + (uint64_t)n;
            break;
        }
    }
    if (*cursor == head) {
        flush_at = atomic_load_explicit(&r->flush_at, memory_order_acquire);
        *flush = flush_at == head && flushed != head;
        *closed = atomic_load_explicit(&r->closed, memory_order_acquire);
    }
    return n;
}
