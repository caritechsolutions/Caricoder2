/*
 * caricoder3 - one reader for a multi-program input, shared by its channels
 * Copyright (c) 2026 CariTech Solutions
 *
 *   caricoder3 share --config FILE [--stats-file FILE]
 *
 * FILE: {"input": "udp://224.100.1.5:49500", "outputs": [{"program": 2090,
 * "port": 41001}, ...]}. The reader joins the input once and sends each
 * output its program only (the PAT, the SDT, the program's PMT and the
 * PIDs that PMT names) as a single-program stream to 127.0.0.1:port, 7
 * packets to a datagram, or fewer when the oldest has waited 3 ms. Each
 * channel then reads a stream of its own size instead of the whole
 * multiplex. Program 0 is the first program of the PAT. SIGHUP re-reads
 * FILE (outputs added and removed without a gap for the others); SIGTERM
 * ends it. The supervisor starts one per input that channels share.
 */
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "cc3.h"
#include "clock.h"
#include "json.h"
#include "log.h"
#include "psi.h"
#include "udp.h"

#define SH_OUT_MAX   64
#define SH_BATCH     32
#define SH_DGRAM     2048
#define SH_HOLD_NS   3000000LL          /* the longest a packet waits for six more */

struct sh_out {
    int program;                        /* asked for; 0 = the first */
    int port;
    struct sockaddr_in to;
    uint16_t pmt_pid;                   /* 0 = not in the PAT (yet) */
    uint16_t pids[MAX_ES + 1];          /* the PMT's streams and its PCR PID */
    int n_pids;
    bool pmt_seen;
    uint8_t buf[7 * TS_SIZE];
    int n;                              /* packets waiting */
    int64_t first_ns;
    uint64_t packets, datagrams, send_errors;
};

struct sh {
    char input[512];
    struct sh_out out[SH_OUT_MAX];
    int n_out;
    uint64_t route[PID_COUNT];          /* PID -> the outputs it goes to (bits) */
    struct section_asm pat_asm;
    struct section_asm pmt_asm[SH_OUT_MAX];
    int pat_first;                      /* the PAT's first program */
    uint16_t pat_pmt[SH_OUT_MAX];       /* per output: PMT PID per the PAT */
    uint8_t pat_version;
    bool have_pat;
    int tx;
    uint64_t in_bytes, in_datagrams, overflow;
};

static atomic_int g_stop, g_hup;

static void on_signal(int sig)
{
    if (sig == SIGHUP)
        atomic_store(&g_hup, 1);
    else
        atomic_store(&g_stop, 1);
}

static void routes(struct sh *s)
{
    memset(s->route, 0, sizeof(s->route));
    for (int k = 0; k < s->n_out; k++) {
        const struct sh_out *o = &s->out[k];
        uint64_t bit = 1ULL << k;

        s->route[0] |= bit;
        s->route[PID_SDT] |= bit;
        if (o->pmt_pid)
            s->route[o->pmt_pid] |= bit;
        for (int i = 0; i < o->n_pids; i++)
            s->route[o->pids[i]] |= bit;
    }
    s->route[PID_NULL] = 0;
}

/* The PMT PID of each output from the PAT: by number, or the first. */
static void apply_pat(struct sh *s, const uint8_t *sec, int len)
{
    int first = 0;
    bool changed = false;

    for (int pos = 8; pos + 4 <= len - 4; pos += 4) {
        int num = sec[pos] << 8 | sec[pos + 1];

        if (num && !first)
            first = num;
    }
    s->pat_first = first;
    for (int k = 0; k < s->n_out; k++) {
        struct sh_out *o = &s->out[k];
        int want = o->program ? o->program : first;
        uint16_t pmt = 0;

        for (int pos = 8; pos + 4 <= len - 4; pos += 4) {
            int num = sec[pos] << 8 | sec[pos + 1];

            if (num && num == want)
                pmt = (uint16_t)((sec[pos + 2] & 0x1F) << 8 | sec[pos + 3]);
        }
        if (pmt != o->pmt_pid) {
            if (o->pmt_pid)
                log_info("share: program %d (port %d) left the PAT or moved", want, o->port);
            o->pmt_pid = pmt;
            o->n_pids = 0;
            o->pmt_seen = false;
            section_asm_reset(&s->pmt_asm[k]);
            changed = true;
        }
    }
    if (changed)
        routes(s);
}

