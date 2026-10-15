/*
 * caricoder3 - output as HLS: the packager, and the web server that serves it
 * Copyright (c) 2026 CariTech Solutions
 *
 *   hls://[:port][/name][?segment=4&window=6&dir=/some/path&keep=20]
 *
 * The muxed stream is cut into MPEG-TS segments at video key frames,
 * one set of segments per program (a channel with several renditions
 * gives a master playlist with one variant per rendition, each variant
 * a clean single-program stream: its own PAT, its PMT, its PIDs, no
 * null packets). The last `window` segments are kept in memory and
 * served by this process over HTTP:
 *
 *   /index.m3u8            master playlist (or the media playlist when
 *                          there is one program)
 *   /v0/index.m3u8         media playlist of variant 0 (program order)
 *   /v0/seg123.ts          a segment
 *
 * on a TCP port when the URL names one, and always on a Unix socket next
 * to the control socket (--name), which the supervisor serves on its own
 * port under /hls/CHANNEL/. With `dir=` the segments and playlists are
 * also written to that directory (for nginx, a CDN origin), the newest
 * `keep` segments kept there.
 *
 * Every playlist answer carries the name of the segment that will be
 * listed next (X-Next-Segment), so a proxy in front can fetch it the
 * moment it exists.
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "codec.h"
#include "h264.h"
#include "log.h"
#include "psi.h"
#include "sink.h"

#define HLS_VARIANTS    MAX_PROGRAMS
#define HLS_WINDOW_MAX  64
#define SEG_MAX_BYTES   (32u << 20)         /* a segment never grows past this */
#define HTTP_CLIENTS    64
#define PROBE_PACKETS   8                   /* packets of a PES looked at for the key frame */

struct segment {
    uint32_t seq;
    double duration;                        /* seconds, from the PTS of its first and next key frame */
    int64_t pts_first;                      /* 90 kHz */
    uint8_t *data;
    size_t len, cap;
    int refs;                               /* readers sending it */
};

struct variant {
    bool used;
    struct program prog;
    uint16_t pids[MAX_ES + 4];
    int n_pids;
    uint8_t pat_pkt[TS_SIZE];               /* this variant's PAT, one packet */
    uint8_t pat_cc;
    uint8_t pmt_pkts[8][TS_SIZE];           /* the last PMT seen, to open each segment with */
    int n_pmt;
    uint8_t sdt_pkts[6][TS_SIZE];           /* an SDT naming this service alone */
    int n_sdt;
    uint8_t sdt_cc;
    int width, height;
    double fps;
    uint32_t bandwidth;                     /* bits/s, from the segments */
    bool has_video;
    uint16_t vpid;

    struct segment *cur;                    /* being built */
    struct segment *win[HLS_WINDOW_MAX];    /* complete, oldest first */
    int n_win;
    uint32_t next_seq;
    /* the key-frame probe: a PES started on the video PID at `cut_at`
     * in cur->data, and the first packets of it are being looked at */
    bool probing;
    bool probe_sps_only;                    /* the RAI said key frame; only the SPS is wanted */
    size_t cut_at;
    int64_t cut_pts;
    int probe_n;
    uint8_t probe[PROBE_PACKETS * TS_SIZE];
    int probe_len;
    int64_t last_cut_pts;
};


struct hls_sink {
    int port;
    char name[64];
    char dir[512];
    double target;                          /* seconds per segment */
    int window, keep;

    pthread_mutex_t lock;                   /* variants and their segments */
    struct variant var[HLS_VARIANTS];
    int n_var;
    uint8_t pat_version;
    struct section_asm pat_asm, sdt_asm;
    struct section_asm pmt_asm[HLS_VARIANTS];
    uint16_t pmt_pid[HLS_VARIANTS];
    uint16_t prog_num[HLS_VARIANTS];
    int pmt_vi;                             /* which variant's PMT is being assembled */
    bool have_pat;

    pthread_t th;
    int lfd_tcp, lfd_unix;
    char unix_path[108];
    bool stop;
    int clients;
    _Atomic uint64_t packets, bytes, errors, requests;
};

/* The Unix socket the next HLS sink listens on (set by main from --name). */
static char g_http_socket[108];

void sink_hls_set_socket(const char *path)
{
    snprintf(g_http_socket, sizeof(g_http_socket), "%s", path ? path : "");
}

/* ---- segments ------------------------------------------------------------ */

static struct segment *seg_new(uint32_t seq)
{
    struct segment *g = calloc(1, sizeof(*g));

    if (g)
        g->seq = seq;
    return g;
}

static void seg_unref(struct segment *g)
{
    if (g && --g->refs <= 0) {
        free(g->data);
        free(g);
    }
}

static bool seg_append(struct segment *g, const uint8_t *pkt)
{
    if (g->len + TS_SIZE > g->cap) {
        size_t cap = g->cap ? g->cap * 2 : 256 * 1024;
        uint8_t *d;

        if (cap > SEG_MAX_BYTES)
            return false;
        d = realloc(g->data, cap);
        if (!d)
            return false;
        g->data = d;
        g->cap = cap;
    }
    memcpy(g->data + g->len, pkt, TS_SIZE);
    g->len += TS_SIZE;
    return true;
}

