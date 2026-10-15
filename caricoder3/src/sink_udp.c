/*
 * caricoder3 - output to UDP (unicast or multicast, with or without RTP)
 * and to a file
 * Copyright (c) 2026 CariTech Solutions
 *
 * Packets are packed seven to a datagram (or as the URL says) and sent
 * with sendmmsg in batches; a part-built datagram goes when told to
 * flush. A file is written through stdio with a large buffer.
 */
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "log.h"
#include "sink.h"
#include "udp.h"

#define SINK_DGRAMS     640
#define DGRAM_MAX       (7 * TS_SIZE)

struct udp_sink {
    FILE *fp;                   /* file output, else NULL */
    int fd;                     /* UDP output */
    int pkt;                    /* TS packets per datagram */
    uint8_t (*dg)[DGRAM_MAX];
    struct mmsghdr *mm;
    struct iovec *iov;
    int n_full, fill;
    uint64_t datagrams;
    _Atomic uint64_t packets, bytes, errors;   /* read by the statistics thread */
};

static bool udp_takes(const char *url)
{
    /* Anything that is not another scheme is a file path. */
    if (!strncmp(url, "udp://", 6) || !strncmp(url, "rtp://", 6) ||
        !strncmp(url, "file://", 7))
        return true;
    return !strstr(url, "://");
}

static const char *file_path(const char *s)
{
    return strncmp(s, "file://", 7) == 0 ? s + 7 : s;
}

static void flush(struct udp_sink *s, bool partial)
{
    int total = s->n_full + ((partial && s->fill) ? 1 : 0);
    int i = 0;

    if (s->fp || total == 0)
        return;
    for (int k = 0; k < total; k++) {
        s->iov[k].iov_base = s->dg[k];
        s->iov[k].iov_len = (size_t)(k < s->n_full ? s->pkt : s->fill) * TS_SIZE;
        memset(&s->mm[k], 0, sizeof(s->mm[k]));
        s->mm[k].msg_hdr.msg_iov = &s->iov[k];
        s->mm[k].msg_hdr.msg_iovlen = 1;
    }
    while (i < total) {
        int rc = sendmmsg(s->fd, s->mm + i, (unsigned)(total - i), 0);

        if (rc > 0) {
            s->datagrams += (uint64_t)rc;
            i += rc;
        } else if (errno != EINTR) {
            /* Full socket buffer, or nobody listening on a unicast port:
             * that datagram is lost, the rest still go. */
            atomic_fetch_add(&s->errors, 1);
            i++;
        }
    }
    /* A part-built datagram that was not sent moves to the front. */
    if (partial)
        s->fill = 0;
    else if (s->fill && s->n_full)
        memcpy(s->dg[0], s->dg[s->n_full], (size_t)s->fill * TS_SIZE);
    s->n_full = 0;
}

static void *udp_open(const char *url, char *err, size_t errlen)
{
    struct udp_sink *s = calloc(1, sizeof(*s));

    if (!s) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    s->fd = -1;
    s->pkt = 7;
    if (!strncmp(url, "udp://", 6) || !strncmp(url, "rtp://", 6)) {
        struct udp_addr a;

        if (udp_parse(url, &a, err, errlen) < 0 || (s->fd = udp_open_tx(&a, err, errlen)) < 0) {
            free(s);
            return NULL;
        }
        s->pkt = a.pkt;
        s->dg = malloc((size_t)SINK_DGRAMS * DGRAM_MAX);
        s->mm = calloc(SINK_DGRAMS, sizeof(*s->mm));
        s->iov = calloc(SINK_DGRAMS, sizeof(*s->iov));
        if (!s->dg || !s->mm || !s->iov) {
            snprintf(err, errlen, "out of memory");
            close(s->fd);
            free(s->dg);
            free(s->mm);
            free(s->iov);
            free(s);
            return NULL;
        }
    } else {
        s->fp = fopen(file_path(url), "wb");
        if (!s->fp) {
            snprintf(err, errlen, "%s: %s", file_path(url), strerror(errno));
            free(s);
            return NULL;
        }
        setvbuf(s->fp, NULL, _IOFBF, 1 << 20);
    }
    return s;
}

static void udp_write(void *ctx, const uint8_t (*pkt)[TS_SIZE], int n)
{
    struct udp_sink *s = ctx;

    if (n == 0) {
        if (s->fp)
            fflush(s->fp);
        else
            flush(s, true);
        return;
    }
    if (s->fp) {
        if (fwrite(pkt, TS_SIZE, (size_t)n, s->fp) != (size_t)n)
            atomic_fetch_add(&s->errors, 1);
        atomic_fetch_add(&s->packets, (uint64_t)n);
        atomic_fetch_add(&s->bytes, (uint64_t)n * TS_SIZE);
        return;
    }
    for (int i = 0; i < n; i++) {
        memcpy(s->dg[s->n_full] + (size_t)s->fill * TS_SIZE, pkt[i], TS_SIZE);
        if (++s->fill == s->pkt) {
            s->fill = 0;
            if (++s->n_full == SINK_DGRAMS - 1)
                flush(s, false);
        }
    }
    atomic_fetch_add(&s->packets, (uint64_t)n);
    atomic_fetch_add(&s->bytes, (uint64_t)n * TS_SIZE);
    flush(s, false);                        /* whole datagrams go at once */
}

static void udp_stats(void *ctx, struct sink_stats *st)
{
    struct udp_sink *s = ctx;

    snprintf(st->state, sizeof(st->state), "%s", s->fp ? "writing" : "sending");
    st->packets = atomic_load(&s->packets);
    st->bytes = atomic_load(&s->bytes);
    st->errors = atomic_load(&s->errors);
}

static void udp_close(void *ctx)
{
    struct udp_sink *s = ctx;

    if (s->fp) {
        if (fclose(s->fp) != 0)
            log_err("output file: %s", strerror(errno));
    }
    if (s->fd >= 0)
        close(s->fd);
    free(s->dg);
    free(s->mm);
    free(s->iov);
    free(s);
}

const struct sink_ops sink_udp_ops = {
    .name = "udp", .takes = udp_takes, .open = udp_open, .write = udp_write,
    .stats = udp_stats, .close = udp_close,
};
