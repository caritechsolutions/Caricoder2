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
#include <ifaddrs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "log.h"

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
            } else if (!strcmp(q, "loop") || !strcmp(q, "multicast_loop")) {
                if (strcmp(eq, "0") && strcmp(eq, "1") && strcmp(eq, "false") && strcmp(eq, "true")) {
                    snprintf(err, errlen, "loop must be 0 or 1");
                    return -1;
                }
                a->no_loop = !strcmp(eq, "0") || !strcmp(eq, "false");
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

/* The interface address the route to `group` uses now (0.0.0.0 when there is none) */
static struct in_addr route_addr(struct in_addr group, int port)
{
    struct in_addr out = { 0 };
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0)
        return out;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)(port > 0 ? port : 9));
    sa.sin_addr = group;
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0 && getsockname(fd, (struct sockaddr *)&sa, &sl) == 0)
        out = sa.sin_addr;
    close(fd);
    return out;
}

/* The name of the interface with address `addr` ("?" none) */
static void addr_ifname(struct in_addr addr, char *out, size_t len)
{
    struct ifaddrs *ifa = NULL;

    snprintf(out, len, "%s", addr.s_addr ? "?" : "none");
    if (!addr.s_addr || getifaddrs(&ifa) < 0)
        return;
    for (struct ifaddrs *i = ifa; i; i = i->ifa_next)
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET &&
            ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr == addr.s_addr) {
            snprintf(out, len, "%s", i->ifa_name);
            break;
        }
    freeifaddrs(ifa);
}

static int64_t mono_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Joins (or leaves, `add` false) the group on interface address `local` */
static int membership(int fd, const struct udp_addr *a, struct in_addr group, struct in_addr local, bool add)
{
    if (a->source[0]) {
        struct ip_mreq_source ms;

        memset(&ms, 0, sizeof(ms));
        ms.imr_multiaddr = group;
        ms.imr_interface = local;
        if (inet_pton(AF_INET, a->source, &ms.imr_sourceaddr) != 1) {
            errno = EINVAL;
            return -1;
        }
        return setsockopt(fd, IPPROTO_IP, add ? IP_ADD_SOURCE_MEMBERSHIP : IP_DROP_SOURCE_MEMBERSHIP, &ms, sizeof(ms));
    } else {
        struct ip_mreq mr;

        memset(&mr, 0, sizeof(mr));
        mr.imr_multiaddr = group;
        mr.imr_interface = local;
        return setsockopt(fd, IPPROTO_IP, add ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP, &mr, sizeof(mr));
    }
}

/* Where the group should be joined now: the named interface's address, else the route's */
static bool join_addr(int fd, const struct udp_addr *a, struct in_addr group, struct in_addr *local)
{
    local->s_addr = 0;
    if (a->iface[0])
        return iface_addr(fd, a->iface, local) == 0 && local->s_addr;
    *local = route_addr(group, a->port);
    return true;                        /* (0.0.0.0: the kernel's choice) */
}

int udp_open_rx(const struct udp_addr *a, char *err, size_t errlen)
{
    struct udp_join j;

    return udp_open_rx_join(a, &j, err, errlen);
}

int udp_open_rx_join(const struct udp_addr *a, struct udp_join *j, char *err, size_t errlen)
{
    struct sockaddr_in sa;
    struct in_addr group = { 0 }, local = { 0 };
    struct timeval tv = { 0, 200000 };
    int one = 1;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0)
        return fail(err, errlen, "socket");

    memset(j, 0, sizeof(*j));
    j->a = *a;
    j->at_ns = mono_ns();
    if (a->host[0] && inet_pton(AF_INET, a->host, &group) != 1) {
        snprintf(err, errlen, "bad IPv4 address '%s'", a->host);
        close(fd);
        return -1;
    }
    j->multicast = IN_MULTICAST(ntohl(group.s_addr));
    if (a->iface[0] && !j->multicast && iface_addr(fd, a->iface, &local) < 0) {
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

    if (j->multicast) {
        struct in_addr src;

        if (a->source[0] && inet_pton(AF_INET, a->source, &src) != 1) {
            snprintf(err, errlen, "bad source address '%s'", a->source);
            close(fd);
            return -1;
        }
        if (!join_addr(fd, a, group, &local)) {
            /* the named interface has no address yet: joined when it has */
            snprintf(j->note, sizeof(j->note), "interface %s has no address yet: waiting for it to join %s", a->iface, a->host);
            log_warn("udp: %s", j->note);
            return fd;
        }
        if (membership(fd, a, group, local, true) < 0) {
            fail(err, errlen, "multicast join");
            close(fd);
            return -1;
        }
        j->joined = local.s_addr;
        j->member = true;
        addr_ifname(local, j->ifname, sizeof(j->ifname));
        snprintf(j->note, sizeof(j->note), "joined %s on %s", a->host, local.s_addr ? j->ifname : "the default interface");
    }
    return fd;
}

bool udp_join_tick(struct udp_join *j, int fd, int64_t last_rx_ns, int64_t now_ns)
{
    static const int wait_s[] = { 5, 10, 20, 60 };
    struct in_addr group, local, old;
    int64_t quiet_since, wait;
    char was[32];
    int64_t mono = mono_ns();

    if (!j->multicast || fd < 0)
        return false;
    /* (the callers' clock may differ from this one: only the gap is used) */
    if (last_rx_ns > 0 && now_ns - last_rx_ns < 5000000000LL) {
        j->step = 0;                    /* data comes: all is well */
        j->at_ns = mono;
        return false;
    }
    quiet_since = j->at_ns;
    if (last_rx_ns > 0) {
        int64_t rx_mono = mono - (now_ns - last_rx_ns);

        if (rx_mono > quiet_since)
            quiet_since = rx_mono;
    }
    wait = (int64_t)wait_s[j->step < 3 ? j->step : 3] * 1000000000LL;
    if (mono - quiet_since < wait)
        return false;
    if (inet_pton(AF_INET, j->a.host, &group) != 1)
        return false;
    old.s_addr = (in_addr_t)j->joined;
    snprintf(was, sizeof(was), "%s", j->ifname[0] ? j->ifname : "none");
    if (j->member)
        membership(fd, &j->a, group, old, false);
    j->member = false;
    j->at_ns = mono;
    if (j->step < 3)
        j->step++;
    if (!join_addr(fd, &j->a, group, &local)) {
        j->joined = 0;
        j->ifname[0] = 0;
        snprintf(j->note, sizeof(j->note), "interface %s has no address yet: waiting for it to join %s", j->a.iface, j->a.host);
        return false;
    }
    if (membership(fd, &j->a, group, local, true) < 0) {
        snprintf(j->note, sizeof(j->note), "could not join %s again: %s", j->a.host, strerror(errno));
        log_warn("udp: %s", j->note);
        j->joined = 0;
        return false;
    }
    j->joined = local.s_addr;
    j->member = true;
    addr_ifname(local, j->ifname, sizeof(j->ifname));
    j->rejoins++;
    if (strcmp(was, j->ifname))
        snprintf(j->note, sizeof(j->note), "joined %s again on %s (was %s): nothing came for %lld s",
                 j->a.host, local.s_addr ? j->ifname : "the default interface", was,
                 (long long)((mono - quiet_since) / 1000000000LL));
    else
        snprintf(j->note, sizeof(j->note), "joined %s again on %s: nothing came for %lld s", j->a.host,
                 local.s_addr ? j->ifname : "the default interface", (long long)((mono - quiet_since) / 1000000000LL));
    log_warn("udp: %s", j->note);
    return true;
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
        unsigned char loop = a->no_loop ? 0 : 1;

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