static void dir_write(struct hls_sink *s, const char *name, const uint8_t *data, size_t len);

/* Starts the segment being built again from byte `at`, with the tables
 * in front (what came before the first key frame is not playable). */
static void seg_restart_at(struct variant *v, size_t at, int64_t pts)
{
    struct segment *g = v->cur;
    size_t head = (size_t)(1 + v->n_pmt) * TS_SIZE, rest = g->len - at;
    uint8_t *d = malloc(head + rest + TS_SIZE);

    if (!d)
        return;
    v->pat_pkt[3] = (uint8_t)(0x10 | (v->pat_cc++ & 0x0F));
    memcpy(d, v->pat_pkt, TS_SIZE);
    for (int i = 0; i < v->n_pmt; i++)
        memcpy(d + (size_t)(1 + i) * TS_SIZE, v->pmt_pkts[i], TS_SIZE);
    memcpy(d + head, g->data + at, rest);
    free(g->data);
    g->data = d;
    g->len = head + rest;
    g->cap = head + rest + TS_SIZE;
    g->pts_first = pts;
}
static void dir_remove(struct hls_sink *s, const char *name);
static void playlist_text(struct hls_sink *s, int vi, const char *prefix, char *out, size_t cap);

/* The segment being built ends at byte `at` (the rest starts the next);
 * `pts` is the PTS of the first picture of the next one. */
static void seg_cut(struct hls_sink *s, int vi, size_t at, int64_t pts)
{
    struct variant *v = &s->var[vi];
    struct segment *done = v->cur, *next;
    char name[64];

    next = seg_new(v->next_seq++);
    if (!next)
        return;
    {
        /* the new segment opens with the tables, so it plays on its own */
        size_t head = (size_t)(1 + v->n_pmt) * TS_SIZE, rest = done->len - at;

        next->data = malloc(head + rest + TS_SIZE);
        if (next->data) {
            v->pat_pkt[3] = (uint8_t)(0x10 | (v->pat_cc++ & 0x0F));
            memcpy(next->data, v->pat_pkt, TS_SIZE);
            for (int i = 0; i < v->n_pmt; i++)
                memcpy(next->data + (size_t)(1 + i) * TS_SIZE, v->pmt_pkts[i], TS_SIZE);
            memcpy(next->data + head, done->data + at, rest);
            next->len = head + rest;
            next->cap = head + rest + TS_SIZE;
        }
        done->len = at;
    }
    next->pts_first = pts;
    done->duration = done->pts_first != NOPTS && pts > done->pts_first
                     ? (double)(pts - done->pts_first) / 90000.0 : s->target;
    done->refs = 1;
    pthread_mutex_lock(&s->lock);
    if (v->n_win == s->window + 1) {
        struct segment *old = v->win[0];

        memmove(v->win, v->win + 1, sizeof(v->win[0]) * (size_t)(v->n_win - 1));
        v->n_win--;
        seg_unref(old);
    }
    v->win[v->n_win++] = done;
    v->cur = next;
    {
        /* bandwidth: the window's bytes over its duration */
        double secs = 0;
        size_t bytes = 0;

        for (int i = 0; i < v->n_win; i++) {
            secs += v->win[i]->duration;
            bytes += v->win[i]->len;
        }
        if (secs > 0)
            v->bandwidth = (uint32_t)(bytes * 8.0 / secs);
    }
    pthread_mutex_unlock(&s->lock);
    atomic_fetch_add(&s->packets, done->len / TS_SIZE);
    atomic_fetch_add(&s->bytes, done->len);

    if (s->dir[0]) {
        char *pl = malloc(65536);

        if (!pl)
            return;
        snprintf(name, sizeof(name), "v%d/seg%u.ts", vi, done->seq);
        dir_write(s, name, done->data, done->len);
        playlist_text(s, vi, "", pl, 65536);
        snprintf(name, sizeof(name), "v%d/index.m3u8", vi);
        dir_write(s, name, (const uint8_t *)pl, strlen(pl));
        if (done->seq >= (uint32_t)s->keep) {
            snprintf(name, sizeof(name), "v%d/seg%u.ts", vi, done->seq - (uint32_t)s->keep);
            dir_remove(s, name);
        }
        if (s->n_var > 1)
            playlist_text(s, -1, "", pl, 65536);
        else
            playlist_text(s, 0, "v0/", pl, 65536);
        dir_write(s, "index.m3u8", (const uint8_t *)pl, strlen(pl));
        free(pl);
    }
}

/* ---- the packets, as they come (sink thread) ------------------------------ */

