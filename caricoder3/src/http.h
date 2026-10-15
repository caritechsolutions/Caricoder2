/*
 * caricoder3 - a small HTTP/1.1 server (for the supervisor)
 * Copyright (c) 2026 CariTech Solutions
 *
 * One thread per request, no keep-alive, GET/POST/PUT/DELETE with a body
 * up to HTTP_BODY_MAX. The handler gets the parsed request and fills a
 * response; the server sends it and closes.
 */
#ifndef CC3_HTTP_H
#define CC3_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HTTP_BODY_MAX   (1 << 20)
#define HTTP_HDR_MAX    8192

struct http_req {
    char method[8];
    char path[1024];                        /* without the query */
    char query[1024];
    char auth[256];                         /* the Authorization header, if any */
    char *body;
    size_t body_len;
    int fd;                                 /* the connection, for a handler that streams */
};

struct http_resp {
    int status;                             /* 200, 404 ... */
    const char *type;                       /* content type */
    char *body;                             /* malloc'd; freed by the server */
    size_t body_len;
    char extra[512];                        /* more header lines, each ending in \r\n */
    bool handled;                           /* the handler sent everything itself */
};

typedef void (*http_fn)(void *opaque, const struct http_req *req, struct http_resp *resp);

struct http_server;

struct http_server *http_start(const char *bind_addr, int port, http_fn fn, void *opaque,
                               char *err, size_t errlen);
void http_stop(struct http_server *s);

/* Helpers for handlers. */
void http_resp_text(struct http_resp *r, int status, const char *type, const char *text);
void http_resp_printf(struct http_resp *r, int status, const char *type, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
/* Sends a whole response on a connection (for streaming/proxy handlers). */
bool http_send(int fd, const void *d, size_t len);
/* Percent-decodes in place. */
void http_unescape(char *s);
/* The value of `name` in a query string; false when absent. */
bool http_query_param(const char *query, const char *name, char *buf, size_t len);

#endif
