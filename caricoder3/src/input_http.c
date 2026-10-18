/*
 * caricoder3 - input over HTTP: a TS stream pulled from a server
 * Copyright (c) 2026 CariTech Solutions
 *
 *   http://host[:port]/path        e.g. http://192.168.110.41/bet/mpegts
 *   http://user:pass@host/path     with basic authentication
 *
 * One GET; the body (plain or chunked) is a TS byte stream, cut here into
 * datagrams of up to 7 whole packets, sync found and kept. Redirects
 * (301, 302, 303, 307, 308) are followed, up to 5. When the connection
 * ends or breaks, or nothing arrives for 5 s, it connects again after a
 * second. A server starts with a burst of what it has buffered (seconds
 * of it, faster than real time): that is read and thrown away until the
 * PCR advances at about the speed of the clock, so the channel starts at
 * the live edge, as from UDP. From then on data is handed on as it
 * arrives. HTTPS is not supported.
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "cc3.h"
#include "input.h"
#include "log.h"

#define RECV_TIMEOUT_MS 200
#define CONNECT_TIMEOUT_MS 3000
#define IDLE_TIMEOUT_US 5000000
#define RECONNECT_US    1000000
#define BUF_MAX         (256 * 1024)
#define HDR_MAX         8192

enum hstate { H_IDLE, H_CONNECTING, H_HEADERS, H_BODY };

struct http_in {
    char url[1024];                     /* as given */
    char host[256], path[1024], auth[256];
    int port;
    int redirects;

    enum hstate st;
    int fd;
    int64_t next_try_us, since_us, last_data_us;
    char req[2048];
    size_t req_len, req_sent;
    char hdr[HDR_MAX];
    size_t hdr_len;
    bool chunked;
    int64_t chunk_left;                 /* bytes of the chunk still to come; -1 = a size line next */
    char line[32];
    int line_len;
    bool chunk_crlf;                    /* the CRLF after a chunk's data is next */

    uint8_t buf[BUF_MAX];               /* body bytes not yet handed on */
    size_t len;

    /* the start of each connection: the server's backlog is skipped */
    bool warming;
    int64_t warm_start_us;
    int pcr_pid;                        /* the first PID seen carrying a PCR; -1 = none yet */
    int64_t w_pcr, w_wall;              /* the window's start: PCR (27 MHz) and clock (us) */
    uint64_t skipped;                   /* bytes of backlog thrown away, this connection */

    char last_why[48];                  /* the last reason logged, and when: a server */
    int64_t last_log_us;                /* that stays away is not logged every second */

    pthread_mutex_t lock;
    char state[48];
    _Atomic uint64_t reconnects, errors;
    _Atomic int64_t bytes;
};

static int64_t now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void set_state(struct http_in *h, const char *s)
{
    pthread_mutex_lock(&h->lock);
    snprintf(h->state, sizeof(h->state), "%s", s);
    pthread_mutex_unlock(&h->lock);
}

static bool http_in_takes(const char *url)
{
    return !strncmp(url, "http://", 7);
}

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64(const char *in, char *out, size_t outlen)
{
    size_t n = strlen(in), o = 0;

    for (size_t i = 0; i < n && o + 5 < outlen; i += 3) {
        uint32_t v = (uint32_t)(uint8_t)in[i] << 16;

        if (i + 1 < n)
            v |= (uint32_t)(uint8_t)in[i + 1] << 8;
        if (i + 2 < n)
            v |= (uint8_t)in[i + 2];
        out[o++] = b64[(v >> 18) & 63];
        out[o++] = b64[(v >> 12) & 63];
        out[o++] = i + 1 < n ? b64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? b64[v & 63] : '=';
    }
    out[o] = 0;
}