static void variant_setup(struct hls_sink *s, int vi, const struct program *p)
{
    struct variant *v = &s->var[vi];
    struct program list[1];
    uint8_t sec[1024];
    int len;

    v->used = true;
    v->prog = *p;
    v->n_pids = 0;
    v->pids[v->n_pids++] = p->pmt_pid;
    if (p->pcr_pid != 0x1FFF)
        v->pids[v->n_pids++] = p->pcr_pid;
    v->has_video = false;
    for (int i = 0; i < p->n_es && v->n_pids < (int)(sizeof(v->pids) / sizeof(v->pids[0])); i++) {
        v->pids[v->n_pids++] = p->es[i].pid;
        if (!v->has_video && (p->es[i].stream_type == 0x1B || p->es[i].stream_type == 0x24 ||
                              p->es[i].stream_type == 0x02)) {
            v->has_video = true;
            v->vpid = p->es[i].pid;
        }
    }
    /* a PAT listing this program alone */
    list[0] = *p;
    len = psi_build_pat(list, 1, s->pat_version, sec);
    memset(v->pat_pkt, 0xFF, TS_SIZE);
    v->pat_pkt[0] = 0x47;
    v->pat_pkt[1] = 0x40;                   /* PUSI, PID 0 */
    v->pat_pkt[2] = 0x00;
    v->pat_pkt[3] = 0x10;                   /* payload only, CC filled per packet */
    v->pat_pkt[4] = 0;                      /* pointer field */
    if (len > 0 && len <= TS_SIZE - 5)
        memcpy(v->pat_pkt + 5, sec, (size_t)len);
    if (!v->cur) {
        v->cur = seg_new(v->next_seq++);
        if (v->cur)
            v->cur->pts_first = NOPTS;
    }
    v->last_cut_pts = NOPTS;
}

static bool variant_has_pid(const struct variant *v, uint16_t pid)
{
    for (int i = 0; i < v->n_pids; i++)
        if (v->pids[i] == pid)
            return true;
    return false;
}

static int64_t pes_pts(const uint8_t *pkt)
{
    int off = 4;
    const uint8_t *p;

    if (pkt[3] & 0x20)
        off += 1 + pkt[4];
    if (off + 14 > TS_SIZE)
        return NOPTS;
    p = pkt + off;
    if (p[0] != 0 || p[1] != 0 || p[2] != 1 || (p[6] & 0xC0) != 0x80 || !(p[7] & 0x80))
        return NOPTS;
    return ((int64_t)(p[9] & 0x0E) << 29) | ((int64_t)p[10] << 22) | ((int64_t)(p[11] & 0xFE) << 14) |
           ((int64_t)p[12] << 7) | (p[13] >> 1);
}

static void payload_of(const uint8_t *pkt, const uint8_t **d, int *n)
{
    int off = 4;

    if (pkt[3] & 0x20)
        off += 1 + pkt[4];
    if (off > TS_SIZE)
        off = TS_SIZE;
    *d = pkt + off;
    *n = TS_SIZE - off;
}

static void variant_packet(struct hls_sink *s, int vi, const uint8_t *pkt, uint16_t pid)
{
    struct variant *v = &s->var[vi];
    bool pusi = pkt[1] & 0x40;

    if (!v->cur)
        return;
    if (v->has_video && pid == v->vpid) {
        if (pusi) {
            int64_t pts = pes_pts(pkt);
            bool rai = (pkt[3] & 0x20) && pkt[4] > 0 && (pkt[5] & 0x40);

            v->probing = false;
            if (pts != NOPTS) {
                if (v->cur->pts_first == NOPTS)
                    v->cur->pts_first = pts;
                if (rai) {
                    /* the stream says so: cut here if the segment is long enough */
                    if (v->last_cut_pts != NOPTS && pts - v->last_cut_pts >= (int64_t)(s->target * 90000)) {
                        seg_cut(s, vi, v->cur->len, pts);
                        v->last_cut_pts = pts;
                    } else if (v->last_cut_pts == NOPTS) {
                        v->last_cut_pts = pts;
                        seg_restart_at(v, v->cur->len, pts);
                    }
                }
                if (!rai || !v->width) {
                    v->probing = true;
                    v->probe_sps_only = rai;
                    v->cut_at = v->cur->len;
                    v->cut_pts = pts;
                    v->probe_len = 0;
                    v->probe_n = 0;
                }
            }
        }
        if (v->probing) {
            const uint8_t *d;
            int n;

            payload_of(pkt, &d, &n);
            if (v->probe_len + n <= (int)sizeof(v->probe)) {
                memcpy(v->probe + v->probe_len, d, (size_t)n);
                v->probe_len += n;
            }
            v->probe_n++;
            {
                enum codec_id c = CODEC_H264;
                bool key, slice = false;

                for (int i = 0; i < v->prog.n_es; i++)
                    if (v->prog.es[i].pid == v->vpid && v->prog.es[i].stream_type == 0x24)
                        c = CODEC_HEVC;
                key = codec_es_has_key(c, v->probe, (size_t)v->probe_len, false);
                if (c == CODEC_H264 && !v->width)
                    for (int i = 0; i + 4 < v->probe_len; i++)
                        if (v->probe[i] == 0 && v->probe[i + 1] == 0 && v->probe[i + 2] == 1 &&
                            (v->probe[i + 3] & 0x1F) == 7) {
                            int e = i + 3;

                            while (e + 3 <= v->probe_len &&
                                   !(v->probe[e] == 0 && v->probe[e + 1] == 0 && v->probe[e + 2] <= 1))
                                e++;
                            h264_parse_sps(v->probe + i + 3, e - i - 3, &v->width, &v->height, &v->fps);
                            break;
                        }
                /* a non-key slice before any key NAL settles it the other way */
                for (int i = 0; i + 4 < v->probe_len && !key; i++)
                    if (v->probe[i] == 0 && v->probe[i + 1] == 0 && v->probe[i + 2] == 1) {
                        unsigned t = c == CODEC_H264 ? (v->probe[i + 3] & 0x1F) : ((v->probe[i + 3] >> 1) & 0x3F);

                        if ((c == CODEC_H264 && t == 1) || (c == CODEC_HEVC && t <= 9))
                            slice = true;
                    }
                if (v->probe_sps_only) {
                    if (v->width || v->probe_n >= PROBE_PACKETS)
                        v->probing = false;
                } else if (key || slice || v->probe_n >= PROBE_PACKETS) {
                    v->probing = false;
                    if (key) {
                        if (v->last_cut_pts != NOPTS &&
                            v->cut_pts - v->last_cut_pts >= (int64_t)(s->target * 90000)) {
                            seg_cut(s, vi, v->cut_at, v->cut_pts);
                            v->last_cut_pts = v->cut_pts;
                        } else if (v->last_cut_pts == NOPTS) {
                            v->last_cut_pts = v->cut_pts;
                            seg_restart_at(v, v->cut_at, v->cut_pts);
                        }
                    }
                }
            }
        }
    } else if (!v->has_video && pusi) {
        /* audio only: cut on time */
        int64_t pts = pes_pts(pkt);

        if (pts != NOPTS) {
            if (v->cur->pts_first == NOPTS)
                v->cur->pts_first = pts;
            if (v->last_cut_pts == NOPTS)
                v->last_cut_pts = pts;
            else if (pts - v->last_cut_pts >= (int64_t)(s->target * 90000)) {
                seg_cut(s, vi, v->cur->len, pts);
                v->last_cut_pts = pts;
            }
        }
    }
    if (!seg_append(v->cur, pkt)) {
        atomic_fetch_add(&s->errors, 1);
        v->cur->len = 0;                    /* no key frame for 32 MB: start over */
        v->cur->pts_first = NOPTS;
        v->last_cut_pts = NOPTS;
    }
}