struct pmt_ctx {
    struct sh *s;
    int k;
};

static void on_pmt(void *opaque, const uint8_t *sec, int len)
{
    struct pmt_ctx *c = opaque;
    struct sh_out *o = &c->s->out[c->k];
    static struct program p;
    uint16_t pids[MAX_ES + 1];
    int n = 0;

    if (!psi_section_ok(sec, len, 0x02))
        return;
    memset(&p, 0, sizeof(p));
    if (!psi_parse_pmt(sec, len, &p))
        return;
    if (o->program && p.number != o->program)
        return;                         /* another program on a shared PMT PID */
    for (int i = 0; i < p.n_es && n < MAX_ES; i++)
        pids[n++] = p.es[i].pid;
    if (p.pcr_pid != PID_NULL)
        pids[n++] = p.pcr_pid;
    if (o->pmt_seen && n == o->n_pids && !memcmp(pids, o->pids, sizeof(pids[0]) * (size_t)n))
        return;
    memcpy(o->pids, pids, sizeof(pids[0]) * (size_t)n);
    o->n_pids = n;
    if (!o->pmt_seen)
        log_info("share: program %u to port %d, %d stream(s)", p.number, o->port, p.n_es);
    o->pmt_seen = true;
    routes(c->s);
}

static void on_pat(void *opaque, const uint8_t *sec, int len)
{
    struct sh *s = opaque;

    if (!psi_section_ok(sec, len, 0x00))
        return;
    s->have_pat = true;
    s->pat_version = (uint8_t)((sec[5] >> 1) & 0x1F);
    apply_pat(s, sec, len);
}

static void flush(struct sh *s, struct sh_out *o)
{
    if (!o->n)
        return;
    if (sendto(s->tx, o->buf, (size_t)o->n * TS_SIZE, MSG_DONTWAIT,
               (const struct sockaddr *)&o->to, sizeof(o->to)) > 0)
        o->datagrams++;
    else if (errno != ECONNREFUSED)
        o->send_errors++;                   /* (refused: the channel is not listening yet) */
    o->n = 0;
}

static void packet(struct sh *s, const uint8_t *pkt, int64_t now)
{
    uint16_t pid = (uint16_t)((pkt[1] & 0x1F) << 8 | pkt[2]);
    uint64_t to = s->route[pid];
    bool pusi = pkt[1] & 0x40;

    /* tables first, so a change applies from this packet on */
    if (pid == 0 || (to && pid != PID_SDT)) {
        int afc = (pkt[3] >> 4) & 3, off = 4;

        if (afc & 1) {
            if (afc == 3)
                off += 1 + pkt[4];
            if (off < TS_SIZE) {
                if (pid == 0) {
                    section_asm_push(&s->pat_asm, pkt + off, TS_SIZE - off, pusi, on_pat, s);
                } else {
                    for (int k = 0; k < s->n_out; k++)
                        if (s->out[k].pmt_pid == pid) {
                            struct pmt_ctx c = { s, k };

                            section_asm_push(&s->pmt_asm[k], pkt + off, TS_SIZE - off, pusi, on_pmt, &c);
                        }
                }
            }
        }
        to = s->route[pid];
    }
    for (int k = 0; to && k < s->n_out; k++, to >>= 1) {
        struct sh_out *o = &s->out[k];

        if (!(to & 1))
            continue;
        if (!o->n)
            o->first_ns = now;
        memcpy(o->buf + o->n * TS_SIZE, pkt, TS_SIZE);
        o->packets++;
        if (++o->n == 7)
            flush(s, o);
    }
}

/* A datagram: raw TS, or RTP carrying TS. */
static void datagram(struct sh *s, const uint8_t *p, size_t n, int64_t now)
{
    if (n >= 12 + TS_SIZE && (p[0] & 0xC0) == 0x80) {
        size_t hdr = 12 + 4u * (p[0] & 0x0F);

        if ((p[0] & 0x10) && n >= hdr + 4)
            hdr += 4 + 4u * (((size_t)p[hdr + 2] << 8) | p[hdr + 3]);
        if ((p[0] & 0x20) && n > hdr && p[n - 1] <= n - hdr)
            n -= p[n - 1];
        if (hdr < n && p[hdr] == TS_SYNC) {
            p += hdr;
            n -= hdr;
        }
    }
    s->in_bytes += n;
    s->in_datagrams++;
    for (size_t i = 0; i + TS_SIZE <= n; i += TS_SIZE)
        if (p[i] == TS_SYNC)
            packet(s, p + i, now);
}