/* http://[user:pass@]host[:port][/path] */
static bool parse_url(struct http_in *h, const char *url, char *err, size_t errlen)
{
    const char *p = url + 7, *slash = strchr(p, '/'), *at;
    char hostport[512];
    size_t n = slash ? (size_t)(slash - p) : strlen(p);
    char *colon;

    if (strncmp(url, "http://", 7)) {
        snprintf(err, errlen, "only http:// is supported (not https)");
        return false;
    }
    if (n == 0 || n >= sizeof(hostport)) {
        snprintf(err, errlen, "no host in %s", url);
        return false;
    }
    memcpy(hostport, p, n);
    hostport[n] = 0;
    h->auth[0] = 0;
    if ((at = strrchr(hostport, '@'))) {
        char cred[256];

        snprintf(cred, sizeof(cred), "%.*s", (int)(at - hostport), hostport);
        base64(cred, h->auth, sizeof(h->auth));
        memmove(hostport, at + 1, strlen(at + 1) + 1);
    }
    h->port = 80;
    if ((colon = strrchr(hostport, ':')) && !strchr(hostport, ']')) {
        h->port = atoi(colon + 1);
        *colon = 0;
    }
    if (h->port <= 0 || h->port > 65535 || !hostport[0]) {
        snprintf(err, errlen, "bad host or port in %s", url);
        return false;
    }
    snprintf(h->host, sizeof(h->host), "%.255s", hostport);
    snprintf(h->path, sizeof(h->path), "%s", slash ? slash : "/");
    return true;
}

static void drop_conn(struct http_in *h, const char *why, bool error)
{
    if (h->fd >= 0)
        close(h->fd);
    h->fd = -1;
    if (h->st != H_IDLE) {
        h->reconnects++;
        if (error)
            h->errors++;
        if (strcmp(why, h->last_why) || now_us() - h->last_log_us > 60000000) {
            log_warn("http: %s:%d%s: %s; connecting again", h->host, h->port, h->path, why);
            snprintf(h->last_why, sizeof(h->last_why), "%s", why);
            h->last_log_us = now_us();
        }
    }
    h->st = H_IDLE;
    h->len = 0;
    h->next_try_us = now_us() + RECONNECT_US;
    set_state(h, why);
}

static bool start_connect(struct http_in *h)
{
    struct addrinfo hints, *ai = NULL;
    char port[8];
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof(port), "%d", h->port);
    if (getaddrinfo(h->host, port, &hints, &ai) != 0 || !ai) {
        h->next_try_us = now_us() + RECONNECT_US;
        set_state(h, "cannot resolve the host");
        return false;
    }
    fd = socket(ai->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
        int sz = 4 << 20;

        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0 && errno != EINPROGRESS) {
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(ai);
    if (fd < 0) {
        h->next_try_us = now_us() + RECONNECT_US;
        set_state(h, "cannot connect");
        return false;
    }
    h->fd = fd;
    h->st = H_CONNECTING;
    h->since_us = now_us();
    h->req_len = (size_t)snprintf(h->req, sizeof(h->req),
                                  "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: caricoder3\r\nAccept: */*\r\n"
                                  "%s%s%sConnection: close\r\n\r\n",
                                  h->path, h->host, h->auth[0] ? "Authorization: Basic " : "",
                                  h->auth, h->auth[0] ? "\r\n" : "");
    h->req_sent = 0;
    h->hdr_len = 0;
    set_state(h, "connecting");
    return true;
}

