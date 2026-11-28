/*
 * caricoder3 - an HLS playlist as a source
 * Copyright (c) 2026 CariTech Solutions
 *
 *   https://host/path/index.m3u8        (or http://; hls://, hlss:// and
 *   Flussonic's hls2://, hlss2:// are read as http and https)
 *
 * A live HLS stream is a playlist that keeps growing and the pieces
 * ("segments") it lists, a few seconds of transport stream each. A thread
 * here reads the playlist again and again, fetches each new segment, and
 * queues it; recv() hands the queue to the channel as a live source has
 * to be: at the speed the stream's own clock (its PCR) says, not at the
 * speed the pieces arrive (a segment comes all at once).
 *
 * - A master playlist (several qualities): the one with the highest
 *   BANDWIDTH is taken; "#max=3000k" after the URL takes the best one not
 *   above that, "#lowest" the smallest.
 * - It starts three segments from the end, so two to three segments are
 *   always in hand; the speed is trimmed by up to 200 ppm to keep it so
 *   (the far end's clock and this machine's are not the same).
 * - AES-128 segments are decrypted (the key is fetched from the playlist's
 *   URI). Segments must be MPEG transport stream: fragmented MP4 (CMAF)
 *   is not read, and says so. Audio kept in a playlist of its own is not
 *   merged: the log says so and the variant is taken as it is.
 * - https needs OpenSSL in the build; the server's certificate is checked
 *   ("#insecure" after the URL: not checked).
 *
 * Options go after a "#", which is never sent to the server, so they
 * cannot disturb the address's own "?token=...".
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_OPENSSL
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif

#include "cc3.h"
#include "input.h"
#include "log.h"

#define URL_MAX         2048
#define PLAYLIST_MAX    (8 << 20)
#define SEGMENT_MAX     (96 << 20)
#define TAIL            64                  /* segments of the playlist's end that are kept */
#define START_BACK      3                   /* start this many segments from the end */
#define IO_TIMEOUT_S    10
#define DGRAM_PKTS      7
#define MAX_TRIM        200e-6

struct hurl {
    bool tls;
    char host[256];
    int port;
    char path[URL_MAX];                     /* with the query */
};

struct entry {                              /* one segment of the playlist */
    int64_t seq;
    char url[URL_MAX];
    double dur;
    bool disc;
    bool enc;
    char key_url[URL_MAX];
    uint8_t iv[16];
    bool have_iv;
};

struct seg {                                /* one fetched, waiting to be played */
    struct seg *next;
    uint8_t *data;
    size_t len, pos;
    double dur;
    bool disc;
};

struct hls_in {
    char url[URL_MAX];                      /* as given, without the options */
    bool insecure, lowest;
    long max_bw;

    pthread_t th;
    _Atomic bool stop;

    pthread_mutex_t lock;                   /* the queue, the state */
    struct seg *head, *tail;
    double queued_s;
    double target_s;
    char state[48];
    char error[200];                        /* the last thing that went wrong */
    bool playing;

    _Atomic uint64_t errors, reloads;
    _Atomic int64_t bytes;

    /* the worker's */
    char media_url[URL_MAX];
    char variant[64];
    int64_t next_seq;
    bool started;
    char key_url[URL_MAX];
    uint8_t key[16];
    bool said_audio, said_fmp4;
#ifdef HAVE_OPENSSL
    SSL_CTX *tls;
#endif

    /* the pacer's (recv's thread only) */
    int pcr_pid;
    int64_t last_pcr;                       /* 27 MHz, -1: none yet */
    int64_t t_pcr_us;                       /* when that PCR's packet was due */
    int64_t since_bytes;                    /* sent since it */
    double us_per_byte;                     /* between the last PCR and the next; 0: not known */
    double trim;                            /* the speed correction in use for this stretch */
    const struct seg *ahead_seg;            /* where the next PCR is: looked up once for each stretch */
    size_t ahead_off;                       /* (SIZE_MAX: none in the rest of this segment) */
    int64_t ahead_pcr;
    bool was_empty;
};

static int64_t now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void set_state(struct hls_in *h, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_state(struct hls_in *h, const char *fmt, ...)
{
    va_list ap;

    pthread_mutex_lock(&h->lock);
    va_start(ap, fmt);
    vsnprintf(h->state, sizeof(h->state), fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&h->lock);
}

static void set_error(struct hls_in *h, const char *what)
{
    pthread_mutex_lock(&h->lock);
    snprintf(h->error, sizeof(h->error), "%.199s", what);
    pthread_mutex_unlock(&h->lock);
}

/* ---- addresses ------------------------------------------------------------- */

static const char *path_end(const char *url)
{
    const char *q = url + strcspn(url, "?#");

    return q;
}

static bool hls_takes(const char *url)
{
    const char *e;

    if (!strncmp(url, "hls://", 6) || !strncmp(url, "hlss://", 7) ||
        !strncmp(url, "hls2://", 7) || !strncmp(url, "hlss2://", 8))
        return true;
    if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8))
        return false;
    e = path_end(url);
    return (e - url > 5 && !strncasecmp(e - 5, ".m3u8", 5)) || (e - url > 4 && !strncasecmp(e - 4, ".m3u", 4));
}

