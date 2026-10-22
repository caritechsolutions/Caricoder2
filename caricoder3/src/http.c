/*
 * caricoder3 - a small HTTP/1.1 server (for the supervisor)
 * Copyright (c) 2026 CariTech Solutions
 */
#include "http.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "log.h"

#define MAX_CLIENTS     64
#define IO_TIMEOUT_MS   15000
#define HEADER_DEADLINE_S 10                /* the whole request header */
#define REQUEST_DEADLINE_S 120              /* header to last byte sent */

struct http_server {
    int fd;
    http_fn fn;
    void *opaque;
    pthread_t th;
    bool stop;
    pthread_mutex_t lock;
    int clients;
};

struct conn { struct http_server *s; int fd; };

static __thread time_t t_deadline;          /* this request's, 0 when none */

static int ms_left(void)
{
    time_t now = time(NULL);

    if (!t_deadline)
        return IO_TIMEOUT_MS;
    return t_deadline > now ? (int)((t_deadline - now) * 1000) : 0;
}

bool http_send(int fd, const void *d, size_t len)
{
    const uint8_t *p = d;

    while (len) {
        struct pollfd pf = { fd, POLLOUT, 0 };
        ssize_t w;
        int ms = ms_left();

        if (ms <= 0 || poll(&pf, 1, ms < IO_TIMEOUT_MS ? ms : IO_TIMEOUT_MS) <= 0)
            return false;
        w = send(fd, p, len, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            return false;
        }
        p += w;
        len -= (size_t)w;
    }
    return true;
}

static const char *reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default:  return "Status";
    }
}

static void send_response(int fd, const struct http_resp *r)
{
    char hdr[2048];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Cache-Control: no-cache\r\nConnection: close\r\n%s\r\n",
                     r->status, reason(r->status), r->type ? r->type : "text/plain",
                     r->body_len, r->extra);

    if (http_send(fd, hdr, (size_t)n) && r->body_len)
        http_send(fd, r->body, r->body_len);
}

void http_resp_text(struct http_resp *r, int status, const char *type, const char *text)
{
    free(r->body);
    r->status = status;
    r->type = type;
    r->body = strdup(text);
    r->body_len = r->body ? strlen(r->body) : 0;
}

void http_resp_printf(struct http_resp *r, int status, const char *type, const char *fmt, ...)
{
    va_list ap;
    char *text = NULL;
    int n;

    va_start(ap, fmt);
    n = vasprintf(&text, fmt, ap);
    va_end(ap);
    free(r->body);
    r->status = status;
    r->type = type;
    r->body = n >= 0 ? text : NULL;
    r->body_len = n >= 0 ? (size_t)n : 0;
}