/* The response head is in: status, chunked or not, a redirect. */
static bool headers_done(struct http_in *h)
{
    int code = 0;
    char *loc;

    h->hdr[h->hdr_len] = 0;
    if (sscanf(h->hdr, "HTTP/%*d.%*d %d", &code) != 1) {
        drop_conn(h, "not an HTTP answer", true);
        return false;
    }
    if (code >= 300 && code < 400 && code != 304 && (loc = strcasestr(h->hdr, "\r\nLocation:"))) {
        char target[1024], err[200];
        size_t n;

        loc += 11;
        while (*loc == ' ')
            loc++;
        n = strcspn(loc, "\r\n");
        if (n >= sizeof(target) || ++h->redirects > 5) {
            drop_conn(h, "too many redirects", true);
            return false;
        }
        memcpy(target, loc, n);
        target[n] = 0;
        if (target[0] == '/') {
            snprintf(h->path, sizeof(h->path), "%s", target);
        } else if (!parse_url(h, target, err, sizeof(err))) {
            drop_conn(h, "redirected somewhere unusable", true);
            return false;
        }
        log_info("http: redirected to %s:%d%s", h->host, h->port, h->path);
        close(h->fd);
        h->fd = -1;
        h->st = H_IDLE;
        h->next_try_us = 0;
        return false;
    }
    if (code != 200) {
        char why[48];

        snprintf(why, sizeof(why), "the server answered %d", code);
        drop_conn(h, why, true);
        return false;
    }
    {
        char *te = strcasestr(h->hdr, "\r\nTransfer-Encoding:"), line[128];

        h->chunked = false;
        if (te) {
            snprintf(line, sizeof(line), "%.*s", (int)strcspn(te + 2, "\r\n"), te + 2);
            h->chunked = strcasestr(line, "chunked") != NULL;
        }
    }
    h->chunk_left = -1;
    h->line_len = 0;
    h->chunk_crlf = false;
    h->redirects = 0;
    h->warming = true;
    h->warm_start_us = now_us();
    h->pcr_pid = -1;
    h->w_wall = 0;
    h->skipped = 0;
    h->st = H_BODY;
    h->last_data_us = now_us();
    if (h->last_why[0])
        log_info("http: %s:%d%s: connected again", h->host, h->port, h->path);
    h->last_why[0] = 0;
    set_state(h, "connected");
    return true;
}

/* Body bytes in, chunk framing taken off. */
static void body_bytes(struct http_in *h, const uint8_t *p, size_t n)
{
    while (n) {
        if (!h->chunked || h->chunk_left > 0) {
            size_t take = n;

            if (h->chunked && (int64_t)take > h->chunk_left)
                take = (size_t)h->chunk_left;
            if (take > BUF_MAX - h->len) {
                /* nobody reads fast enough: the oldest goes */
                size_t drop = take - (BUF_MAX - h->len);

                if (drop > h->len)
                    drop = h->len;
                memmove(h->buf, h->buf + drop, h->len - drop);
                h->len -= drop;
                h->errors++;
                if (take > BUF_MAX - h->len)
                    take = BUF_MAX - h->len;
            }
            memcpy(h->buf + h->len, p, take);
            h->len += take;
            p += take;
            n -= take;
            if (h->chunked) {
                h->chunk_left -= (int64_t)take;
                if (h->chunk_left == 0) {
                    h->chunk_crlf = true;
                    h->chunk_left = -1;
                }
            }
            continue;
        }
        /* a size line, or the CRLF after a chunk */
        char c = (char)*p++;

        n--;
        if (c == '\n') {
            if (h->chunk_crlf) {
                h->chunk_crlf = false;
                h->line_len = 0;
                continue;
            }
            h->line[h->line_len] = 0;
            h->line_len = 0;
            h->chunk_left = strtoll(h->line, NULL, 16);
            if (h->chunk_left == 0) {
                drop_conn(h, "the server ended the stream", false);
                return;
            }
        } else if (c != '\r' && h->line_len + 1 < (int)sizeof(h->line)) {
            h->line[h->line_len++] = c;
        }
    }
}

/* Up to 7 whole packets from the buffer, in sync. 0 when not enough yet. */
static int take_dgram_raw(struct http_in *h, uint8_t *out, int size);
static bool backlog(struct http_in *h, const uint8_t *d, int len);

/* The next datagram to hand on, the backlog skipped. */
static int take_dgram(struct http_in *h, uint8_t *out, int size)
{
    int r;

    while ((r = take_dgram_raw(h, out, size)) > 0 && backlog(h, out, r))
        ;
    return r;
}

static int take_dgram_raw(struct http_in *h, uint8_t *out, int size)
{
    size_t start = 0;
    int k = 0;

    /* sync: a 0x47 with another 188 bytes on (or at the end of what is in) */
    while (start < h->len && !(h->buf[start] == TS_SYNC &&
                               (start + TS_SIZE >= h->len || h->buf[start + TS_SIZE] == TS_SYNC)))
        start++;
    if (start) {
        memmove(h->buf, h->buf + start, h->len - start);
        h->len -= start;
    }
    while (k < 7 && (size_t)(k + 1) * TS_SIZE <= h->len && (k + 1) * TS_SIZE <= size &&
           h->buf[(size_t)k * TS_SIZE] == TS_SYNC)
        k++;
    if (k == 0)
        return 0;
    memcpy(out, h->buf, (size_t)k * TS_SIZE);
    memmove(h->buf, h->buf + (size_t)k * TS_SIZE, h->len - (size_t)k * TS_SIZE);
    h->len -= (size_t)k * TS_SIZE;
    return k * TS_SIZE;
}

