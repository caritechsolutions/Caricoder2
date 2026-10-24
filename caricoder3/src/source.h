/*
 * caricoder3 - the channel's sources: one in use, the others standing by
 * Copyright (c) 2026 CariTech Solutions
 *
 * A channel may be given several input URLs (-i more than once). The
 * first is the primary; while it delivers, the others are not opened.
 * When the one in use delivers nothing for `switch_ms` (default 3000),
 * the next is opened, round robin, and the switch is counted. While a
 * backup is in use, the primary is tried again every `retry_s` (default
 * 60) in a thread of its own; as soon as it delivers, the channel goes
 * back to it. A switch looks to the engine like any source restart: the
 * PCR jumps and the output time line is kept continuous.
 */
#ifndef CC3_SOURCE_H
#define CC3_SOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define SOURCES_MAX 4

struct sources;

struct sources *sources_new(int switch_ms, int retry_s);
void sources_free(struct sources *s);

/* Adds a URL (udp, rtp, srt, rist). Nothing is opened yet. */
bool sources_add(struct sources *s, const char *url, char *err, size_t errlen);
int sources_count(const struct sources *s);
const char *sources_url(const struct sources *s, int idx);

/* Opens the primary (the first). false with a reason when it cannot be. */
bool sources_start(struct sources *s, char *err, size_t errlen);

/* One datagram into buf, from whichever source is in use; 0 when none
 * arrived within about 200 ms (keep calling), -1 when every source is
 * finished for good. Switching happens inside. `now_ns` is the clock
 * the engine uses. */
int sources_recv(struct sources *s, uint8_t *buf, int size, int64_t now_ns);

struct source_stats {
    int active;                     /* index of the source in use */
    uint64_t switches;
    int64_t last_rx_ns;             /* 0 = nothing yet */
    uint64_t rx_overflow;           /* UDP: kernel socket buffer drops, the source in use */
    uint64_t rx_truncated;
    bool link;                      /* the source in use is SRT/RIST: link stats apply */
    char link_transport[8];
    char link_state[48];
    bool link_connected;
    double rtt_ms;
    int64_t lost, recovered;
    uint64_t reconnects, link_errors;
    bool multicast;                 /* the source in use is a UDP multicast group */
    unsigned rejoins;               /* ... joined again after quiet spells (udp_join_tick) */
    char joined_on[32];             /* ... the interface of its membership */
    char join_note[200];
};
/* Called now and then from the input thread while data flows: starts the
 * primary's retry when a backup is in use. */
void sources_tick(struct sources *s, int64_t now_ns);

void sources_stats(struct sources *s, struct source_stats *st);
/* "sources": [{"url": ..., "state": "in use"|"standing by"|"failed"|"trying", "failures": n}, ...] */
void sources_json(struct sources *s, FILE *f, void (*json_str)(FILE *, const char *));

#endif