void http_unescape(char *s)
{
    char *o = s;

    for (; *s; s++) {
        if (*s == '%' && s[1] && s[2]) {
            char h[3] = { s[1], s[2], 0 };

            *o++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else if (*s == '+') {
            *o++ = ' ';
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

bool http_query_param(const char *query, const char *name, char *buf, size_t len)
{
    size_t nl = strlen(name);
    const char *q = query;

    while (q && *q) {
        if (!strncmp(q, name, nl) && q[nl] == '=') {
            const char *v = q + nl + 1;
            const char *e = strchr(v, '&');
            size_t vl = e ? (size_t)(e - v) : strlen(v);

            if (vl >= len)
                vl = len - 1;
            memcpy(buf, v, vl);
            buf[vl] = 0;
            http_unescape(buf);
            return true;
        }
        q = strchr(q, '&');
        if (q)
            q++;
    }
    return false;
}

static bool read_more(int fd, char *buf, size_t *len, size_t cap)
{
    struct pollfd pf = { fd, POLLIN, 0 };
    ssize_t r;
    int ms = ms_left();

    if (*len >= cap || ms <= 0 || poll(&pf, 1, ms < IO_TIMEOUT_MS ? ms : IO_TIMEOUT_MS) <= 0)
        return false;
    r = recv(fd, buf + *len, cap - *len, 0);
    if (r <= 0)
        return false;
    *len += (size_t)r;
    return true;
}

static void *conn_thread(void *arg)
{
    struct conn *c = arg;
    struct http_server *s = c->s;
    char *buf = malloc(HTTP_HDR_MAX + HTTP_BODY_MAX + 1);
    size_t len = 0;
    char *hdr_end = NULL;
    struct http_req req;
    struct http_resp resp;

    prctl(PR_SET_NAME, "cc3-www");
    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));
    if (!buf)
        goto out;
    /* the header, within its own deadline */
    t_deadline = time(NULL) + HEADER_DEADLINE_S;
    for (;;) {
        if (!read_more(c->fd, buf, &len, HTTP_HDR_MAX))
            goto out;
        buf[len] = 0;
        hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end)
            break;
    }
    {
        char *line = buf, *p, *q;
        size_t clen = 0;

        hdr_end[2] = 0;                     /* headers end with one \r\n now */
        p = strchr(line, ' ');
        if (!p || (size_t)(p - line) >= sizeof(req.method)) {
            resp.status = 400;
            goto reply;
        }
        memcpy(req.method, line, (size_t)(p - line));
        req.method[p - line] = 0;
        p++;
        q = strchr(p, ' ');
        if (!q || (size_t)(q - p) >= sizeof(req.path)) {
            resp.status = 400;
            goto reply;
        }
        memcpy(req.path, p, (size_t)(q - p));
        req.path[q - p] = 0;
        if ((p = strchr(req.path, '?')) != NULL) {
            *p = 0;
            snprintf(req.query, sizeof(req.query), "%s", p + 1);
        }
        http_unescape(req.path);
        /* the headers we care about */
        for (p = strstr(line, "\r\n"); p && p[2]; p = strstr(p + 2, "\r\n")) {
            char *h = p + 2;

            if (!strncasecmp(h, "Content-Length:", 15))
                clen = (size_t)strtoul(h + 15, NULL, 10);
            else if (!strncasecmp(h, "Authorization:", 14)) {
                const char *v = h + 14;
                const char *e = strstr(v, "\r\n");

                while (*v == ' ')
                    v++;
                snprintf(req.auth, sizeof(req.auth), "%.*s", (int)(e ? e - v : (long)strlen(v)), v);
            }
        }
        if (clen > HTTP_BODY_MAX) {
            resp.status = 413;
            goto reply;
        }
        req.body = hdr_end + 4;
        {
            size_t have = len - (size_t)(req.body - buf);

            while (have < clen) {
                if (!read_more(c->fd, buf, &len, HTTP_HDR_MAX + HTTP_BODY_MAX))
                    goto out;
                have = len - (size_t)(req.body - buf);
            }
        }
        req.body[clen] = 0;
        req.body_len = clen;
    }
    req.fd = c->fd;
    resp.status = 404;
    resp.type = "text/plain";
    t_deadline = time(NULL) + REQUEST_DEADLINE_S;
    s->fn(s->opaque, &req, &resp);
reply:
    if (!resp.handled) {
        if (!resp.body) {
            char text[64];

            snprintf(text, sizeof(text), "%d %s\n", resp.status, reason(resp.status));
            http_resp_text(&resp, resp.status, "text/plain", text);
        }
        send_response(c->fd, &resp);
    }
out:
    free(resp.body);
    free(buf);
    close(c->fd);
    pthread_mutex_lock(&s->lock);
    s->clients--;
    pthread_mutex_unlock(&s->lock);
    free(c);
    return NULL;
}

static void *accept_thread(void *arg)
{
    struct http_server *s = arg;

    prctl(PR_SET_NAME, "cc3-www-accept");
    while (!__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE)) {
        struct pollfd pf = { s->fd, POLLIN, 0 };
        int fd;
        struct conn *c;
        pthread_t th;
        pthread_attr_t attr;
        bool full;
        int one = 1;

        if (poll(&pf, 1, 200) <= 0)
            continue;
        fd = accept4(s->fd, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0)
            continue;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        pthread_mutex_lock(&s->lock);
        full = s->clients >= MAX_CLIENTS;
        if (!full)
            s->clients++;
        pthread_mutex_unlock(&s->lock);
        if (full) {
            http_send(fd, "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", 73);
            close(fd);
            continue;
        }
        c = malloc(sizeof(*c));
        if (!c) {
            close(fd);
            continue;
        }
        c->s = s;
        c->fd = fd;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);      /* the API handler's frame is large (~370 kB) */
        if (pthread_create(&th, &attr, conn_thread, c) != 0) {
            close(fd);
            free(c);
            pthread_mutex_lock(&s->lock);
            s->clients--;
            pthread_mutex_unlock(&s->lock);
        }
        pthread_attr_destroy(&attr);
    }
    return NULL;
}

struct http_server *http_start(const char *bind_addr, int port, http_fn fn, void *opaque,
                               char *err, size_t errlen)
{
    struct http_server *s = calloc(1, sizeof(*s));
    struct addrinfo hints, *res = NULL;
    char portstr[8];
    int rc, one = 1;

    if (!s) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    s->fn = fn;
    s->opaque = opaque;
    s->fd = -1;
    pthread_mutex_init(&s->lock, NULL);
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    snprintf(portstr, sizeof(portstr), "%d", port);
    rc = getaddrinfo(bind_addr && bind_addr[0] ? bind_addr : NULL, portstr, &hints, &res);
    if (rc != 0) {
        snprintf(err, errlen, "%s: %s", bind_addr, gai_strerror(rc));
        free(s);
        return NULL;
    }
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        s->fd = socket(ai->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (s->fd < 0)
            continue;
        setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(s->fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(s->fd, 64) == 0)
            break;
        snprintf(err, errlen, "%s:%d: %s", bind_addr && bind_addr[0] ? bind_addr : "*", port, strerror(errno));
        close(s->fd);
        s->fd = -1;
    }
    freeaddrinfo(res);
    if (s->fd < 0) {
        if (!err[0])
            snprintf(err, errlen, "cannot listen on %s:%d", bind_addr ? bind_addr : "*", port);
        free(s);
        return NULL;
    }
    if (pthread_create(&s->th, NULL, accept_thread, s) != 0) {
        snprintf(err, errlen, "cannot start the web thread");
        close(s->fd);
        free(s);
        return NULL;
    }
    return s;
}

void http_stop(struct http_server *s)
{
    if (!s)
        return;
    __atomic_store_n(&s->stop, true, __ATOMIC_RELEASE);
    pthread_join(s->th, NULL);
    close(s->fd);
    for (int i = 0; i < 50; i++) {
        bool none;

        pthread_mutex_lock(&s->lock);
        none = s->clients == 0;
        pthread_mutex_unlock(&s->lock);
        if (none)
            break;
        usleep(100000);
    }
    pthread_mutex_destroy(&s->lock);
    free(s);
}