/*
 * Still in the server's backlog? Looks at the PCRs of a datagram: over
 * 200 ms of clock, the PCR has to move less than 1.5 times as far, or
 * the data is coming faster than real time. Gives up waiting after 10 s
 * (and after 2 s with no PCR at all), and lets everything through.
 */
static bool backlog(struct http_in *h, const uint8_t *d, int len)
{
    int64_t now = now_us();

    if (!h->warming)
        return false;
    if (now - h->warm_start_us > 10000000 || (h->pcr_pid < 0 && now - h->warm_start_us > 2000000)) {
        h->warming = false;
        return false;
    }
    for (int i = 0; i + TS_SIZE <= len; i += TS_SIZE) {
        const uint8_t *p = d + i;
        int pid = (p[1] & 0x1F) << 8 | p[2];
        int64_t pcr;

        if (!(p[3] & 0x20) || p[4] < 7 || !(p[5] & 0x10))
            continue;
        if (h->pcr_pid < 0)
            h->pcr_pid = pid;
        if (pid != h->pcr_pid)
            continue;
        pcr = ((int64_t)p[6] << 25 | (int64_t)p[7] << 17 | (int64_t)p[8] << 9 | (int64_t)p[9] << 1 | p[10] >> 7) * 300 +
              (((p[10] & 1) << 8) | p[11]);
        if (!h->w_wall || pcr < h->w_pcr) {
            h->w_pcr = pcr;
            h->w_wall = now;
            continue;
        }
        if (now - h->w_wall >= 200000) {
            double ratio = (double)(pcr - h->w_pcr) / 27.0 / (double)(now - h->w_wall);

            if (ratio < 1.5) {
                h->warming = false;
                if (h->skipped)
                    log_info("http: %s:%d%s: skipped %.1f MB the server had buffered; live from here",
                             h->host, h->port, h->path, (double)h->skipped / 1e6);
                return false;
            }
            h->w_pcr = pcr;
            h->w_wall = now;
        }
    }
    h->skipped += (uint64_t)len;
    return true;
}

static void *http_in_open(const char *url, char *err, size_t errlen)
{
    struct http_in *h = calloc(1, sizeof(*h));

    if (!h) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    snprintf(h->url, sizeof(h->url), "%s", url);
    if (!parse_url(h, url, err, errlen)) {
        free(h);
        return NULL;
    }
    pthread_mutex_init(&h->lock, NULL);
    h->fd = -1;
    h->st = H_IDLE;
    snprintf(h->state, sizeof(h->state), "connecting");
    return h;
}