static bool parse_url(const char *url, struct hurl *u, char *err, size_t errlen)
{
    const char *p, *slash, *colon;
    size_t hl;

    memset(u, 0, sizeof(*u));
    if (!strncmp(url, "https://", 8)) {
        u->tls = true;
        p = url + 8;
    } else if (!strncmp(url, "http://", 7)) {
        p = url + 7;
    } else {
        snprintf(err, errlen, "not an http or https address: %.80s", url);
        return false;
    }
    slash = p + strcspn(p, "/?");
    colon = memchr(p, ':', (size_t)(slash - p));
    hl = (size_t)((colon ? colon : slash) - p);
    if (hl == 0 || hl >= sizeof(u->host)) {
        snprintf(err, errlen, "no server name in %.80s", url);
        return false;
    }
    memcpy(u->host, p, hl);
    u->port = colon ? atoi(colon + 1) : u->tls ? 443 : 80;
    if (u->port <= 0 || u->port > 65535) {
        snprintf(err, errlen, "bad port in %.80s", url);
        return false;
    }
    snprintf(u->path, sizeof(u->path), "%s%s", *slash == '/' ? "" : "/", slash);
    return true;
}

/* `ref` as found in the playlist at `base`, made a whole address. */
static void resolve(const char *base, const char *ref, char *out, size_t len)
{
    const char *scheme_end = strstr(base, "://");
    const char *host, *host_end, *dir_end;

    if (!strncmp(ref, "http://", 7) || !strncmp(ref, "https://", 8) || !scheme_end) {
        snprintf(out, len, "%.*s", (int)len - 1, ref);
        return;
    }
    host = scheme_end + 3;
    host_end = host + strcspn(host, "/?#");
    dir_end = path_end(base);
    while (dir_end > host_end && dir_end[-1] != '/')
        dir_end--;
    if (!strncmp(ref, "//", 2))
        snprintf(out, len, "%.*s%s", (int)(scheme_end - base + 1), base, ref);
    else if (ref[0] == '/')
        snprintf(out, len, "%.*s%s", (int)(host_end - base), base, ref);
    else if (dir_end <= host_end)
        snprintf(out, len, "%.*s/%s", (int)(host_end - base), base, ref);
    else
        snprintf(out, len, "%.*s%s", (int)(dir_end - base), base, ref);
    /* "a/../" and "./" taken out of the path */
    for (;;) {
        char *path = strstr(out, "://");
        char *d;

        path = path ? strchr(path + 3, '/') : NULL;
        if (!path)
            break;
        if ((d = strstr(path, "/./")) && d < path + strcspn(path, "?")) {
            memmove(d, d + 2, strlen(d + 2) + 1);
            continue;
        }
        if ((d = strstr(path, "/../")) && d < path + strcspn(path, "?") && d > path) {
            char *up = d - 1;

            while (up > path && *up != '/')
                up--;
            memmove(up, d + 3, strlen(d + 3) + 1);
            continue;
        }
        break;
    }
}

/* ---- one GET --------------------------------------------------------------- */

struct conn {
    int fd;
#ifdef HAVE_OPENSSL
    SSL *ssl;
#endif
};

static void conn_close(struct conn *c)
{
#ifdef HAVE_OPENSSL
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
        c->ssl = NULL;
    }
#endif
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
}

static bool conn_open(struct hls_in *h, const struct hurl *u, struct conn *c, char *err, size_t errlen)
{
    struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *res = NULL, *ai;
    char port[8];
    struct timeval tv = { IO_TIMEOUT_S, 0 };
    int one = 1, rc;

    memset(c, 0, sizeof(*c));
    c->fd = -1;
    snprintf(port, sizeof(port), "%d", u->port);
    rc = getaddrinfo(u->host, port, &hints, &res);
    if (rc != 0) {
        snprintf(err, errlen, "%.100s: %s", u->host, gai_strerror(rc));
        return false;
    }
    for (ai = res; ai && c->fd < 0; ai = ai->ai_next) {
        struct pollfd pf;
        int fd = socket(ai->ai_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        int e = 0;
        socklen_t el = sizeof(e);

        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0 && errno != EINPROGRESS) {
            close(fd);
            continue;
        }
        pf.fd = fd;
        pf.events = POLLOUT;
        if (poll(&pf, 1, IO_TIMEOUT_S * 1000) <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el) < 0 || e) {
            close(fd);
            continue;
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
        c->fd = fd;
    }
    freeaddrinfo(res);
    if (c->fd < 0) {
        snprintf(err, errlen, "%.100s:%d does not answer", u->host, u->port);
        return false;
    }
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (!u->tls)
        return true;
#ifdef HAVE_OPENSSL
    if (!h->tls) {
        h->tls = SSL_CTX_new(TLS_client_method());
        if (h->tls)
            SSL_CTX_set_default_verify_paths(h->tls);
    }
    c->ssl = h->tls ? SSL_new(h->tls) : NULL;
    if (!c->ssl) {
        snprintf(err, errlen, "TLS could not be set up");
        conn_close(c);
        return false;
    }
    SSL_set_fd(c->ssl, c->fd);
    SSL_set_tlsext_host_name(c->ssl, u->host);
    if (!h->insecure) {
        SSL_set1_host(c->ssl, u->host);
        SSL_set_verify(c->ssl, SSL_VERIFY_PEER, NULL);
    }
    if (SSL_connect(c->ssl) != 1) {
        long v = SSL_get_verify_result(c->ssl);

        if (v != X509_V_OK)
            snprintf(err, errlen, "%.100s: its certificate is not accepted (%s); \"#insecure\" after the address skips the check",
                     u->host, X509_verify_cert_error_string(v));
        else
            snprintf(err, errlen, "%.100s: the TLS handshake failed", u->host);
        ERR_clear_error();
        conn_close(c);
        return false;
    }
    return true;
#else
    (void)h;
    snprintf(err, errlen, "https is not in this build (OpenSSL was not found when it was built)");
    conn_close(c);
    return false;
#endif
}

