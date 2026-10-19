/*
 * caricoder3 - the other transcoders: asking them, keeping track of them
 * Copyright (c) 2026 CariTech Solutions
 *
 * Each transcoder's supervisor knows its peers by address and shares a
 * key with them. In a thread of its own it says hello to each every 30 s
 * (are you there, how busy, how many incidents open) and asks them
 * questions when one of its own problems could be told apart by looking
 * from another place: "I lost this source; can you see it from your
 * side?". Every question and answer is a line of the conversation in the
 * history (messages), and the answers are evidence for the incident that
 * asked. The other side of it, answering, is in the supervisor
 * (/api/peer/...).
 */
#ifndef CC3_PEER_H
#define CC3_PEER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define PEERS_MAX 16

struct peer_conf {
    char name[64];
    char url[256];              /* http://host:port */
};

struct peer_state {
    char name[64];
    char url[256];
    bool reachable;
    int64_t last_ok, last_try;  /* wall clock */
    char version[48], host[64];
    int channels, running, open_incidents;
    double cpu;
    char error[160];
};

/* Starts the thread (once); the list, this server's name and the key can
 * be changed at any time. */
void peer_set(const struct peer_conf *list, int n, const char *my_name, const char *key);
void peer_stop(void);
int peer_get(struct peer_state *out, int max);

/* Asks every reachable peer whether it sees `url` (`program` 0: any),
 * for `seconds`; `why` in a few words for the conversation. The answers
 * become evidence for `incident` (0: none). Returns how many were asked. */
int peer_ask_check(const char *url, int program, int seconds, int64_t incident, const char *why);

/* A request to another caricoder3: `body` NULL for none. The answer's
 * body is malloc'd into *out (may be NULL); returns the HTTP status, or
 * -1 with *out a malloc'd reason. */
int peer_http(const char *url, const char *method, const char *path, const char *key, const char *body,
              int timeout_ms, char **out);

#endif
