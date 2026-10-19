/*
 * caricoder3 - UDP sockets (IPv4 unicast and multicast)
 * Copyright (c) 2026 CariTech Solutions
 */
#ifndef CC3_UDP_H
#define CC3_UDP_H

#include <stdbool.h>
#include <stddef.h>

struct udp_addr {
    char host[64];      /* group or address; empty = any (receive only) */
    int  port;
    char source[64];    /* source-specific multicast, optional */
    char iface[64];     /* interface name or its IPv4 address, optional */
    int  ttl;           /* 0 = default */
    int  bufsize;       /* socket buffer in bytes, 0 = default */
    int  pkt;           /* TS packets per datagram on output, 1..7 */
    bool no_loop;       /* output to a group: not delivered back to this machine */
};

/* udp://[source@]host:port[?iface=..&ttl=..&buf=..&pkt=..&source=..]
 * rtp:// is accepted too (RTP is detected per datagram on input). */
int udp_parse(const char *url, struct udp_addr *a, char *err, size_t errlen);

/* Both return a socket or -1 with a message in err. */
int udp_open_rx(const struct udp_addr *a, char *err, size_t errlen);
int udp_open_tx(const struct udp_addr *a, char *err, size_t errlen);

#endif