static bool load(struct sh *s, const char *path, char *err, size_t errlen)
{
    static struct json_doc doc;
    char text[JSON_TEXT_MAX];
    FILE *f = fopen(path, "r");
    size_t len;
    int root, a;
    struct sh_out old[SH_OUT_MAX];
    int n_old = s->n_out;
    const char *input;

    if (!f) {
        snprintf(err, errlen, "%s: %s", path, strerror(errno));
        return false;
    }
    len = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[len] = 0;
    if ((root = json_parse(&doc, text, err, errlen)) < 0)
        return false;
    input = json_get_str(&doc, root, "input", NULL);
    if (!input || !input[0]) {
        snprintf(err, errlen, "no \"input\"");
        return false;
    }
    if (s->input[0] && strcmp(s->input, input)) {
        snprintf(err, errlen, "the input cannot change while running");
        return false;
    }
    snprintf(s->input, sizeof(s->input), "%s", input);
    memcpy(old, s->out, sizeof(old));
    s->n_out = 0;
    a = json_get(&doc, root, "outputs");
    if (a >= 0 && doc.node[a].type == JSON_ARRAY)
        for (int j = doc.node[a].first_child; j >= 0 && s->n_out < SH_OUT_MAX; j = doc.node[j].next) {
            int program = (int)json_get_num(&doc, j, "program", 0);
            int port = (int)json_get_num(&doc, j, "port", 0);
            struct sh_out *o = &s->out[s->n_out];

            if (port <= 0 || port > 65535)
                continue;
            memset(o, 0, sizeof(*o));
            /* an output that was there already keeps what it knows */
            for (int k = 0; k < n_old; k++)
                if (old[k].port == port && old[k].program == program)
                    *o = old[k];            /* packets waiting included: nothing is lost */
            o->program = program;
            o->port = port;
            o->to.sin_family = AF_INET;
            o->to.sin_port = htons((uint16_t)port);
            o->to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            s->n_out++;
        }
    for (int k = 0; k < SH_OUT_MAX; k++)
        section_asm_reset(&s->pmt_asm[k]);      /* the outputs may have moved in the list */
    /* new outputs learn their PMT PID from the next PAT */
    section_asm_reset(&s->pat_asm);
    routes(s);
    return true;
}

static void write_stats(const struct sh *s, const char *path, double in_bps, const struct udp_join *j)
{
    char tmp[600];
    FILE *f;

    snprintf(tmp, sizeof(tmp), "%.590s.tmp", path);
    if (!(f = fopen(tmp, "w")))
        return;
    fprintf(f, "{\"input\": \"%s\", \"bitrate\": %.0f, \"datagrams\": %llu, \"socket_overflow\": %llu, "
               "\"pat\": %s, \"rejoins\": %u, \"joined_on\": \"%s\", \"join_note\": \"%s\", \"outputs\": [", s->input, in_bps,
            (unsigned long long)s->in_datagrams, (unsigned long long)s->overflow, s->have_pat ? "true" : "false",
            j->rejoins, j->ifname, j->note);
    for (int k = 0; k < s->n_out; k++)
        fprintf(f, "%s{\"program\": %d, \"port\": %d, \"pmt_pid\": %u, \"pmt_seen\": %s, \"packets\": %llu, \"send_errors\": %llu}",
                k ? ", " : "", s->out[k].program, s->out[k].port, s->out[k].pmt_pid,
                s->out[k].pmt_seen ? "true" : "false", (unsigned long long)s->out[k].packets,
                (unsigned long long)s->out[k].send_errors);
    fprintf(f, "]}\n");
    fclose(f);
    rename(tmp, path);
}

int share_main(int argc, char **argv);

