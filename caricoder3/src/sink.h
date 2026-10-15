/*
 * caricoder3 - outputs ("sinks")
 * Copyright (c) 2026 CariTech Solutions
 *
 * An output is a sink: something that takes the transport stream the mux
 * made and sends it somewhere. Each runs in a thread of its own, reading
 * from the ring (ring.h), so outputs can be added and removed while the
 * channel runs and none can hold up another. One type per URL scheme:
 * udp:// rtp:// file:// (udp.c and sink_udp.c), srt://, rist://, rtmp://
 * (later files).
 *
 * In a file run (no live clock) there are no threads: the one sink is
 * written in the caller's thread as packets come (sinks_write_sync).
 */
#ifndef CC3_SINK_H
#define CC3_SINK_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "cc3.h"
#include "ring.h"

#define SINK_ID_MAX     48
#define SINK_URL_MAX    512
#define SINKS_MAX       16
#define SINK_BATCH      448                 /* packets read from the ring at a time (64 datagrams) */

struct sink_stats {
    char     state[48];                     /* "sending", "connecting", "listening: 2 receivers" ... */
    uint64_t packets, bytes;                /* handed to the transport */
    uint64_t errors;                        /* sends that failed */
    uint64_t reconnects;
    /* what the transport offers, -1 when it does not */
    double   rtt_ms;
    int64_t  retransmitted, lost;
};

struct sink_ops {
    const char *name;                       /* "udp", "srt", ... */
    bool (*takes)(const char *url);         /* this type handles that URL */

    /* Opens (or starts connecting / listening; a sink that connects does
     * so in the background and reconnects by itself). NULL with a reason
     * in err only for something wrong with the URL or the machine. */
    void *(*open)(const char *url, char *err, size_t errlen);

    /* `n` packets to send, in order, now. n == 0: no packet has come for
     * a while, send whatever is part-built (a datagram waiting to fill). */
    void (*write)(void *ctx, const uint8_t (*pkt)[TS_SIZE], int n);

    void (*stats)(void *ctx, struct sink_stats *st);
    void (*close)(void *ctx);
};

struct sink {
    char id[SINK_ID_MAX];
    char url[SINK_URL_MAX];
    const struct sink_ops *ops;
    void *ctx;
    struct ring *ring;
    pthread_t th;
    bool threaded, stop;
    uint64_t cursor;
    uint64_t overruns;                      /* packets missed for being too slow */
    bool in_config;                         /* came from the config file (or was saved to it) */
};

struct sinks {
    struct ring *ring;
    bool live;
    pthread_mutex_t lock;
    struct sink *list[SINKS_MAX];
    int n;
    unsigned seq;                           /* for ids given by us */
    char last_id[SINK_ID_MAX];
};

bool sinks_init(struct sinks *s, struct ring *ring, bool live);
void sinks_destroy(struct sinks *s);

/* Adds an output. `id` may be NULL or empty: one is made up. Returns
 * false with a reason when the URL is not understood, the id is in use,
 * or the output could not be opened. */
bool sinks_add(struct sinks *s, const char *id, const char *url, bool from_config,
               char *err, size_t errlen);
bool sinks_remove(struct sinks *s, const char *id, char *err, size_t errlen);

/* File run only: write the packet to every sink now. */
void sinks_write_sync(struct sinks *s, const uint8_t *pkt);

/* No more packets: every sink sends what it holds and its thread ends. */
void sinks_finish(struct sinks *s);

/* The outputs as JSON (an array), for the statistics and the control
 * socket. Returns the number of outputs. */
int sinks_json(struct sinks *s, FILE *f, int indent);
uint64_t sinks_errors(struct sinks *s);
int sinks_count(struct sinks *s);
/* The URL of the first output, for the log line. */
const char *sinks_first_url(struct sinks *s, char *buf, size_t len);
/* The id of the output added last (for the reply to add-output). */
void sinks_last_id(struct sinks *s, char *buf, size_t len);
bool sinks_has(struct sinks *s, const char *id);
bool sinks_is_saved(struct sinks *s, const char *id);
/* Finds a saved output whose id is not among `keep`; false when none. */
bool sinks_saved_not_in(struct sinks *s, const char *const *keep, int n_keep,
                        char *id, size_t len);
/* The saved outputs only, as {"id","url"} objects, for the config file. */
void sinks_json_saved(struct sinks *s, FILE *f, int indent);

/* The known sink types, for --help and for choosing by URL. */
const struct sink_ops *sink_ops_for(const char *url);
/* Lists the output types this build has (for --backends). */
void sink_list(FILE *f);

/* ---- helpers for the sink types ---------------------------------------- */

/* The value of `name` in the URL's query (?a=1&b=2); false when absent. */
bool sink_url_param(const char *url, const char *name, char *buf, size_t len);
/* host and port of "scheme://host:port?..." (host may be empty; [v6]). */
bool sink_url_hostport(const char *url, char *host, size_t hostlen, int *port);

/* Packs transport packets into datagrams of `pkt` packets (7 by default,
 * 1316 bytes: what SRT and RIST carry). */
#define PACKER_MAX_PKT  7
struct packer {
    uint8_t buf[PACKER_MAX_PKT * TS_SIZE];
    int pkt, fill;
};
typedef void (*packer_fn)(void *ctx, const uint8_t *dgram, int len);
void packer_init(struct packer *p, int pkt);
/* Feeds `n` packets; `fn` is called for every datagram completed. */
void packer_feed(struct packer *p, const uint8_t (*pkts)[TS_SIZE], int n, packer_fn fn, void *ctx);
/* Sends the part-built datagram, if any. */
void packer_flush(struct packer *p, packer_fn fn, void *ctx);

#endif