static int http_in_recv(void *ctx, uint8_t *buf, int size)
{
    struct http_in *h = ctx;
    int64_t t_end = now_us() + RECV_TIMEOUT_MS * 1000;

    for (;;) {
        int64_t now = now_us();
        int r;

        /* whole packets already in: out they go, before any more reading */
        if (h->st == H_BODY && h->len >= TS_SIZE * 7) {
            r = take_dgram(h, buf, size);
            if (r > 0)
                return r;
        }
        if (now >= t_end) {
            /* a slow trickle still goes out within the timeout */
            if (h->st == H_BODY && h->len >= TS_SIZE && (r = take_dgram(h, buf, size)) > 0)
                return r;
            return 0;
        }
        if (h->st == H_IDLE) {
            if (now < h->next_try_us) {
                int64_t w = (h->next_try_us < t_end ? h->next_try_us : t_end) - now;

                usleep((useconds_t)(w > 0 ? w : 1000));
                continue;
            }
            if (!start_connect(h))
                continue;
        }
        if (h->st == H_CONNECTING && now - h->since_us > CONNECT_TIMEOUT_MS * 1000) {
            drop_conn(h, "connection timed out", true);
            continue;
        }
        if (h->st == H_BODY && now - h->last_data_us > IDLE_TIMEOUT_US) {
            drop_conn(h, "nothing from the server for 5 s", true);
            continue;
        }

        struct pollfd pf = { h->fd, h->st == H_CONNECTING ? POLLOUT : POLLIN, 0 };
        int wait = (int)((t_end - now) / 1000);

        if (h->st == H_BODY && h->len >= TS_SIZE && wait > 20)
            wait = 20;                  /* a partial datagram waits a little, not the whole timeout */
        r = poll(&pf, 1, wait > 0 ? wait : 1);
        if (r <= 0) {
            if (h->st == H_BODY && h->len >= TS_SIZE && (r = take_dgram(h, buf, size)) > 0)
                return r;
            continue;
        }
        if (h->st == H_CONNECTING) {
            int e = 0;
            socklen_t el = sizeof(e);

            getsockopt(h->fd, SOL_SOCKET, SO_ERROR, &e, &el);
            if (e) {
                char why[48];

                snprintf(why, sizeof(why), "cannot connect: %.30s", strerror(e));
                drop_conn(h, why, true);
                continue;
            }
            ssize_t w = send(h->fd, h->req + h->req_sent, h->req_len - h->req_sent, MSG_NOSIGNAL);

            if (w < 0 && errno != EAGAIN) {
                drop_conn(h, "the request could not be sent", true);
                continue;
            }
            if (w > 0)
                h->req_sent += (size_t)w;
            if (h->req_sent == h->req_len) {
                h->st = H_HEADERS;
                set_state(h, "waiting for the answer");
            }
            continue;
        }
        if (h->st == H_HEADERS) {
            ssize_t n = recv(h->fd, h->hdr + h->hdr_len, HDR_MAX - 1 - h->hdr_len, 0);
            char *end;

            if (n <= 0) {
                if (n < 0 && errno == EAGAIN)
                    continue;
                drop_conn(h, "the server closed before answering", true);
                continue;
            }
            h->hdr_len += (size_t)n;
            h->hdr[h->hdr_len] = 0;
            if ((end = strstr(h->hdr, "\r\n\r\n"))) {
                size_t head = (size_t)(end + 4 - h->hdr), rest = h->hdr_len - head;
                uint8_t extra[HDR_MAX];

                memcpy(extra, end + 4, rest);
                h->hdr_len = head;
                if (headers_done(h) && rest) {
                    h->bytes += (int64_t)rest;
                    body_bytes(h, extra, rest);
                }
            } else if (h->hdr_len >= HDR_MAX - 1) {
                drop_conn(h, "the answer's head is too long", true);
            }
            continue;
        }
        if (h->st == H_BODY) {
            uint8_t tmp[65536];
            ssize_t n = recv(h->fd, tmp, sizeof(tmp), 0);

            if (n < 0 && errno == EAGAIN)
                continue;
            if (n <= 0) {
                drop_conn(h, n == 0 ? "the server closed the stream" : "the connection broke", n != 0);
                continue;
            }
            h->last_data_us = now;
            h->bytes += n;
            body_bytes(h, tmp, (size_t)n);
        }
    }
}

static void http_in_stats(void *ctx, struct input_stats *st)
{
    struct http_in *h = ctx;

    memset(st, 0, sizeof(*st));
    pthread_mutex_lock(&h->lock);
    snprintf(st->state, sizeof(st->state), "%s", h->st == H_BODY && h->warming ? "connected, skipping the backlog" : h->state);
    pthread_mutex_unlock(&h->lock);
    st->connected = h->st == H_BODY;
    st->reconnects = h->reconnects;
    st->errors = h->errors;
    st->bytes = h->bytes;
}

static void http_in_close(void *ctx)
{
    struct http_in *h = ctx;

    if (h->fd >= 0)
        close(h->fd);
    pthread_mutex_destroy(&h->lock);
    free(h);
}

const struct input_ops input_http_ops = {
    .name = "http", .takes = http_in_takes, .open = http_in_open, .recv = http_in_recv,
    .stats = http_in_stats, .close = http_in_close,
};