int share_main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "config", required_argument, 0, 'c' },
        { "stats-file", required_argument, 0, 's' },
        { 0, 0, 0, 0 },
    };
    static struct sh s;
    static uint8_t bufs[SH_BATCH][SH_DGRAM];
    static char cbuf[SH_BATCH][64];
    struct mmsghdr mm[SH_BATCH];
    struct iovec iov[SH_BATCH];
    const char *cfg = NULL, *stats = NULL;
    char err[300];
    struct udp_addr a;
    static struct udp_join join;
    int64_t last_rx = 0;
    int fd, opt;
    int64_t last_stats = 0;
    uint64_t last_bytes = 0;

    optind = 2;
    while ((opt = getopt_long(argc, argv, "c:s:", opts, NULL)) != -1) {
        if (opt == 'c')
            cfg = optarg;
        else if (opt == 's')
            stats = optarg;
        else
            return 2;
    }
    if (!cfg) {
        fprintf(stderr, "usage: caricoder3 share --config FILE [--stats-file FILE]\n");
        return 2;
    }
    if (!load(&s, cfg, err, sizeof(err))) {
        log_err("share: %s", err);
        return 1;
    }
    if (udp_parse(s.input, &a, err, sizeof(err)) < 0 || (fd = udp_open_rx_join(&a, &join, err, sizeof(err))) < 0) {
        log_err("share: %s: %s", s.input, err);
        return 1;
    }
    s.tx = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    {
        int sz = 4 << 20;

        setsockopt(s.tx, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
    }
    signal(SIGHUP, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    log_info("share: %s for %d channel(s)", s.input, s.n_out);

    for (int i = 0; i < SH_BATCH; i++) {
        iov[i].iov_base = bufs[i];
        iov[i].iov_len = SH_DGRAM;
    }
    while (!atomic_load(&g_stop)) {
        struct pollfd pf = { fd, POLLIN, 0 };
        int64_t now;
        int r;

        if (atomic_exchange(&g_hup, 0)) {
            if (!load(&s, cfg, err, sizeof(err)))
                log_err("share: %s (kept the outputs as they were)", err);
            else
                log_info("share: %d channel(s) now", s.n_out);
        }
        r = poll(&pf, 1, 5);
        now = clock_now_ns();
        if (r > 0) {
            for (int i = 0; i < SH_BATCH; i++) {
                memset(&mm[i].msg_hdr, 0, sizeof(mm[i].msg_hdr));
                mm[i].msg_hdr.msg_iov = &iov[i];
                mm[i].msg_hdr.msg_iovlen = 1;
                mm[i].msg_hdr.msg_control = cbuf[i];
                mm[i].msg_hdr.msg_controllen = sizeof(cbuf[i]);
            }
            int n = recvmmsg(fd, mm, SH_BATCH, MSG_DONTWAIT, NULL);

            if (n > 0)
                last_rx = now;
            for (int i = 0; i < n; i++) {
                for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mm[i].msg_hdr); cm; cm = CMSG_NXTHDR(&mm[i].msg_hdr, cm))
                    if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SO_RXQ_OVFL) {
                        uint32_t drops;

                        memcpy(&drops, CMSG_DATA(cm), sizeof(drops));
                        s.overflow = drops;
                    }
                datagram(&s, bufs[i], mm[i].msg_len, now);
            }
        }
        /* nothing for a while: the group joined again (a join lost at start-up, on another interface) */
        if (r <= 0)
            udp_join_tick(&join, fd, last_rx, now);
        /* nothing waits long: a low-rate program still gets its packets */
        for (int k = 0; k < s.n_out; k++)
            if (s.out[k].n && now - s.out[k].first_ns >= SH_HOLD_NS)
                flush(&s, &s.out[k]);
        if (stats && now - last_stats >= 1000000000LL) {
            double dt = last_stats ? (double)(now - last_stats) / 1e9 : 1;

            write_stats(&s, stats, (double)(s.in_bytes - last_bytes) * 8 / dt, &join);
            last_stats = now;
            last_bytes = s.in_bytes;
        }
    }
    for (int k = 0; k < s.n_out; k++)
        flush(&s, &s.out[k]);
    close(fd);
    close(s.tx);
    if (stats)
        unlink(stats);
    log_info("share: %s stopped", s.input);
    return 0;
}
