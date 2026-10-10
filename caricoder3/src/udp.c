/*
 * caricoder3 - UDP sockets (IPv4 unicast and multicast)
 * Copyright (c) 2026 CariTech Solutions
 */
#include "udp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define DEFAULT_RCVBUF  (8 * 1024 * 1024)
#define DEFAULT_SNDBUF  (2 * 1024 * 1024)
#define DEFAULT_TTL     8

static int fail(char *err, size_t errlen, const char *what)
{
    snprintf(err, errlen, "%s: %s", what, strerror(errno));
    return -1;
}

/* Bounded copy; false when the text does not fit. */
static bool put(char *dst, size_t size, const char *src)
{
    size_t n = strlen(src);

    if (n >= size)
        return false;
    memcpy(dst, src, n + 1);
    return true;
}

int udp_parse(const char *url, struct udp_addr *a, char *err, size_t errlen)
{
    char tmp[512];
    char *s, *q, *at, *colon;

    memset(a, 0, sizeof(*a));
    a->pkt = 7;

    if (strncmp(url, "udp://", 6) == 0 || strncmp(url, "rtp://", 6) == 0) {
        url += 6;
    } else {
        snprintf(err, errlen, "not a udp:// or rtp:// URL: %s", url);
        return -1;
    }
    if (!put(tmp, sizeof(tmp), url))
        goto too_long;
    s = tmp;

    q = strchr(s, '?');
    if (q)
        *q++ = '\0';

    at = strchr(s, '@');
    if (at) {
        *at = '\0';
        if (!put(a->source, sizeof(a->source), s))
            goto too_long;
        s = at + 1;
    }
    colon = strrchr(s, ':');
    if (!colon) {
        snprintf(err, errlen, "missing :port in %s", url);
        return -1;
    }
    *colon = '\0';
    if (!put(a->host, sizeof(a->host), s))
        goto too_long;
    a->port = atoi(colon + 1);
    if (a->port <= 0 || a->port > 65535) {
        snprintf(err, errlen, "bad port in %s", url);
        return -1;
    }

    while (q && *q) {
        char *next = strchr(q, '&');
        char *eq;

        if (next)
            *next++ = '\0';
        eq = strchr(q, '=');
        if (eq) {
            *eq++ = '\0';
            if (!strcmp(q, "iface") || !strcmp(q, "interface") || !strcmp(q, "localaddr")) {
                if (!put(a->iface, sizeof(a->iface), eq))
                    goto too_long;
            } else if (!strcmp(q, "source")) {
                if (!put(a->source, sizeof(a->source), eq))
                    goto too_long;
            } else if (!strcmp(q, "ttl")) {
                a->ttl = atoi(eq);
            } else if (!strcmp(q, "buf") || !strcmp(q, "rcvbuf") ||
                       !strcmp(q, "sndbuf") || !strcmp(q, "buffer_size")) {
                a->bufsize = atoi(eq);
            } else if (!strcmp(q, "pkt")) {
                a->pkt = atoi(eq);
            } else {
                snprintf(err, errlen, "unknown URL option '%s'", q);
                return -1;
            }
        }
        q = next;
    }
    if (a->pkt < 1 || a->pkt > 7) {
        snprintf(err, errlen, "pkt must be 1..7");
        return -1;
    }
    if (a->ttl < 0 || a->ttl > 255) {
        snprintf(err, errlen, "ttl must be 0..255");
        return -1;
    }
    return 0;

too_long:
    snprintf(err, errlen, "URL or one of its fields is too long");
    return -1;
}

/* Interface given by name or by its address -> that interface's address. */
static int iface_addr(int fd, const char *iface, struct in_addr *out)
{
    struct ifreq ifr;

    if (inet_pton(AF_INET, iface, out) == 1)
        return 0;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", iface);
    if (ioctl(fd, SIOCGIFADDR, &ifr) < 0)
        return -1;
    *out = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;
    return 0;
}

