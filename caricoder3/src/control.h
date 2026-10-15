/*
 * caricoder3 - the control socket
 * Copyright (c) 2026 CariTech Solutions
 *
 * A Unix socket on which a client sends one JSON object per line and gets
 * one back: {"cmd": "list-outputs"}, {"cmd": "add-output", "url": ...,
 * "id": ...}, {"cmd": "remove-output", "id": ...}, {"cmd": "stats"}.
 * The reply always has "ok": true or false, and "error" when false.
 * The supervisor and the GUI use nothing else; `caricoder3 ctl` is the
 * command-line client.
 */
#ifndef CC3_CONTROL_H
#define CC3_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "json.h"

/* Called in the control thread with the parsed request (root is an
 * object); writes the reply (one JSON object, no newline) to `reply`. */
typedef void (*control_fn)(void *opaque, const struct json_doc *req, FILE *reply);

struct control;

/* Listens on `path` (an existing socket file there is replaced). */
struct control *control_start(const char *path, control_fn fn, void *opaque,
                              char *err, size_t errlen);
void control_stop(struct control *c);

/* The client side: sends `request` to the socket at `path`, prints the
 * reply to `out`. Returns the exit status for the shell: 0 when the
 * reply says ok, 1 when it says not, 2 when the socket cannot be reached. */
int control_client(const char *path, const char *request, FILE *out);

#endif