static ssize_t conn_read(struct conn *c, void *buf, size_t len)
{
#ifdef HAVE_OPENSSL
    if (c->ssl) {
        int r = SSL_read(c->ssl, buf, (int)(len > 1 << 20 ? 1 << 20 : len));

        if (r > 0)
            return r;
        return SSL_get_error(c->ssl, r) == SSL_ERROR_ZERO_RETURN ? 0 : -1;
    }
#endif
    return recv(c->fd, buf, len, 0);
}

static bool conn_write(struct conn *c, const char *d, size_t len)
{
    while (len) {
        ssize_t w;

#ifdef HAVE_OPENSSL
        if (c->ssl)
            w = SSL_write(c->ssl, d, (int)len);
        else
#endif
            w = send(c->fd, d, len, MSG_NOSIGNAL);
        if (w <= 0)
            return false;
        d += w;
        len -= (size_t)w;
    }
    return true;
}

/* The body of `url` (redirects followed) in *out, malloc'd; `final` gets
 * the address it was at last (what relative addresses in it are relative to). */
static bool http_get(struct hls_in *h, const char *url, size_t cap, uint8_t **out, size_t *out_len,
                     char *final, size_t final_len, char *err, size_t errlen)
{
    char cur[URL_MAX];

    snprintf(cur, sizeof(cur), "%s", url);
    for (int hop = 0; hop < 6; hop++) {
        struct hurl u;
        struct conn c;
        char req[URL_MAX + 512], head[16384], *loc, *cl, *body_at;
        size_t hl = 0, len = 0, room = 0;
        uint8_t *body = NULL;
        long want = -1;
        int status;
        bool chunked, ok = true;
        int64_t deadline = now_us() + (int64_t)(cap > PLAYLIST_MAX ? 60 : 20) * 1000000;

        if (!parse_url(cur, &u, err, errlen) || !conn_open(h, &u, &c, err, errlen))
            return false;
        snprintf(req, sizeof(req),
                 "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Mozilla/5.0 (X11; Linux x86_64) caricoder3\r\n"
                 "Accept: */*\r\nConnection: close\r\n\r\n", u.path, u.host);
        if (!conn_write(&c, req, strlen(req))) {
            snprintf(err, errlen, "%.100s: the request could not be sent", u.host);
            conn_close(&c);
            return false;
        }
        head[0] = 0;
        while (!(body_at = strstr(head, "\r\n\r\n"))) {
            ssize_t r = hl < sizeof(head) - 1 ? conn_read(&c, head + hl, sizeof(head) - 1 - hl) : -1;

            if (r <= 0 || atomic_load(&h->stop)) {
                snprintf(err, errlen, "%.100s: no answer to the request", u.host);
                conn_close(&c);
                return false;
            }
            hl += (size_t)r;
            head[hl] = 0;
        }
        *body_at = 0;
        body_at += 4;
        status = strncmp(head, "HTTP/1.", 7) ? 0 : atoi(head + 9);
        loc = strcasestr(head, "\r\nLocation:");
        if (status >= 301 && status <= 308 && loc) {
            char ref[URL_MAX], next[URL_MAX];

            loc += 11;
            while (*loc == ' ')
                loc++;
            snprintf(ref, sizeof(ref), "%.*s", (int)strcspn(loc, "\r\n"), loc);
            resolve(cur, ref, next, sizeof(next));
            snprintf(cur, sizeof(cur), "%s", next);
            conn_close(&c);
            continue;
        }
        if (status != 200) {
            snprintf(err, errlen, "%.100s answers %d to %.60s", u.host, status, u.path);
            conn_close(&c);
            return false;
        }
        chunked = strcasestr(head, "\r\nTransfer-Encoding: chunked") != NULL;
        cl = strcasestr(head, "\r\nContent-Length:");
        if (cl && !chunked)
            want = atol(cl + 17);
        if (want > (long)cap) {
            snprintf(err, errlen, "%.60s is %ld bytes: more than is taken", u.path, want);
            conn_close(&c);
            return false;
        }
        len = hl - (size_t)(body_at - head);
        room = want > 0 ? (size_t)want + 1 : len + 65536;
        body = malloc(room);
        if (body)
            memcpy(body, body_at, len);
        while (body && (want < 0 || len < (size_t)want)) {
            ssize_t r;

            if (len + 32768 > room) {
                uint8_t *nb = room * 2 <= cap + 65536 ? realloc(body, room * 2) : NULL;

                if (!nb) {
                    snprintf(err, errlen, "%.60s: more than %zu MB", u.path, cap >> 20);
                    ok = false;
                    break;
                }
                body = nb;
                room *= 2;
            }
            r = conn_read(&c, body + len, room - len);
            if (r == 0)
                break;
            if (r < 0 || atomic_load(&h->stop) || now_us() > deadline) {
                snprintf(err, errlen, "%.60s: stopped coming after %zu bytes", u.path, len);
                ok = false;
                break;
            }
            len += (size_t)r;
        }
        conn_close(&c);
        if (!body) {
            snprintf(err, errlen, "out of memory");
            return false;
        }
        if (ok && want > 0 && len < (size_t)want) {
            snprintf(err, errlen, "%.60s: %zu of %ld bytes came", u.path, len, want);
            ok = false;
        }
        if (ok && chunked) {
            /* everything is in (the connection closed): the chunk sizes are taken out */
            size_t rd = 0, wr = 0;

            while (rd < len) {
                char *end;
                unsigned long n = strtoul((char *)body + rd, &end, 16);
                size_t at = (size_t)((uint8_t *)end - body);

                while (at < len && body[at] != '\n')
                    at++;
                at++;
                if (n == 0 || at + n > len)
                    break;
                memmove(body + wr, body + at, n);
                wr += n;
                rd = at + n + 2;
            }
            len = wr;
        }
        if (!ok) {
            free(body);
            return false;
        }
        *out = body;
        *out_len = len;
        if (final)
            snprintf(final, final_len, "%s", cur);
        return true;
    }
    snprintf(err, errlen, "too many redirects from %.80s", url);
    return false;
}