static void set_bufsize(int fd, int opt, int force_opt, int size)
{
    /* The FORCE variant ignores rmem_max/wmem_max but needs privilege. */
    if (setsockopt(fd, SOL_SOCKET, force_opt, &size, sizeof(size)) < 0)
        setsockopt(fd, SOL_SOCKET, opt, &size, sizeof(size));
}

int udp_open_rx(const struct udp_addr *a, char *err, size_t errlen)
{
    struct sockaddr_in sa;
    struct in_addr group = { 0 }, local = { 0 };
    struct timeval tv = { 0, 200000 };
    int one = 1;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0)
        return fail(err, errlen, "socket");

    if (a->host[0] && inet_pton(AF_INET, a->host, &group) != 1) {
        snprintf(err, errlen, "bad IPv4 address '%s'", a->host);
        close(fd);
        return -1;
    }
    if (a->iface[0] && iface_addr(fd, a->iface, &local) < 0) {
        snprintf(err, errlen, "unknown interface '%s'", a->iface);
        close(fd);
        return -1;
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_RXQ_OVFL, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    set_bufsize(fd, SO_RCVBUF, SO_RCVBUFFORCE, a->bufsize ? a->bufsize : DEFAULT_RCVBUF);

    /* Bind to the group itself, not to INADDR_ANY: otherwise the socket
     * also collects every other stream that uses the same port. */
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)a->port);
    sa.sin_addr = group;
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fail(err, errlen, "bind");
        close(fd);
        return -1;
    }

    if (IN_MULTICAST(ntohl(group.s_addr))) {
        int rc;

        if (a->source[0]) {
            struct ip_mreq_source ms;

            memset(&ms, 0, sizeof(ms));
            ms.imr_multiaddr = group;
            ms.imr_interface = local;
            if (inet_pton(AF_INET, a->source, &ms.imr_sourceaddr) != 1) {
                snprintf(err, errlen, "bad source address '%s'", a->source);
                close(fd);
                return -1;
            }
            rc = setsockopt(fd, IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP, &ms, sizeof(ms));
        } else {
            struct ip_mreq mr;

            memset(&mr, 0, sizeof(mr));
            mr.imr_multiaddr = group;
            mr.imr_interface = local;
            rc = setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof(mr));
        }
        if (rc < 0) {
            fail(err, errlen, "multicast join");
            close(fd);
            return -1;
        }
    }
    return fd;
}

int udp_open_tx(const struct udp_addr *a, char *err, size_t errlen)
{
    struct sockaddr_in sa;
    struct in_addr dst, local = { 0 };
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0)
        return fail(err, errlen, "socket");

    if (inet_pton(AF_INET, a->host, &dst) != 1) {
        snprintf(err, errlen, "bad IPv4 address '%s'", a->host);
        close(fd);
        return -1;
    }
    if (a->iface[0] && iface_addr(fd, a->iface, &local) < 0) {
        snprintf(err, errlen, "unknown interface '%s'", a->iface);
        close(fd);
        return -1;
    }

    set_bufsize(fd, SO_SNDBUF, SO_SNDBUFFORCE, a->bufsize ? a->bufsize : DEFAULT_SNDBUF);

    if (IN_MULTICAST(ntohl(dst.s_addr))) {
        unsigned char ttl = (unsigned char)(a->ttl ? a->ttl : DEFAULT_TTL);
        unsigned char loop = 1;

        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
        if (a->iface[0] &&
            setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &local, sizeof(local)) < 0) {
            fail(err, errlen, "IP_MULTICAST_IF");
            close(fd);
            return -1;
        }
    } else {
        if (a->ttl) {
            int ttl = a->ttl;

            setsockopt(fd, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl));
        }
        if (a->iface[0]) {
            memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_addr = local;
            if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
                fail(err, errlen, "bind");
                close(fd);
                return -1;
            }
        }
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)a->port);
    sa.sin_addr = dst;
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fail(err, errlen, "connect");
        close(fd);
        return -1;
    }
    /* The pacing thread must never block on a full socket buffer. */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}