static void on_pat(void *opaque, const uint8_t *sec, int len)
{
    struct hls_sink *s = opaque;
    uint16_t ts_id, number, pmt_pid;
    uint8_t version;
    uint16_t nums[MAX_OTHER_PROGRAMS], pids[MAX_OTHER_PROGRAMS];
    int n;

    if (!psi_section_ok(sec, len, 0) ||
        !psi_pat_find(sec, len, 0, &ts_id, &version, &number, &pmt_pid))
        return;
    if (s->have_pat && version == s->pat_version)
        return;
    s->pat_version = version;
    s->have_pat = true;
    s->n_var = 0;
    s->prog_num[s->n_var] = number;
    s->pmt_pid[s->n_var++] = pmt_pid;
    n = psi_pat_others(sec, len, number, nums, pids, MAX_OTHER_PROGRAMS);
    for (int i = 0; i < n && s->n_var < HLS_VARIANTS; i++) {
        s->prog_num[s->n_var] = nums[i];
        s->pmt_pid[s->n_var++] = pids[i];
    }
    for (int i = 0; i < HLS_VARIANTS; i++)
        section_asm_reset(&s->pmt_asm[i]);
}

static void on_pmt(void *opaque, const uint8_t *sec, int len)
{
    struct hls_sink *s = opaque;
    int vi = s->pmt_vi;
    struct program p;

    if (!psi_section_ok(sec, len, 2))
        return;
    memset(&p, 0, sizeof(p));
    p.number = s->prog_num[vi];
    p.pmt_pid = s->pmt_pid[vi];
    if (!psi_parse_pmt(sec, len, &p))
        return;
    if (s->var[vi].used && !memcmp(&p.es, &s->var[vi].prog.es, sizeof(p.es)) &&
        p.pcr_pid == s->var[vi].prog.pcr_pid)
        return;
    pthread_mutex_lock(&s->lock);
    variant_setup(s, vi, &p);
    pthread_mutex_unlock(&s->lock);
    log_info("hls: variant %d is program %u, %d stream%s%s", vi, p.number,
             p.n_es, p.n_es == 1 ? "" : "s", s->var[vi].has_video ? "" : " (no video: cut on time)");
}

/* Packs a section into TS packets on `pid` (payload only, pointer field). */
static int packetize(const uint8_t *sec, int len, uint16_t pid, uint8_t (*out)[TS_SIZE], int max)
{
    int n = 0, off = 0;

    while (off < len && n < max) {
        uint8_t *p = out[n];
        int room = TS_SIZE - 4 - (n == 0 ? 1 : 0), take = len - off < room ? len - off : room;

        memset(p, 0xFF, TS_SIZE);
        p[0] = 0x47;
        p[1] = (uint8_t)((n == 0 ? 0x40 : 0) | (pid >> 8));
        p[2] = (uint8_t)pid;
        p[3] = 0x10;
        if (n == 0) {
            p[4] = 0;
            memcpy(p + 5, sec, (size_t)take);
        } else {
            memcpy(p + 4, sec + off, (size_t)take);
        }
        off += take;
        n++;
    }
    return n;
}

