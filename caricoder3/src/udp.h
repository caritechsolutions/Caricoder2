/*
 * caricoder3 - UDP sockets (IPv4 unicast and multicast)
 * Copyright (c) 2026 CariTech Solutions
 */
#ifndef CC3_UDP_H
#define CC3_UDP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/*
 * A multicast input's membership, kept up. The group is joined on the
 * interface its route uses (or the one named with iface=); when nothing
 * has come for a while it is left and joined again, the interface looked
 * up afresh: a join lost (the switch still starting, the multicast
 * interface not up yet, so the join went out of another one) is sent
 * again, as another program joining the group would. After 5 s of quiet,
 * then 10, 20, then every 60 s while it stays quiet. A named interface
 * without an address yet is waited for, not an error.
 */
struct udp_join {
    struct udp_addr a;
    bool multicast;
    bool member;                /* a membership is held now */
    unsigned long joined;       /* the interface address of the membership (network order), 0: none */
    char ifname[32];            /* ... its name */
    int64_t at_ns;              /* opened, or last joined again */
    int step;
    unsigned rejoins;
    char note[200];             /* the last thing it did, for the statistics */
};
int udp_open_rx_join(const struct udp_addr *a, struct udp_join *j, char *err, size_t errlen);
/* Every idle moment (no datagram): `last_rx_ns` the last datagram (0 none),
 * on the same clock as `now_ns`. True when it joined again just now. */
bool udp_join_tick(struct udp_join *j, int fd, int64_t last_rx_ns, int64_t now_ns);

#endif