/* ---- the playlists --------------------------------------------------------- */

/* The value of NAME= in an attribute list, quotes removed. */
static bool attr(const char *list, const char *name, char *out, size_t len)
{
    size_t nl = strlen(name);
    const char *p = list;

    while ((p = strstr(p, name)) != NULL) {
        if ((p == list || p[-1] == ',' || p[-1] == ':') && p[nl] == '=') {
            p += nl + 1;
            if (*p == '"')
                snprintf(out, len, "%.*s", (int)strcspn(p + 1, "\""), p + 1);
            else
                snprintf(out, len, "%.*s", (int)strcspn(p, ",\r\n"), p);
            return true;
        }
        p += nl;
    }
    return false;
}

/* A master playlist: the media playlist to follow, in h->media_url.
 * false when `text` is not a master playlist. */
static bool pick_variant(struct hls_in *h, const char *base, char *text)
{
    long best_bw = -1, low_bw = -1;
    char best[URL_MAX] = "", best_what[64] = "", low[URL_MAX] = "", low_what[64] = "";
    bool separate_audio = strstr(text, "TYPE=AUDIO") && strstr(text, "URI=");
    char *save = NULL;
    long pending = -1;
    char pending_what[64] = "";

    if (!strstr(text, "#EXT-X-STREAM-INF"))
        return false;
    for (char *ln = strtok_r(text, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
        ln[strcspn(ln, "\r")] = 0;
        if (!strncmp(ln, "#EXT-X-STREAM-INF:", 18)) {
            char v[64], res[32] = "";

            pending = attr(ln + 18, "BANDWIDTH", v, sizeof(v)) ? atol(v) : 0;
            attr(ln + 18, "RESOLUTION", res, sizeof(res));
            snprintf(pending_what, sizeof(pending_what), "%s%s%.1f Mb/s", res, res[0] ? ", " : "", (double)pending / 1e6);
        } else if (ln[0] && ln[0] != '#' && pending >= 0) {
            /* the highest (not above "max" when one is given), and the lowest of all */
            if ((!h->max_bw || pending <= h->max_bw) && pending > best_bw) {
                best_bw = pending;
                resolve(base, ln, best, sizeof(best));
                snprintf(best_what, sizeof(best_what), "%s", pending_what);
            }
            if (low_bw < 0 || pending < low_bw) {
                low_bw = pending;
                resolve(base, ln, low, sizeof(low));
                snprintf(low_what, sizeof(low_what), "%s", pending_what);
            }
            pending = -1;
        }
    }
    if (h->lowest || !best[0]) {            /* asked for, or every one is above "max" */
        snprintf(best, sizeof(best), "%s", low);
        snprintf(best_what, sizeof(best_what), "%s", low_what);
    }
    if (!best[0])
        return false;
    if (strcmp(best, h->media_url)) {
        log_info("hls: of the qualities offered, taking %s", best_what);
        if (separate_audio && !h->said_audio) {
            h->said_audio = true;
            log_warn("hls: this stream keeps its audio in a playlist of its own, which is not merged here: "
                     "the channel may have no sound from it");
        }
    }
    snprintf(h->media_url, sizeof(h->media_url), "%s", best);
    snprintf(h->variant, sizeof(h->variant), "%s", best_what);
    return true;
}

struct media {
    double target;
    bool ended, fmp4, byterange;
    int n;
    struct entry e[TAIL];
};

static void parse_media(const char *base, char *text, struct media *m)
{
    char *save = NULL;
    int64_t seq = 0;
    double dur = 0;
    bool disc = false, enc = false, have_iv = false;
    char key_url[URL_MAX] = "";
    uint8_t iv[16] = { 0 };

    memset(m, 0, sizeof(*m));
    m->target = 6;
    for (char *ln = strtok_r(text, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
        ln[strcspn(ln, "\r")] = 0;
        if (!strncmp(ln, "#EXT-X-TARGETDURATION:", 22)) {
            m->target = atof(ln + 22) > 0 ? atof(ln + 22) : 6;
        } else if (!strncmp(ln, "#EXT-X-MEDIA-SEQUENCE:", 22)) {
            seq = atoll(ln + 22);
        } else if (!strncmp(ln, "#EXTINF:", 8)) {
            dur = atof(ln + 8);
        } else if (!strncmp(ln, "#EXT-X-DISCONTINUITY", 20) && strncmp(ln, "#EXT-X-DISCONTINUITY-SEQUENCE", 29)) {
            disc = true;
        } else if (!strncmp(ln, "#EXT-X-ENDLIST", 14)) {
            m->ended = true;
        } else if (!strncmp(ln, "#EXT-X-MAP:", 11)) {
            m->fmp4 = true;
        } else if (!strncmp(ln, "#EXT-X-BYTERANGE:", 17)) {
            m->byterange = true;
        } else if (!strncmp(ln, "#EXT-X-KEY:", 11)) {
            char method[32] = "", v[URL_MAX];

            attr(ln + 11, "METHOD", method, sizeof(method));
            enc = !strcmp(method, "AES-128");
            have_iv = false;
            key_url[0] = 0;
            if (enc && attr(ln + 11, "URI", v, sizeof(v)))
                resolve(base, v, key_url, sizeof(key_url));
            if (enc && attr(ln + 11, "IV", v, sizeof(v)) && (v[0] == '0') && (v[1] == 'x' || v[1] == 'X') &&
                strlen(v) >= 34) {
                for (int i = 0; i < 16; i++) {
                    char b[3] = { v[2 + i * 2], v[3 + i * 2], 0 };

                    iv[i] = (uint8_t)strtoul(b, NULL, 16);
                }
                have_iv = true;
            }
            if (strcmp(method, "NONE") && !enc)
                m->fmp4 = true;             /* SAMPLE-AES and the like: not read either */
        } else if (ln[0] && ln[0] != '#') {
            struct entry *e;

            if (m->n == TAIL) {
                memmove(&m->e[0], &m->e[1], sizeof(m->e[0]) * (TAIL - 1));
                m->n--;
            }
            e = &m->e[m->n++];
            memset(e, 0, sizeof(*e));
            e->seq = seq++;
            resolve(base, ln, e->url, sizeof(e->url));
            e->dur = dur > 0 ? dur : m->target;
            e->disc = disc;
            e->enc = enc;
            snprintf(e->key_url, sizeof(e->key_url), "%s", key_url);
            memcpy(e->iv, iv, 16);
            e->have_iv = have_iv;
            disc = false;
            dur = 0;
        }
    }
}

/* ---- the worker: playlist, segments, the queue ------------------------------ */

static bool nap(struct hls_in *h, double seconds)
{
    int64_t until = now_us() + (int64_t)(seconds * 1e6);

    while (now_us() < until && !atomic_load(&h->stop))
        usleep(50000);
    return !atomic_load(&h->stop);
}

static bool decrypt(struct hls_in *h, const struct entry *e, uint8_t *data, size_t *len, char *err, size_t errlen)
{
#ifdef HAVE_OPENSSL
    EVP_CIPHER_CTX *c;
    uint8_t iv[16], *out;
    int n1 = 0, n2 = 0;
    bool ok;

    if (strcmp(e->key_url, h->key_url)) {
        uint8_t *k = NULL;
        size_t kl = 0;

        if (!http_get(h, e->key_url, 4096, &k, &kl, NULL, 0, err, errlen))
            return false;
        if (kl != 16) {
            snprintf(err, errlen, "the key is %zu bytes, not 16", kl);
            free(k);
            return false;
        }
        memcpy(h->key, k, 16);
        free(k);
        snprintf(h->key_url, sizeof(h->key_url), "%s", e->key_url);
    }
    if (e->have_iv) {
        memcpy(iv, e->iv, 16);
    } else {
        memset(iv, 0, 16);
        for (int i = 0; i < 8; i++)
            iv[15 - i] = (uint8_t)((uint64_t)e->seq >> (8 * i));
    }
    out = malloc(*len + 16);
    c = EVP_CIPHER_CTX_new();
    ok = out && c && EVP_DecryptInit_ex(c, EVP_aes_128_cbc(), NULL, h->key, iv) == 1 &&
         EVP_DecryptUpdate(c, out, &n1, data, (int)*len) == 1 && EVP_DecryptFinal_ex(c, out + n1, &n2) == 1;
    if (c)
        EVP_CIPHER_CTX_free(c);
    if (ok) {
        memcpy(data, out, (size_t)(n1 + n2));
        *len = (size_t)(n1 + n2);
    } else {
        snprintf(err, errlen, "the segment does not decrypt with the playlist's key");
        ERR_clear_error();
    }
    free(out);
    return ok;
#else
    (void)h; (void)e; (void)data; (void)len;
    snprintf(err, errlen, "the segments are encrypted and this build has no OpenSSL");
    return false;
#endif
}

static void enqueue(struct hls_in *h, uint8_t *data, size_t len, double dur, bool disc)
{
    struct seg *s = calloc(1, sizeof(*s));
    size_t off = 0;

    if (!s) {
        free(data);
        return;
    }
    /* whole packets only, from the first sync byte */
    while (off + TS_SIZE <= len && !(data[off] == TS_SYNC && (off + TS_SIZE >= len || data[off + TS_SIZE] == TS_SYNC)))
        off++;
    s->data = data;
    s->pos = off;
    s->len = off + (len - off) / TS_SIZE * TS_SIZE;
    s->dur = dur;
    s->disc = disc;
    pthread_mutex_lock(&h->lock);
    if (h->tail)
        h->tail->next = s;
    else
        h->head = s;
    h->tail = s;
    h->queued_s += dur;
    /* far more in hand than is wanted (this machine's clock is slow against
     * the far end's, or it stood still): the oldest go */
    while (h->queued_s > 6 * h->target_s + 10 && h->head->next && h->head->next != h->tail) {
        struct seg *old = h->head->next;    /* not the head: recv() may be in the middle of it */

        h->head->next = old->next;
        h->queued_s -= old->dur;
        atomic_fetch_add(&h->errors, 1);
        log_warn("hls: %.0f s in hand, more than is wanted: a segment (%.1f s) is passed over", h->queued_s + old->dur, old->dur);
        free(old->data);
        free(old);
    }
    pthread_mutex_unlock(&h->lock);
}

static void *worker(void *arg)
{
    struct hls_in *h = arg;
    char err[256], base[URL_MAX];
    int fails = 0;

    prctl(PR_SET_NAME, "cc3-hls-in");
    snprintf(h->media_url, sizeof(h->media_url), "%s", h->url);
    set_state(h, "reading the playlist");
    while (!atomic_load(&h->stop)) {
        uint8_t *text = NULL;
        size_t tl = 0;
        struct media *m;
        int got = 0;
        double queued;

        if (!http_get(h, h->media_url, PLAYLIST_MAX, &text, &tl, base, sizeof(base), err, sizeof(err))) {
            if (fails++ == 0 || fails % 20 == 0)
                log_warn("hls: the playlist: %s", err);
            set_error(h, err);
            atomic_fetch_add(&h->reloads, 1);
            set_state(h, "the playlist does not come");
            if (fails > 5)                  /* a variant's address can go stale: from the top again */
                snprintf(h->media_url, sizeof(h->media_url), "%s", h->url);
            nap(h, fails < 5 ? 1.0 : 5.0);
            continue;
        }
        {
            uint8_t *t2 = realloc(text, tl + 1);

            if (!t2) {
                free(text);
                nap(h, 1.0);
                continue;
            }
            text = t2;
            text[tl] = 0;
        }
        if (strncmp((char *)text, "#EXTM3U", 7)) {
            if (fails++ == 0)
                log_warn("hls: %.80s is not a playlist (it does not begin with #EXTM3U)", h->media_url);
            set_state(h, "not a playlist");
            set_error(h, "what the address gives is not a playlist (it does not begin with #EXTM3U)");
            free(text);
            nap(h, 5.0);
            continue;
        }
        if (pick_variant(h, base, (char *)text)) {      /* a master playlist: its variant next */
            free(text);
            nap(h, 0.1);
            continue;
        }
        m = malloc(sizeof(*m));
        if (!m) {
            free(text);
            nap(h, 1.0);
            continue;
        }
        parse_media(base, (char *)text, m);
        free(text);
        pthread_mutex_lock(&h->lock);
        h->target_s = m->target;
        pthread_mutex_unlock(&h->lock);
        if (m->fmp4 || m->byterange) {
            if (!h->said_fmp4) {
                h->said_fmp4 = true;
                log_err("hls: this stream's segments are %s, not plain transport stream files: it cannot be read here",
                        m->fmp4 ? "fragmented MP4 (or encrypted by sample)" : "byte ranges of one file");
            }
            set_state(h, "segments are not transport stream");
            set_error(h, m->fmp4 ? "its segments are fragmented MP4 (or encrypted by sample), not transport stream files: not read here"
                                 : "its segments are byte ranges of one file: not read here");
            free(m);
            nap(h, 30.0);
            continue;
        }
        fails = 0;
        if (m->n && !h->started) {
            h->next_seq = m->e[m->n > START_BACK ? m->n - START_BACK : 0].seq;
            h->started = true;
        } else if (m->n && (h->next_seq < m->e[0].seq || h->next_seq > m->e[m->n - 1].seq + 1)) {
            /* the playlist has moved on past us, or begun again */
            log_warn("hls: the playlist is at segments %lld to %lld, the next one wanted was %lld: carrying on from its end",
                     (long long)m->e[0].seq, (long long)m->e[m->n - 1].seq, (long long)h->next_seq);
            atomic_fetch_add(&h->errors, 1);
            h->next_seq = m->e[m->n > START_BACK ? m->n - START_BACK : 0].seq;
        }
        for (int i = 0; i < m->n && !atomic_load(&h->stop); i++) {
            const struct entry *e = &m->e[i];
            uint8_t *data = NULL;
            size_t len = 0;
            bool ok = false;

            if (e->seq < h->next_seq)
                continue;
            for (int attempt = 0; attempt < 3 && !ok && !atomic_load(&h->stop); attempt++) {
                ok = http_get(h, e->url, SEGMENT_MAX, &data, &len, NULL, 0, err, sizeof(err));
                if (ok && e->enc && !decrypt(h, e, data, &len, err, sizeof(err))) {
                    free(data);
                    data = NULL;
                    ok = false;
                }
                if (!ok)
                    nap(h, 0.5);
            }
            h->next_seq = e->seq + 1;
            if (!ok) {
                log_warn("hls: segment %lld: %s: passed over", (long long)e->seq, err);
                set_error(h, err);
                atomic_fetch_add(&h->errors, 1);
                continue;
            }
            atomic_fetch_add(&h->bytes, (int64_t)len);
            enqueue(h, data, len, e->dur, e->disc);
            got++;
        }
        pthread_mutex_lock(&h->lock);
        queued = h->queued_s;
        h->playing = h->head != NULL;
        snprintf(h->state, sizeof(h->state), "playing%s%.22s, %.0f s in hand", h->variant[0] ? " " : "", h->variant, queued);
        pthread_mutex_unlock(&h->lock);
        if (m->ended && !got) {
            set_state(h, "the stream has ended");
            free(m);
            nap(h, 5.0);
            h->started = false;             /* a new one may begin under the same address */
            continue;
        }
        nap(h, got ? m->target / 2 : (m->target / 4 > 0.5 ? m->target / 4 : 0.5));
        free(m);
    }
    return NULL;
}

/* ---- open, recv (the pacer), stats, close ---------------------------------- */

static bool pcr_of(const struct hls_in *h, const uint8_t *p, int64_t *pcr, int *pid)
{
    if (p[0] != TS_SYNC || !(p[3] & 0x20) || p[4] < 7 || !(p[5] & 0x10))
        return false;
    *pid = (p[1] & 0x1F) << 8 | p[2];
    if (h->pcr_pid >= 0 && *pid != h->pcr_pid)
        return false;
    *pcr = ((int64_t)p[6] << 25 | (int64_t)p[7] << 17 | (int64_t)p[8] << 9 | (int64_t)p[9] << 1 | p[10] >> 7) * 300 +
           (((p[10] & 1) << 8) | p[11]);
    return true;
}

static void *hls_open(const char *url, char *err, size_t errlen)
{
    struct hls_in *h = calloc(1, sizeof(*h));
    char clean[URL_MAX];
    const char *rest, *frag;
    struct hurl u;

    if (!h) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    if (!strncmp(url, "hlss2://", 8) || !strncmp(url, "hlss://", 7)) {
        rest = strstr(url, "://") + 3;
        snprintf(clean, sizeof(clean), "https://%s", rest);
    } else if (!strncmp(url, "hls2://", 7) || !strncmp(url, "hls://", 6)) {
        rest = strstr(url, "://") + 3;
        snprintf(clean, sizeof(clean), "http://%s", rest);
    } else {
        snprintf(clean, sizeof(clean), "%s", url);
    }
    frag = strchr(clean, '#');
    if (frag) {
        char *save = NULL, opts[256];

        snprintf(opts, sizeof(opts), "%s", frag + 1);
        clean[frag - clean] = 0;
        for (char *o = strtok_r(opts, "&,", &save); o; o = strtok_r(NULL, "&,", &save)) {
            if (!strcmp(o, "insecure")) {
                h->insecure = true;
            } else if (!strcmp(o, "lowest")) {
                h->lowest = true;
            } else if (!strncmp(o, "max=", 4)) {
                char *end;
                double v = strtod(o + 4, &end);

                h->max_bw = (long)(v * (*end == 'k' || *end == 'K' ? 1e3 : *end == 'm' || *end == 'M' ? 1e6 : 1));
            } else {
                snprintf(err, errlen, "'%s' after the # is not an option here (insecure, lowest, max=3000k)", o);
                free(h);
                return NULL;
            }
        }
    }
    if (!parse_url(clean, &u, err, errlen)) {
        free(h);
        return NULL;
    }
#ifndef HAVE_OPENSSL
    if (u.tls) {
        snprintf(err, errlen, "https is not in this build (OpenSSL was not found when it was built)");
        free(h);
        return NULL;
    }
#endif
    snprintf(h->url, sizeof(h->url), "%s", clean);
    pthread_mutex_init(&h->lock, NULL);
    h->target_s = 6;
    h->pcr_pid = -1;
    h->last_pcr = -1;
    snprintf(h->state, sizeof(h->state), "reading the playlist");
    if (pthread_create(&h->th, NULL, worker, h) != 0) {
        snprintf(err, errlen, "no thread for the HLS source");
        pthread_mutex_destroy(&h->lock);
        free(h);
        return NULL;
    }
    return h;
}

static int hls_recv(void *ctx, uint8_t *buf, int size)
{
    struct hls_in *h = ctx;
    struct seg *s;
    int64_t now, due;
    int n = 0, max = size / TS_SIZE < DGRAM_PKTS ? size / TS_SIZE : DGRAM_PKTS;
    double queued, target;

    pthread_mutex_lock(&h->lock);
    while (h->head && h->head->pos + TS_SIZE > h->head->len) {
        struct seg *done = h->head;

        h->head = done->next;
        if (!h->head)
            h->tail = NULL;
        h->queued_s -= done->dur;
        free(done->data);
        free(done);
    }
    s = h->head;
    queued = h->queued_s;
    target = h->target_s;
    pthread_mutex_unlock(&h->lock);
    if (!s) {
        h->was_empty = true;
        usleep(20000);
        return 0;
    }
    now = now_us();
    if (s->disc) {                          /* the stream's clock starts again here */
        if (h->last_pcr >= 0)
            h->t_pcr_us += (int64_t)((double)(h->since_bytes + TS_SIZE) * h->us_per_byte);
        h->since_bytes = -TS_SIZE;
        h->last_pcr = -2;                   /* the next PCR is a new beginning, at t_pcr_us */
        s->disc = false;
    }
    /* Where the next PCR is, and so how fast the bytes up to it go: every
     * PCR then leaves exactly when its own value says, whatever the sizes of
     * the pictures in between. */
    if (h->last_pcr >= 0 && (h->ahead_seg != s || (h->ahead_off != SIZE_MAX && h->ahead_off < s->pos))) {
        h->ahead_seg = s;
        h->ahead_off = SIZE_MAX;
        for (size_t o = s->pos; o + TS_SIZE <= s->len; o += TS_SIZE) {
            int pid;

            if (pcr_of(h, s->data + o, &h->ahead_pcr, &pid)) {
                int64_t d = h->ahead_pcr - h->last_pcr;

                h->ahead_off = o;
                if (d > 0 && d < 27000000) {
                    /* a little faster with too much in hand, a little slower with too little */
                    double trim = (queued - 2.5 * target) / (target > 0 ? target : 6) * 100e-6;

                    h->trim = trim > MAX_TRIM ? MAX_TRIM : trim < -MAX_TRIM ? -MAX_TRIM : trim;
                    h->us_per_byte = (double)d / 27.0 * (1.0 - h->trim) /
                                     (double)(h->since_bytes + TS_SIZE + (int64_t)(o - s->pos));
                }
                break;
            }
        }
    }
    due = h->last_pcr == -1 ? now : h->t_pcr_us + (int64_t)((double)(h->since_bytes + TS_SIZE) * h->us_per_byte);
    if (h->was_empty || now - due > 1000000) {
        /* nothing was in hand for a while (or this is the start): from now, not in a rush to catch up */
        h->t_pcr_us += now - due;
        due = now;
        h->was_empty = false;
    }
    if (due > now) {
        int64_t wait = due - now;

        usleep((useconds_t)(wait > 200000 ? 200000 : wait));
        if (wait > 200000)
            return 0;
    }
    /* (the segment's bytes are this thread's alone once it is at the head) */
    while (n < max && s->pos + TS_SIZE <= s->len) {
        const uint8_t *p = s->data + s->pos;
        int64_t pcr;
        int pid;
        bool is_pcr = pcr_of(h, p, &pcr, &pid);

        if (is_pcr && n > 0)
            break;                          /* a PCR leads its datagram, so that it leaves on its own time */
        memcpy(buf + n * TS_SIZE, p, TS_SIZE);
        s->pos += TS_SIZE;
        n++;
        if (is_pcr) {
            int64_t d = h->last_pcr >= 0 ? pcr - h->last_pcr : -1;
            int64_t here = h->t_pcr_us + (int64_t)((double)(h->since_bytes + TS_SIZE) * h->us_per_byte);

            h->pcr_pid = pid;
            if (h->last_pcr == -1)
                h->t_pcr_us = now;          /* the very first */
            else if (d > 0 && d < 27000000)
                h->t_pcr_us += (int64_t)((double)d / 27.0 * (1.0 - h->trim));
            else
                h->t_pcr_us = here;         /* the clock jumped: time carries on from where the bytes got to */
            h->last_pcr = pcr;
            h->since_bytes = -TS_SIZE;      /* (this packet is added just below) */
            h->ahead_seg = NULL;            /* the next stretch: look ahead again */
        }
        h->since_bytes += TS_SIZE;
    }
    return n * TS_SIZE;
}

static void hls_stats(void *ctx, struct input_stats *st)
{
    struct hls_in *h = ctx;

    memset(st, 0, sizeof(*st));
    pthread_mutex_lock(&h->lock);
    snprintf(st->state, sizeof(st->state), "%s", h->state);
    snprintf(st->error, sizeof(st->error), "%s", h->error);
    st->connected = h->playing;
    pthread_mutex_unlock(&h->lock);
    st->reconnects = atomic_load(&h->reloads);
    st->errors = atomic_load(&h->errors);
    st->bytes = atomic_load(&h->bytes);
}

static void hls_close(void *ctx)
{
    struct hls_in *h = ctx;

    atomic_store(&h->stop, true);
    pthread_join(h->th, NULL);
    while (h->head) {
        struct seg *s = h->head;

        h->head = s->next;
        free(s->data);
        free(s);
    }
#ifdef HAVE_OPENSSL
    if (h->tls)
        SSL_CTX_free(h->tls);
#endif
    pthread_mutex_destroy(&h->lock);
    free(h);
}

const struct input_ops input_hls_ops = {
    .name = "hls", .takes = hls_takes, .open = hls_open, .recv = hls_recv,
    .stats = hls_stats, .close = hls_close,
};