static void on_sdt(void *opaque, const uint8_t *sec, int len)
{
    struct hls_sink *s = opaque;

    if (!psi_section_ok(sec, len, 0x42))
        return;
    for (int vi = 0; vi < s->n_var; vi++) {
        struct variant *v = &s->var[vi];
        struct program list[1];
        uint8_t out[1024];
        int olen;

        if (!v->used || !psi_parse_sdt(sec, len, &v->prog))
            continue;
        list[0] = v->prog;
        olen = psi_build_sdt(list, 1, v->prog.sdt_version, out);
        if (olen > 0)
            v->n_sdt = packetize(out, olen, 0x11, v->sdt_pkts, 6);
    }
}

static void hls_packet(struct hls_sink *s, const uint8_t *pkt)
{
    uint16_t pid = (uint16_t)(((pkt[1] & 0x1F) << 8) | pkt[2]);

    if (pkt[0] != 0x47 || pid == 0x1FFF)
        return;
    if (pid == 0) {
        const uint8_t *d;
        int n;

        payload_of(pkt, &d, &n);
        if (pkt[3] & 0x10)
            section_asm_push(&s->pat_asm, d, n, pkt[1] & 0x40, on_pat, s);
        return;
    }
    for (int vi = 0; vi < s->n_var; vi++) {
        if (pid == s->pmt_pid[vi]) {
            const uint8_t *d;
            int n;

            payload_of(pkt, &d, &n);
            s->pmt_vi = vi;
            if (pkt[3] & 0x10)
                section_asm_push(&s->pmt_asm[vi], d, n, pkt[1] & 0x40, on_pmt, s);
        }
        if (s->var[vi].used && variant_has_pid(&s->var[vi], pid)) {
            struct variant *v = &s->var[vi];

            /* a PAT of our own ahead of every PMT, and the PMT remembered */
            if (pid == v->prog.pmt_pid) {
                if (pkt[1] & 0x40) {
                    if (v->cur) {
                        v->pat_pkt[3] = (uint8_t)(0x10 | (v->pat_cc++ & 0x0F));
                        seg_append(v->cur, v->pat_pkt);
                    }
                    v->n_pmt = 0;
                }
                if (v->n_pmt < 8)
                    memcpy(v->pmt_pkts[v->n_pmt++], pkt, TS_SIZE);
            }
            variant_packet(s, vi, pkt, pid);
        }
    }
    /* the SDT: each variant gets one naming its service alone, where
     * the stream's own SDT was */
    if (pid == 0x11) {
        const uint8_t *d;
        int n;

        payload_of(pkt, &d, &n);
        if (pkt[3] & 0x10)
            section_asm_push(&s->sdt_asm, d, n, pkt[1] & 0x40, on_sdt, s);
        if (pkt[1] & 0x40)
            for (int vi = 0; vi < s->n_var; vi++) {
                struct variant *v = &s->var[vi];

                if (!v->used || !v->cur)
                    continue;
                for (int i = 0; i < v->n_sdt; i++) {
                    v->sdt_pkts[i][3] = (uint8_t)(0x10 | (v->sdt_cc++ & 0x0F));
                    seg_append(v->cur, v->sdt_pkts[i]);
                }
            }
    }
}

/* ---- playlists ------------------------------------------------------------ */

/* vi < 0: the master playlist. `prefix` goes in front of segment names. */
static void playlist_text(struct hls_sink *s, int vi, const char *prefix, char *out, size_t cap)
{
    size_t o = 0;

    pthread_mutex_lock(&s->lock);
    if (vi < 0) {
        o += (size_t)snprintf(out + o, cap - o, "#EXTM3U\n#EXT-X-VERSION:3\n");
        for (int i = 0; i < s->n_var && o < cap; i++) {
            struct variant *v = &s->var[i];
            char name[SERVICE_TEXT_MAX + 1];

            if (!v->used)
                continue;
            program_service_name(&v->prog, name, sizeof(name));
            o += (size_t)snprintf(out + o, cap - o, "#EXT-X-STREAM-INF:BANDWIDTH=%u",
                                  v->bandwidth ? v->bandwidth : 1000000);
            if (v->width && v->height)
                o += (size_t)snprintf(out + o, cap - o, ",RESOLUTION=%dx%d", v->width, v->height);
            if (v->fps > 0)
                o += (size_t)snprintf(out + o, cap - o, ",FRAME-RATE=%.3f", v->fps);
            if (name[0])
                o += (size_t)snprintf(out + o, cap - o, ",NAME=\"%s\"", name);
            o += (size_t)snprintf(out + o, cap - o, "\nv%d/index.m3u8\n", i);
        }
    } else if (vi < s->n_var && s->var[vi].used) {
        struct variant *v = &s->var[vi];
        double target = s->target;

        for (int i = 0; i < v->n_win; i++)
            if (v->win[i]->duration > target)
                target = v->win[i]->duration;
        o += (size_t)snprintf(out + o, cap - o,
                              "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:%d\n"
                              "#EXT-X-MEDIA-SEQUENCE:%u\n",
                              (int)(target + 0.999), v->n_win ? v->win[0]->seq : v->next_seq);
        for (int i = 0; i < v->n_win && o < cap; i++)
            o += (size_t)snprintf(out + o, cap - o, "#EXTINF:%.3f,\n%sseg%u.ts\n",
                                  v->win[i]->duration, prefix, v->win[i]->seq);
    } else {
        o += (size_t)snprintf(out + o, cap - o, "#EXTM3U\n");
    }
    pthread_mutex_unlock(&s->lock);
    if (o >= cap)
        out[cap - 1] = 0;
}

/* ---- the directory ---------------------------------------------------------- */

static void dir_write(struct hls_sink *s, const char *name, const uint8_t *data, size_t len)
{
    char path[1024], tmp[1040];
    int fd;

    snprintf(path, sizeof(path), "%s/%s", s->dir, name);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        char *slash = strrchr(path, '/');

        if (slash) {                        /* the variant directory may not exist yet */
            *slash = 0;
            mkdir(path, 0755);
            *slash = '/';
        }
        fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    }
    if (fd < 0) {
        atomic_fetch_add(&s->errors, 1);
        return;
    }
    while (len) {
        ssize_t w = write(fd, data, len);

        if (w <= 0) {
            atomic_fetch_add(&s->errors, 1);
            break;
        }
        data += w;
        len -= (size_t)w;
    }
    close(fd);
    if (rename(tmp, path) < 0)
        atomic_fetch_add(&s->errors, 1);
}

static void dir_remove(struct hls_sink *s, const char *name)
{
    char path[1024];

    snprintf(path, sizeof(path), "%s/%s", s->dir, name);
    unlink(path);
}

/* ---- HTTP ------------------------------------------------------------------- */

/* The sequence number of the segment being built (listed next). */
static unsigned next_seq_of(struct hls_sink *s, int vi)
{
    unsigned seq;

    pthread_mutex_lock(&s->lock);
    seq = s->var[vi].cur ? s->var[vi].cur->seq : s->var[vi].next_seq;
    pthread_mutex_unlock(&s->lock);
    return seq;
}

struct client { struct hls_sink *s; int fd; };

static bool send_bytes(int fd, const void *d, size_t len)
{
    const uint8_t *p = d;

    while (len) {
        struct pollfd pf = { fd, POLLOUT, 0 };
        ssize_t w;

        if (poll(&pf, 1, 10000) <= 0)
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

static void reply(int fd, int code, const char *type, const char *extra, const void *body, size_t len)
{
    char hdr[1024];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Access-Control-Allow-Origin: *\r\nCache-Control: %s\r\nConnection: close\r\n%s\r\n",
                     code, code == 200 ? "OK" : code == 404 ? "Not Found" : "Bad Request", type, len,
                     strstr(type, "mpegurl") ? "no-cache" : "max-age=60", extra ? extra : "");

    if (send_bytes(fd, hdr, (size_t)n))
        send_bytes(fd, body, len);
}

static void *client_thread(void *arg)
{
    struct client *c = arg;
    struct hls_sink *s = c->s;
    char req[2048], path[512];
    int n = 0;
    struct pollfd pf = { c->fd, POLLIN, 0 };

    prctl(PR_SET_NAME, "cc3-http");
    atomic_fetch_add(&s->requests, 1);
    while (n < (int)sizeof(req) - 1 && poll(&pf, 1, 5000) > 0) {
        ssize_t r = recv(c->fd, req + n, sizeof(req) - 1 - (size_t)n, 0);

        if (r <= 0)
            break;
        n += (int)r;
        req[n] = 0;
        if (strstr(req, "\r\n\r\n"))
            break;
    }
    req[n] = 0;
    if (sscanf(req, "GET %511s", path) != 1) {
        reply(c->fd, 400, "text/plain", NULL, "bad request\n", 12);
        goto out;
    }
    {
        char *q = strchr(path, '?');
        const char *p = path;
        int vi = -1;
        unsigned seq;

        if (q)
            *q = 0;
        /* the supervisor hands us /hls/NAME/...; the name is ours to drop */
        if (!strncmp(p, "/hls/", 5)) {
            const char *slash = strchr(p + 5, '/');

            p = slash ? slash : "/";
        }
        if (!strcmp(p, "/") || !strcmp(p, "/index.m3u8") || !strcmp(p, "/master.m3u8")) {
            char *pl = malloc(65536);
            char extra[128] = "";
            int one = -1;

            if (!pl)
                goto out;
            pthread_mutex_lock(&s->lock);
            if (s->n_var == 1)
                one = 0;
            pthread_mutex_unlock(&s->lock);
            if (one == 0) {
                playlist_text(s, 0, "v0/", pl, 65536);
                snprintf(extra, sizeof(extra), "X-Next-Segment: v0/seg%u.ts\r\n", next_seq_of(s, 0));
            } else {
                playlist_text(s, -1, "", pl, 65536);
            }
            reply(c->fd, 200, "application/vnd.apple.mpegurl", extra, pl, strlen(pl));
            free(pl);
            goto out;
        }
        int used = 0;

        if (sscanf(p, "/v%d/index.m3u8%n", &vi, &used) == 1 && used == (int)strlen(p) &&
            vi >= 0 && vi < HLS_VARIANTS) {
            char *pl = malloc(65536);
            char extra[128];

            if (!pl)
                goto out;
            playlist_text(s, vi, "", pl, 65536);
            snprintf(extra, sizeof(extra), "X-Next-Segment: seg%u.ts\r\n", next_seq_of(s, vi));
            reply(c->fd, 200, "application/vnd.apple.mpegurl", extra, pl, strlen(pl));
            free(pl);
            goto out;
        }
        if (sscanf(p, "/v%d/seg%u.ts%n", &vi, &seq, &used) == 2 && used == (int)strlen(p) &&
            vi >= 0 && vi < HLS_VARIANTS) {
            struct segment *g = NULL;

            pthread_mutex_lock(&s->lock);
            for (int i = 0; i < s->var[vi].n_win; i++)
                if (s->var[vi].win[i]->seq == seq) {
                    g = s->var[vi].win[i];
                    g->refs++;
                    break;
                }
            pthread_mutex_unlock(&s->lock);
            if (g) {
                reply(c->fd, 200, "video/mp2t", NULL, g->data, g->len);
                pthread_mutex_lock(&s->lock);
                seg_unref(g);
                pthread_mutex_unlock(&s->lock);
                goto out;
            }
        }
        reply(c->fd, 404, "text/plain", NULL, "not here\n", 9);
    }
out:
    close(c->fd);
    pthread_mutex_lock(&s->lock);
    s->clients--;
    pthread_mutex_unlock(&s->lock);
    free(c);
    return NULL;
}

static void accept_one(struct hls_sink *s, int lfd)
{
    int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
    struct client *c;
    pthread_t th;
    pthread_attr_t attr;
    bool full;

    if (fd < 0)
        return;
    pthread_mutex_lock(&s->lock);
    full = s->clients >= HTTP_CLIENTS;
    if (!full)
        s->clients++;
    pthread_mutex_unlock(&s->lock);
    if (full) {
        close(fd);
        return;
    }
    c = malloc(sizeof(*c));
    if (!c) {
        close(fd);
        return;
    }
    c->s = s;
    c->fd = fd;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    if (pthread_create(&th, &attr, client_thread, c) != 0) {
        close(fd);
        free(c);
        pthread_mutex_lock(&s->lock);
        s->clients--;
        pthread_mutex_unlock(&s->lock);
    }
    pthread_attr_destroy(&attr);
}

static void *http_thread(void *arg)
{
    struct hls_sink *s = arg;

    prctl(PR_SET_NAME, "cc3-hls");
    while (!__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE)) {
        struct pollfd pf[2];
        int n = 0;

        if (s->lfd_tcp >= 0) {
            pf[n].fd = s->lfd_tcp;
            pf[n++].events = POLLIN;
        }
        if (s->lfd_unix >= 0) {
            pf[n].fd = s->lfd_unix;
            pf[n++].events = POLLIN;
        }
        if (n == 0 || poll(pf, (nfds_t)n, 200) <= 0)
            continue;
        for (int i = 0; i < n; i++)
            if (pf[i].revents & POLLIN)
                accept_one(s, pf[i].fd);
    }
    /* let the client threads finish their replies */
    for (int i = 0; i < 50; i++) {
        bool none;

        pthread_mutex_lock(&s->lock);
        none = s->clients == 0;
        pthread_mutex_unlock(&s->lock);
        if (none)
            break;
        usleep(100000);
    }
    return NULL;
}

/* ---- the sink ------------------------------------------------------------- */

static bool hls_takes(const char *url)
{
    return !strncmp(url, "hls://", 6);
}

static void *hls_open(const char *url, char *err, size_t errlen)
{
    struct hls_sink *s = calloc(1, sizeof(*s));
    char v[600];
    const char *p = url + 6, *q = strchr(p, '?'), *slash;

    if (!s) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    s->lfd_tcp = s->lfd_unix = -1;
    s->target = 4;
    s->window = 6;
    s->keep = 20;
    pthread_mutex_init(&s->lock, NULL);
    /* hls://[host]:port/name or hls:///name or hls:// */
    slash = strchr(p, '/');
    if (q && slash > q)
        slash = NULL;
    {
        size_t hl = slash ? (size_t)(slash - p) : q ? (size_t)(q - p) : strlen(p);
        char hostport[256];
        const char *colon;

        if (hl >= sizeof(hostport)) {
            snprintf(err, errlen, "hls://[:port][/name] expected");
            goto fail;
        }
        memcpy(hostport, p, hl);
        hostport[hl] = 0;
        colon = strrchr(hostport, ':');
        if (colon)
            s->port = atoi(colon + 1);
        if (hl && !colon) {
            snprintf(err, errlen, "hls://[:port][/name] expected (the port needs a colon)");
            goto fail;
        }
        if (slash) {
            const char *end = q ? q : slash + strlen(slash);
            size_t nl = (size_t)(end - slash - 1);

            if (nl >= sizeof(s->name))
                nl = sizeof(s->name) - 1;
            memcpy(s->name, slash + 1, nl);
            s->name[nl] = 0;
        }
    }
    if (sink_url_param(url, "segment", v, sizeof(v))) {
        s->target = atof(v);
        if (s->target < 0.5 || s->target > 60) {
            snprintf(err, errlen, "segment must be 0.5..60 seconds");
            goto fail;
        }
    }
    if (sink_url_param(url, "window", v, sizeof(v))) {
        s->window = atoi(v);
        if (s->window < 2 || s->window > HLS_WINDOW_MAX - 1) {
            snprintf(err, errlen, "window must be 2..%d segments", HLS_WINDOW_MAX - 1);
            goto fail;
        }
    }
    if (sink_url_param(url, "keep", v, sizeof(v)))
        s->keep = atoi(v) > s->window ? atoi(v) : s->window;
    if (sink_url_param(url, "dir", s->dir, sizeof(s->dir))) {
        struct stat st;

        if (stat(s->dir, &st) < 0 || !S_ISDIR(st.st_mode)) {
            snprintf(err, errlen, "dir %s: not a directory", s->dir);
            goto fail;
        }
    }
    if (s->port) {
        /* IPv6 (with v4 mapped) when the machine has it, else IPv4 */
        struct sockaddr_in6 sa6;
        struct sockaddr_in sa4;
        int one = 1, off = 0;
        bool ok;

        s->lfd_tcp = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (s->lfd_tcp >= 0) {
            setsockopt(s->lfd_tcp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            setsockopt(s->lfd_tcp, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
            memset(&sa6, 0, sizeof(sa6));
            sa6.sin6_family = AF_INET6;
            sa6.sin6_port = htons((uint16_t)s->port);
            sa6.sin6_addr = in6addr_any;
            ok = bind(s->lfd_tcp, (struct sockaddr *)&sa6, sizeof(sa6)) >= 0;
        } else {
            s->lfd_tcp = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (s->lfd_tcp < 0) {
                snprintf(err, errlen, "socket: %s", strerror(errno));
                goto fail;
            }
            setsockopt(s->lfd_tcp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            memset(&sa4, 0, sizeof(sa4));
            sa4.sin_family = AF_INET;
            sa4.sin_port = htons((uint16_t)s->port);
            sa4.sin_addr.s_addr = htonl(INADDR_ANY);
            ok = bind(s->lfd_tcp, (struct sockaddr *)&sa4, sizeof(sa4)) >= 0;
        }
        if (!ok || listen(s->lfd_tcp, 32) < 0) {
            snprintf(err, errlen, "port %d: %s", s->port, strerror(errno));
            goto fail;
        }
    }
    if (g_http_socket[0]) {
        struct sockaddr_un sa;

        s->lfd_unix = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", g_http_socket);
        unlink(sa.sun_path);
        if (s->lfd_unix < 0 || bind(s->lfd_unix, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
            listen(s->lfd_unix, 32) < 0) {
            snprintf(err, errlen, "%s: %s", g_http_socket, strerror(errno));
            goto fail;
        }
        chmod(sa.sun_path, 0660);
        snprintf(s->unix_path, sizeof(s->unix_path), "%s", g_http_socket);
    }
    if (s->lfd_tcp < 0 && s->lfd_unix < 0 && !s->dir[0]) {
        snprintf(err, errlen, "nowhere to serve from: give a port (hls://:8080), a dir=, or run with --name");
        goto fail;
    }
    if (pthread_create(&s->th, NULL, http_thread, s) != 0) {
        snprintf(err, errlen, "cannot start the HTTP thread");
        goto fail;
    }
    return s;
fail:
    if (s->lfd_tcp >= 0)
        close(s->lfd_tcp);
    if (s->lfd_unix >= 0) {
        close(s->lfd_unix);
        unlink(s->unix_path);
    }
    pthread_mutex_destroy(&s->lock);
    free(s);
    return NULL;
}

static void hls_write(void *ctx, const uint8_t (*pkt)[TS_SIZE], int n)
{
    struct hls_sink *s = ctx;

    for (int i = 0; i < n; i++)
        hls_packet(s, pkt[i]);
}

static void hls_stats(void *ctx, struct sink_stats *st)
{
    struct hls_sink *s = ctx;
    int segs = 0, vars = 0;

    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < s->n_var; i++)
        if (s->var[i].used) {
            vars++;
            segs += s->var[i].n_win;
        }
    snprintf(st->state, sizeof(st->state), "serving: %d variant%s, %d segments, %llu requests",
             vars, vars == 1 ? "" : "s", segs, (unsigned long long)atomic_load(&s->requests));
    pthread_mutex_unlock(&s->lock);
    st->packets = atomic_load(&s->packets);
    st->bytes = atomic_load(&s->bytes);
    st->errors = atomic_load(&s->errors);
}

static void hls_close(void *ctx)
{
    struct hls_sink *s = ctx;

    __atomic_store_n(&s->stop, true, __ATOMIC_RELEASE);
    pthread_join(s->th, NULL);
    if (s->lfd_tcp >= 0)
        close(s->lfd_tcp);
    if (s->lfd_unix >= 0) {
        close(s->lfd_unix);
        unlink(s->unix_path);
    }
    for (int i = 0; i < HLS_VARIANTS; i++) {
        struct variant *v = &s->var[i];

        for (int j = 0; j < v->n_win; j++)
            seg_unref(v->win[j]);
        if (v->cur) {
            free(v->cur->data);
            free(v->cur);
        }
    }
    pthread_mutex_destroy(&s->lock);
    free(s);
}

const struct sink_ops sink_hls_ops = {
    .name = "hls", .takes = hls_takes, .open = hls_open, .write = hls_write,
    .stats = hls_stats, .close = hls_close,
};
