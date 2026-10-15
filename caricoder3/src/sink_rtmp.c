/*
 * caricoder3 - output as an RTMP push (H.264 video, AAC audio)
 * Copyright (c) 2026 CariTech Solutions
 *
 *   rtmp://host[:1935]/app/stream[?program=N]
 *
 * RTMP carries H.264 and AAC in FLV tags, so this sink demultiplexes the
 * transport stream again (demux.c, the same code the input uses), takes
 * the H.264 stream and the AAC stream of the program, turns each access
 * unit into an FLV tag and pushes the tags to the server over one TCP
 * connection, in RTMP's chunked framing with AMF0 commands. Everything
 * network-side happens in the sink's own RTMP thread: the sink thread
 * (sink.c) only demultiplexes and queues tags, so a slow server never
 * holds up the pipeline; when the queue fills, tags are dropped until
 * the next key frame and counted. The connection is made again after a
 * break, from the next key frame, with the sequence headers sent anew.
 *
 * What is not here: HEVC (RTMP's "enhanced" extension; later if wanted),
 * audio other than AAC (MP3 would fit FLV; nothing else does), and the
 * digest handshake (servers accept the plain one from a publisher).
 */
#include <arpa/inet.h>
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
#include <time.h>
#include <unistd.h>

#include "codec.h"
#include "demux.h"
#include "h264.h"
#include "log.h"
#include "sink.h"
#include "xcode.h"

#define QUEUE_MAX_BYTES   (8u << 20)        /* tags waiting for the server */
#define QUEUE_MAX_TAGS    4096
#define RECONNECT_MS      2000
#define CONNECT_TIMEOUT_MS 5000
#define SEND_TIMEOUT_MS   10000
#define CHUNK_SIZE        4096
#define CSID_CMD          3
#define CSID_AUDIO        4
#define CSID_VIDEO        6
#define CSID_DATA         5

/* ---- FLV tags waiting to be sent --------------------------------------- */

struct tag {
    struct tag *next;
    uint8_t type;                           /* 8 audio, 9 video, 18 data */
    bool key;                               /* a video key frame or a sequence header */
    uint32_t ts_ms;
    uint32_t len;
    uint8_t data[];
};

struct rtmp_sink {
    char host[256], app[256], stream[256];
    int port, program;

    /* the demultiplexer, in the sink thread */
    struct demux *dmx;
    uint16_t vpid, apid;
    bool have_video, have_audio;
    int64_t base_dts;                       /* 90 kHz: the first DTS, timestamp 0 */
    int aac_sr, aac_ch, aac_obj;
    uint8_t sps[512], pps[256];
    int sps_len, pps_len;
    bool seq_sent;                          /* sequence headers queued for this connection */
    bool waiting_key;                       /* dropping until a video key frame */
    struct tag *pend_head, *pend_tail;      /* audio held back meanwhile, sent after the key frame */
    int n_pend;
    int width, height;
    double fps;
    int64_t last_vdts;

    /* the queue: sink thread -> RTMP thread */
    pthread_mutex_t lock;
    pthread_cond_t cond;
    struct tag *head, *tail;
    size_t q_bytes;
    int q_tags;
    bool stop;

    /* the connection, RTMP thread only */
    pthread_t th;
    int fd;
    uint8_t *out;                           /* chunked bytes being written */
    size_t out_len, out_cap;
    uint32_t in_chunk_size, out_chunk_size;
    uint32_t stream_id;
    char state[48];
    _Atomic bool resync;                    /* RTMP thread -> sink thread: start again at a key frame */

    _Atomic uint64_t packets, bytes, errors, reconnects;
};

/* ---- byte writing -------------------------------------------------------- */

struct buf { uint8_t *p; size_t len, cap; };

static bool buf_reserve(struct buf *b, size_t more)
{
    if (b->len + more <= b->cap)
        return true;
    size_t cap = b->cap ? b->cap * 2 : 4096;

    while (cap < b->len + more)
        cap *= 2;
    uint8_t *p = realloc(b->p, cap);

    if (!p)
        return false;
    b->p = p;
    b->cap = cap;
    return true;
}

static void put(struct buf *b, const void *d, size_t n)
{
    if (buf_reserve(b, n)) {
        memcpy(b->p + b->len, d, n);
        b->len += n;
    }
}

static void put8(struct buf *b, unsigned v) { uint8_t x = (uint8_t)v; put(b, &x, 1); }
static void put16(struct buf *b, unsigned v) { put8(b, v >> 8); put8(b, v); }
static void put24(struct buf *b, unsigned v) { put8(b, v >> 16); put8(b, v >> 8); put8(b, v); }
static void put32(struct buf *b, uint32_t v) { put8(b, v >> 24); put8(b, v >> 16); put8(b, v >> 8); put8(b, v); }

/* AMF0 */
static void amf_str(struct buf *b, const char *s)
{
    size_t n = strlen(s);

    put8(b, 0x02);
    put16(b, (unsigned)n);
    put(b, s, n);
}

static void amf_num(struct buf *b, double v)
{
    uint64_t u;
    uint8_t be[8];

    memcpy(&u, &v, 8);
    for (int i = 0; i < 8; i++)
        be[i] = (uint8_t)(u >> (56 - 8 * i));
    put8(b, 0x00);
    put(b, be, 8);
}

static void amf_bool(struct buf *b, bool v) { put8(b, 0x01); put8(b, v); }
static void amf_null(struct buf *b) { put8(b, 0x05); }
static void amf_obj_begin(struct buf *b) { put8(b, 0x03); }
static void amf_key(struct buf *b, const char *k) { size_t n = strlen(k); put16(b, (unsigned)n); put(b, k, n); }
static void amf_obj_end(struct buf *b) { put16(b, 0); put8(b, 0x09); }
static void amf_prop_str(struct buf *b, const char *k, const char *v) { amf_key(b, k); amf_str(b, v); }
static void amf_prop_num(struct buf *b, const char *k, double v) { amf_key(b, k); amf_num(b, v); }
static void amf_prop_bool(struct buf *b, const char *k, bool v) { amf_key(b, k); amf_bool(b, v); }

/* ---- tags into the queue ---------------------------------------------- */

static bool queue_tag(struct rtmp_sink *s, uint8_t type, bool key, uint32_t ts, const struct buf *body)
{
    struct tag *t;

    pthread_mutex_lock(&s->lock);
    if (s->q_bytes + body->len > QUEUE_MAX_BYTES || s->q_tags >= QUEUE_MAX_TAGS) {
        /* The server is not taking it: drop everything queued, and start
         * again from the next key frame (the RTMP thread also does this
         * when it finds the first tag it holds is not a key frame). */
        struct tag *x;

        while ((x = s->head)) {
            s->head = x->next;
            free(x);
        }
        s->tail = NULL;
        s->q_bytes = 0;
        s->q_tags = 0;
        atomic_fetch_add(&s->errors, 1);
        s->waiting_key = s->have_video;
        s->seq_sent = false;
        pthread_mutex_unlock(&s->lock);
        return false;
    }
    t = malloc(sizeof(*t) + body->len);
    if (!t) {
        pthread_mutex_unlock(&s->lock);
        return false;
    }
    t->next = NULL;
    t->type = type;
    t->key = key;
    t->ts_ms = ts;
    t->len = (uint32_t)body->len;
    memcpy(t->data, body->p, body->len);
    if (s->tail)
        s->tail->next = t;
    else
        s->head = t;
    s->tail = t;
    s->q_bytes += body->len;
    s->q_tags++;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->lock);
    return true;
}

static uint32_t ts_ms(struct rtmp_sink *s, int64_t t90)
{
    if (s->base_dts == NOPTS)
        s->base_dts = t90;
    if (t90 < s->base_dts)
        return 0;
    return (uint32_t)((t90 - s->base_dts) / 90);
}

static bool queue_sequence_headers(struct rtmp_sink *s, uint32_t ts)
{
    struct buf b = { 0 };
    bool ok;

    /* onMetaData, so players know what is coming */
    amf_str(&b, "@setDataFrame");
    amf_str(&b, "onMetaData");
    put8(&b, 0x08);                         /* ECMA array */
    put32(&b, 0);
    if (s->have_video) {
        amf_prop_num(&b, "width", s->width);
        amf_prop_num(&b, "height", s->height);
        amf_prop_num(&b, "videocodecid", 7);
        if (s->fps > 0)
            amf_prop_num(&b, "framerate", s->fps);
    }
    if (s->have_audio) {
        amf_prop_num(&b, "audiocodecid", 10);
        amf_prop_num(&b, "audiosamplerate", s->aac_sr);
        amf_prop_num(&b, "audiochannels", s->aac_ch);
        amf_prop_bool(&b, "stereo", s->aac_ch == 2);
    }
    amf_prop_str(&b, "encoder", "caricoder3 " CC3_VERSION);
    amf_obj_end(&b);
    ok = queue_tag(s, 18, true, ts, &b);
    b.len = 0;
    if (!ok) {
        free(b.p);
        return false;
    }

    if (s->have_video && s->sps_len > 3 && s->pps_len > 0) {
        /* AVCDecoderConfigurationRecord */
        put8(&b, 0x17); put8(&b, 0x00); put24(&b, 0);
        put8(&b, 1);
        put8(&b, s->sps[1]); put8(&b, s->sps[2]); put8(&b, s->sps[3]);
        put8(&b, 0xFF);                     /* 4-byte NAL lengths */
        put8(&b, 0xE1);                     /* 1 SPS */
        put16(&b, (unsigned)s->sps_len); put(&b, s->sps, (size_t)s->sps_len);
        put8(&b, 1);                        /* 1 PPS */
        put16(&b, (unsigned)s->pps_len); put(&b, s->pps, (size_t)s->pps_len);
        ok = queue_tag(s, 9, true, ts, &b);
        b.len = 0;
        if (!ok) {
            free(b.p);
            return false;
        }
    }
    if (s->have_audio && s->aac_sr) {
        static const int rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
                                     16000, 12000, 11025, 8000, 7350 };
        int sri = 4;

        for (int i = 0; i < 13; i++)
            if (rates[i] == s->aac_sr)
                sri = i;
        /* AudioSpecificConfig: 5 bits object type, 4 bits rate index, 4 bits channels */
        put8(&b, 0xAF); put8(&b, 0x00);
        put8(&b, (unsigned)((s->aac_obj << 3) | (sri >> 1)));
        put8(&b, (unsigned)(((sri & 1) << 7) | (s->aac_ch << 3)));
        ok = queue_tag(s, 8, true, ts, &b);
        if (!ok) {
            free(b.p);
            return false;
        }
    }
    free(b.p);
    s->seq_sent = true;
    return true;
}

static void pend_release(struct rtmp_sink *s, uint32_t from_ts);

struct nal_ctx { struct rtmp_sink *s; struct buf *b; bool key; bool params_changed; };

static void on_nal(void *ctx, unsigned type, const uint8_t *nal, int len)
{
    struct nal_ctx *c = ctx;
    struct rtmp_sink *s = c->s;

    if (type == 7 && len <= (int)sizeof(s->sps)) {
        if (len != s->sps_len || memcmp(nal, s->sps, (size_t)len)) {
            memcpy(s->sps, nal, (size_t)len);
            s->sps_len = len;
            h264_parse_sps(nal, len, &s->width, &s->height, &s->fps);
            c->params_changed = true;
        }
    } else if (type == 8 && len <= (int)sizeof(s->pps)) {
        if (len != s->pps_len || memcmp(nal, s->pps, (size_t)len)) {
            memcpy(s->pps, nal, (size_t)len);
            s->pps_len = len;
            c->params_changed = true;
        }
    }
    if (type == 9)                          /* access unit delimiter: FLV does without */
        return;
    if (type == 5)
        c->key = true;
    put32(c->b, (uint32_t)len);
    put(c->b, nal, (size_t)len);
}

static void video_unit(struct rtmp_sink *s, const uint8_t *es, size_t len, int64_t pts, int64_t dts)
{
    struct buf b = { 0 };
    struct nal_ctx c = { s, &b, false, false };
    uint32_t ts;
    int32_t cts;

    if (dts == NOPTS)
        dts = pts;
    if (pts == NOPTS)
        return;
    put8(&b, 0x27); put8(&b, 0x01); put24(&b, 0);   /* filled in below */
    h264_each_nal(es, len, on_nal, &c);
    if (b.len <= 5) {
        free(b.p);
        return;
    }
    if (c.key)
        b.p[0] = 0x17;
    if (s->waiting_key) {
        if (!c.key) {
            free(b.p);
            return;
        }
        s->waiting_key = false;
    }
    ts = ts_ms(s, dts);
    if (!s->seq_sent || c.params_changed) {
        if (s->sps_len && s->pps_len) {
            if (!queue_sequence_headers(s, ts)) {
                free(b.p);              /* the queue is full: this GOP is lost */
                return;
            }
        } else {
            free(b.p);
            s->waiting_key = true;          /* no parameters yet: wait for a key frame with them */
            return;
        }
    }
    if (s->pend_head)
        pend_release(s, ts);                /* audio from the key frame on */
    cts = (int32_t)((pts - dts) / 90);
    if (cts < 0)
        cts = 0;
    b.p[2] = (uint8_t)(cts >> 16); b.p[3] = (uint8_t)(cts >> 8); b.p[4] = (uint8_t)cts;
    queue_tag(s, 9, c.key, ts, &b);
    free(b.p);
    s->last_vdts = dts;
}

static void pend_clear(struct rtmp_sink *s)
{
    struct tag *t;

    while ((t = s->pend_head)) {
        s->pend_head = t->next;
        free(t);
    }
    s->pend_tail = NULL;
    s->n_pend = 0;
}

/* Audio that arrived while waiting for the video key frame: held back,
 * and queued after the key frame if it belongs after it in time. */
static void pend_audio(struct rtmp_sink *s, uint32_t ts, const struct buf *b)
{
    struct tag *t = malloc(sizeof(*t) + b->len);

    if (!t)
        return;
    t->next = NULL;
    t->type = 8;
    t->key = false;
    t->ts_ms = ts;
    t->len = (uint32_t)b->len;
    memcpy(t->data, b->p, b->len);
    if (s->pend_tail)
        s->pend_tail->next = t;
    else
        s->pend_head = t;
    s->pend_tail = t;
    if (++s->n_pend > 200) {                /* 4 s: the key frame is not coming */
        t = s->pend_head;
        s->pend_head = t->next;
        free(t);
        s->n_pend--;
    }
}

static void pend_release(struct rtmp_sink *s, uint32_t from_ts)
{
    struct tag *t;

    while ((t = s->pend_head)) {
        s->pend_head = t->next;
        if (t->ts_ms >= from_ts) {
            struct buf b = { t->data, t->len, t->len };

            queue_tag(s, 8, false, t->ts_ms, &b);
        }
        free(t);
    }
    s->pend_tail = NULL;
    s->n_pend = 0;
}

static void audio_unit(struct rtmp_sink *s, const uint8_t *es, size_t len, int64_t pts)
{
    static const int rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
                                 16000, 12000, 11025, 8000, 7350, 0, 0, 0 };
    size_t i = 0;
    int n = 0;

    if (pts == NOPTS)
        return;
    while (i + 7 <= len) {
        if (es[i] != 0xFF || (es[i + 1] & 0xF6) != 0xF0) {
            i++;
            continue;
        }
        int hdr = (es[i + 1] & 1) ? 7 : 9;
        size_t flen = ((size_t)(es[i + 3] & 3) << 11) | ((size_t)es[i + 4] << 3) | (es[i + 5] >> 5);
        int sri = (es[i + 2] >> 2) & 0xF, obj = ((es[i + 2] >> 6) & 3) + 1;
        int ch = ((es[i + 2] & 1) << 2) | (es[i + 3] >> 6);

        if (flen < (size_t)hdr || i + flen > len)
            break;
        if (rates[sri] && (s->aac_sr != rates[sri] || s->aac_ch != ch || s->aac_obj != obj)) {
            s->aac_sr = rates[sri];
            s->aac_ch = ch ? ch : 2;
            s->aac_obj = obj;
            if (s->seq_sent)
                s->seq_sent = false;        /* the headers go again before the next tag */
        }
        if (s->aac_sr) {
            struct buf b = { 0 };
            int64_t t = pts + (int64_t)n * 1024 * 90000 / s->aac_sr;

            put8(&b, 0xAF); put8(&b, 0x01);
            put(&b, es + i + hdr, flen - (size_t)hdr);
            if (s->waiting_key) {
                pend_audio(s, ts_ms(s, t), &b);
            } else {
                if (s->seq_sent || queue_sequence_headers(s, ts_ms(s, t)))
                    queue_tag(s, 8, false, ts_ms(s, t), &b);
            }
            free(b.p);
        }
        i += flen;
        n++;
    }
}

/* ---- the demultiplexer's callbacks (sink thread) -------------------------- */

static void on_program(void *opaque, const struct program *p)
{
    struct rtmp_sink *s = opaque;
    uint16_t vpid = 0, apid = 0;

    for (int i = 0; i < p->n_es; i++) {
        enum codec_id c = xcode_input_codec(p, i);

        if (c == CODEC_H264 && !vpid)
            vpid = p->es[i].pid;
        else if (c == CODEC_AAC && !apid)
            apid = p->es[i].pid;
    }
    if (vpid != s->vpid || apid != s->apid) {
        s->vpid = vpid;
        s->apid = apid;
        s->have_video = vpid != 0;
        s->have_audio = apid != 0;
        s->seq_sent = false;
        s->waiting_key = s->have_video;
        if (!vpid && !apid)
            log_warn("rtmp %s/%s: program %u has no H.264 video and no AAC audio: nothing to push",
                     s->app, s->stream, p->number);
        else
            log_info("rtmp %s/%s: pushing %s%s%s from program %u", s->app, s->stream,
                     vpid ? "H.264" : "", vpid && apid ? " + " : "", apid ? "AAC" : "", p->number);
    }
}

static void on_unit(void *opaque, struct es_unit *u)
{
    struct rtmp_sink *s = opaque;

    if (atomic_exchange(&s->resync, false)) {
        s->seq_sent = false;
        s->waiting_key = s->have_video;
        pend_clear(s);
    }
    if (u->kind == ES_PES) {
        int off = pes_payload_offset(u->data, u->len);

        if (off > 0) {
            if (u->pid == s->vpid)
                video_unit(s, u->data + off, u->len - (size_t)off, u->pts, u->dts);
            else if (u->pid == s->apid)
                audio_unit(s, u->data + off, u->len - (size_t)off, u->pts);
        }
    }
    free(u);
}

static void on_pcr(void *opaque, stc_t pcr, stc_t expected, bool jump, stc_t since)
{
    (void)opaque; (void)pcr; (void)expected; (void)jump; (void)since;
}

/* ---- RTMP: chunks out, chunks in (RTMP thread) --------------------------- */

static bool send_all(struct rtmp_sink *s, const uint8_t *d, size_t len)
{
    while (len) {
        struct pollfd pf = { s->fd, POLLOUT, 0 };
        ssize_t w;

        if (poll(&pf, 1, SEND_TIMEOUT_MS) <= 0)
            return false;
        w = send(s->fd, d, len, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            return false;
        }
        d += w;
        len -= (size_t)w;
    }
    return true;
}

/* One RTMP message, chunked, written to the socket. */
static bool send_message(struct rtmp_sink *s, int csid, uint8_t type, uint32_t ts, uint32_t sid,
                         const uint8_t *body, size_t len)
{
    struct buf b = { 0 };
    size_t off = 0;
    bool ok;

    /* type 0 header (we do not bother compressing headers: a few bytes
     * per tag against kilobytes of payload) */
    put8(&b, (unsigned)csid);
    put24(&b, ts < 0xFFFFFF ? ts : 0xFFFFFF);
    put24(&b, (unsigned)len);
    put8(&b, type);
    {
        uint8_t le[4] = { (uint8_t)sid, (uint8_t)(sid >> 8), (uint8_t)(sid >> 16), (uint8_t)(sid >> 24) };
        put(&b, le, 4);
    }
    if (ts >= 0xFFFFFF)
        put32(&b, ts);
    while (off < len) {
        size_t n = len - off < s->out_chunk_size ? len - off : s->out_chunk_size;

        if (off)
            put8(&b, 0xC0 | (unsigned)csid);    /* type 3: continuation */
        put(&b, body + off, n);
        off += n;
        if (off < len && ts >= 0xFFFFFF)
            put32(&b, ts);                  /* the extended timestamp goes with every chunk */
    }
    ok = send_all(s, b.p, b.len);
    free(b.p);
    return ok;
}

static bool send_command(struct rtmp_sink *s, const struct buf *b)
{
    return send_message(s, CSID_CMD, 20, 0, 0, b->p, b->len);
}

/* Reads one whole message from the server (handling chunking, the few
 * control messages, and returning command/data messages). Returns the
 * message type, 0 on timeout/error. */
struct in_msg { uint8_t type; uint32_t len; uint8_t data[65536]; };

static bool recv_all(struct rtmp_sink *s, uint8_t *d, size_t len, int timeout_ms)
{
    while (len) {
        struct pollfd pf = { s->fd, POLLIN, 0 };
        ssize_t r;

        if (poll(&pf, 1, timeout_ms) <= 0)
            return false;
        r = recv(s->fd, d, len, 0);
        if (r <= 0) {
            if (r < 0 && (errno == EAGAIN || errno == EINTR))
                continue;
            return false;
        }
        d += r;
        len -= (size_t)r;
    }
    return true;
}

struct in_state {
    struct { uint32_t ts, len, delta; uint8_t type; uint32_t sid; uint32_t got; uint8_t *buf; } cs[64];
};

static int read_message(struct rtmp_sink *s, struct in_state *st, struct in_msg *m, int timeout_ms)
{
    for (;;) {
        uint8_t h[16];
        unsigned fmt, csid;
        uint32_t ts;

        if (!recv_all(s, h, 1, timeout_ms))
            return 0;
        fmt = h[0] >> 6;
        csid = h[0] & 0x3F;
        if (csid == 0) {
            if (!recv_all(s, h + 1, 1, timeout_ms))
                return 0;
            csid = 64 + h[1];
        } else if (csid == 1) {
            if (!recv_all(s, h + 1, 2, timeout_ms))
                return 0;
            csid = 64 + h[1] + 256u * h[2];
        }
        if (csid >= 64)
            csid = 63;                      /* out of our table: share the last slot */
        struct in_state *x = st;
        unsigned hl = fmt == 0 ? 11 : fmt == 1 ? 7 : fmt == 2 ? 3 : 0;

        if (hl && !recv_all(s, h, hl, timeout_ms))
            return 0;
        if (fmt <= 2)
            ts = ((uint32_t)h[0] << 16) | ((uint32_t)h[1] << 8) | h[2];
        else
            ts = x->cs[csid].delta;
        if (fmt <= 1) {
            x->cs[csid].len = ((uint32_t)h[3] << 16) | ((uint32_t)h[4] << 8) | h[5];
            x->cs[csid].type = h[6];
        }
        if (fmt == 0)
            x->cs[csid].sid = (uint32_t)h[7] | ((uint32_t)h[8] << 8) | ((uint32_t)h[9] << 16) | ((uint32_t)h[10] << 24);
        if (fmt <= 2 && ts == 0xFFFFFF) {
            if (!recv_all(s, h, 4, timeout_ms))
                return 0;
            ts = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
        }
        if (fmt == 0)
            x->cs[csid].ts = ts;
        else if (x->cs[csid].got == 0) {
            x->cs[csid].delta = ts;
            x->cs[csid].ts += ts;
        }
        if (x->cs[csid].len > 1 << 20)
            return 0;
        if (x->cs[csid].got == 0) {
            free(x->cs[csid].buf);
            x->cs[csid].buf = malloc(x->cs[csid].len + 1);
            if (!x->cs[csid].buf)
                return 0;
        }
        {
            uint32_t want = x->cs[csid].len - x->cs[csid].got;

            if (want > s->in_chunk_size)
                want = s->in_chunk_size;
            if (!recv_all(s, x->cs[csid].buf + x->cs[csid].got, want, timeout_ms))
                return 0;
            x->cs[csid].got += want;
        }
        if (x->cs[csid].got < x->cs[csid].len)
            continue;
        /* a whole message */
        m->type = x->cs[csid].type;
        m->len = x->cs[csid].len < sizeof(m->data) ? x->cs[csid].len : (uint32_t)sizeof(m->data);
        memcpy(m->data, x->cs[csid].buf, m->len);
        x->cs[csid].got = 0;
        switch (m->type) {
        case 1:                             /* set chunk size */
            if (m->len >= 4)
                s->in_chunk_size = ((uint32_t)m->data[0] << 24) | ((uint32_t)m->data[1] << 16) |
                                   ((uint32_t)m->data[2] << 8) | m->data[3];
            continue;
        case 4:                             /* user control: ping -> pong */
            if (m->len >= 6 && m->data[0] == 0 && m->data[1] == 6) {
                uint8_t pong[6] = { 0, 7, m->data[2], m->data[3], m->data[4], m->data[5] };

                send_message(s, 2, 4, 0, 0, pong, 6);
            }
            continue;
        case 5:                             /* window ack size */
        case 6:                             /* set peer bandwidth */
        case 3:                             /* ack */
            continue;
        default:
            return m->type;
        }
    }
}

/* Finds a string in an AMF0 command body (naive, enough for _result /
 * onStatus / codes). */
static bool amf_has(const struct in_msg *m, const char *word)
{
    size_t wl = strlen(word);

    for (uint32_t i = 0; i + wl <= m->len; i++)
        if (!memcmp(m->data + i, word, wl))
            return true;
    return false;
}

static double amf_number_at(const struct in_msg *m, uint32_t off)
{
    uint64_t u = 0;
    double v;

    if (off + 9 > m->len || m->data[off] != 0)
        return -1;
    for (int i = 0; i < 8; i++)
        u = (u << 8) | m->data[off + 1 + i];
    memcpy(&v, &u, 8);
    return v;
}

/* Waits for the _result of the command with transaction `tid`. */
static bool wait_result(struct rtmp_sink *s, struct in_state *st, double tid, struct in_msg *m)
{
    for (int i = 0; i < 20; i++) {
        int t = read_message(s, st, m, CONNECT_TIMEOUT_MS);

        if (t == 0)
            return false;
        if (t != 20)
            continue;
        if (amf_has(m, "_error"))
            return false;
        if (amf_has(m, "_result")) {
            /* "_result" string is 2+7 bytes + 1 marker; the transaction id follows */
            if (amf_number_at(m, 10) == tid)
                return true;
        }
    }
    return false;
}

static bool connect_tcp(struct rtmp_sink *s, char *err, size_t errlen)
{
    struct addrinfo hints, *res, *ai;
    char portstr[8];
    int rc, fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%d", s->port);
    rc = getaddrinfo(s->host, portstr, &hints, &res);
    if (rc != 0) {
        snprintf(err, errlen, "%.100s: %.80s", s->host, gai_strerror(rc));
        return false;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        struct pollfd pf;
        int e = 0;
        socklen_t el = sizeof(e);

        fd = socket(ai->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0 && errno != EINPROGRESS) {
            close(fd);
            fd = -1;
            continue;
        }
        pf.fd = fd;
        pf.events = POLLOUT;
        if (poll(&pf, 1, CONNECT_TIMEOUT_MS) <= 0 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el) < 0 || e) {
            snprintf(err, errlen, "%s", e ? strerror(e) : "timed out");
            close(fd);
            fd = -1;
            continue;
        }
        break;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        if (!err[0])
            snprintf(err, errlen, "cannot connect");
        return false;
    }
    {
        int one = 1;

        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    pthread_mutex_lock(&s->lock);
    s->fd = fd;
    if (s->stop)
        shutdown(fd, SHUT_RDWR);            /* closing meanwhile: do not start */
    pthread_mutex_unlock(&s->lock);
    return true;
}

static bool handshake(struct rtmp_sink *s)
{
    uint8_t c0c1[1537], s0s1s2[1537 + 1536];

    c0c1[0] = 3;
    memset(c0c1 + 1, 0, 8);
    for (int i = 9; i < 1537; i++)
        c0c1[i] = (uint8_t)(i * 7 + 13);
    if (!send_all(s, c0c1, sizeof(c0c1)))
        return false;
    if (!recv_all(s, s0s1s2, sizeof(s0s1s2), CONNECT_TIMEOUT_MS))
        return false;
    if (s0s1s2[0] != 3)
        return false;
    return send_all(s, s0s1s2 + 1, 1536);   /* C2 = S1 */
}

static bool rtmp_connect(struct rtmp_sink *s, char *err, size_t errlen)
{
    struct buf b = { 0 };
    struct in_state st;
    struct in_msg *m = malloc(sizeof(*m));
    char tcurl[600];
    uint8_t cs[4] = { CHUNK_SIZE >> 24, (CHUNK_SIZE >> 16) & 0xFF, (CHUNK_SIZE >> 8) & 0xFF, CHUNK_SIZE & 0xFF };
    bool ok = false;

    memset(&st, 0, sizeof(st));
    err[0] = 0;
    if (!m)
        return false;
    s->in_chunk_size = 128;
    s->out_chunk_size = 128;
    if (!connect_tcp(s, err, errlen))
        goto out;
    if (!handshake(s)) {
        snprintf(err, errlen, "handshake failed");
        goto out;
    }
    if (!send_message(s, 2, 1, 0, 0, cs, 4)) {
        snprintf(err, errlen, "send failed");
        goto out;
    }
    s->out_chunk_size = CHUNK_SIZE;

    snprintf(tcurl, sizeof(tcurl), "rtmp://%s:%d/%s", s->host, s->port, s->app);
    amf_str(&b, "connect"); amf_num(&b, 1);
    amf_obj_begin(&b);
    amf_prop_str(&b, "app", s->app);
    amf_prop_str(&b, "type", "nonprivate");
    amf_prop_str(&b, "flashVer", "FMLE/3.0 (compatible; caricoder3)");
    amf_prop_str(&b, "tcUrl", tcurl);
    amf_obj_end(&b);
    if (!send_command(s, &b) || !wait_result(s, &st, 1, m)) {
        snprintf(err, errlen, "the server refused connect (app '%.100s')", s->app);
        goto out;
    }
    b.len = 0;
    amf_str(&b, "releaseStream"); amf_num(&b, 2); amf_null(&b); amf_str(&b, s->stream);
    send_command(s, &b);
    b.len = 0;
    amf_str(&b, "FCPublish"); amf_num(&b, 3); amf_null(&b); amf_str(&b, s->stream);
    send_command(s, &b);
    b.len = 0;
    amf_str(&b, "createStream"); amf_num(&b, 4); amf_null(&b);
    if (!send_command(s, &b) || !wait_result(s, &st, 4, m)) {
        snprintf(err, errlen, "createStream refused");
        goto out;
    }
    /* _result, 4, null, stream id: "_result"(10) + number(9) + null(1) */
    s->stream_id = (uint32_t)amf_number_at(m, 20);
    if (s->stream_id == 0 && amf_number_at(m, 20) < 0)
        s->stream_id = 1;
    b.len = 0;
    amf_str(&b, "publish"); amf_num(&b, 5); amf_null(&b); amf_str(&b, s->stream); amf_str(&b, "live");
    if (!send_message(s, CSID_CMD, 20, 0, s->stream_id, b.p, b.len)) {
        snprintf(err, errlen, "send failed");
        goto out;
    }
    /* onStatus NetStream.Publish.Start */
    for (int i = 0; i < 20; i++) {
        int t = read_message(s, &st, m, CONNECT_TIMEOUT_MS);

        if (t == 0) {
            snprintf(err, errlen, "no answer to publish");
            goto out;
        }
        if (t == 20 && amf_has(m, "onStatus")) {
            if (amf_has(m, "NetStream.Publish.Start")) {
                ok = true;
                break;
            }
            if (amf_has(m, "error")) {
                snprintf(err, errlen, "publish refused (%s)",
                         amf_has(m, "BadName") ? "name in use or not allowed" : "see the server");
                goto out;
            }
        }
    }
    if (!ok)
        snprintf(err, errlen, "publish not confirmed");
out:
    for (int i = 0; i < 64; i++)
        free(st.cs[i].buf);
    free(b.p);
    free(m);
    if (!ok && s->fd >= 0) {
        pthread_mutex_lock(&s->lock);
        close(s->fd);
        s->fd = -1;
        pthread_mutex_unlock(&s->lock);
    }
    return ok;
}

static void set_state(struct rtmp_sink *s, const char *st)
{
    pthread_mutex_lock(&s->lock);
    snprintf(s->state, sizeof(s->state), "%s", st);
    pthread_mutex_unlock(&s->lock);
}

/* Takes the next tag, waiting up to `ms`. NULL when none or stopping. */
static struct tag *next_tag(struct rtmp_sink *s, int ms)
{
    struct tag *t = NULL;
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&s->lock);
    while (!s->head && !s->stop)
        if (pthread_cond_timedwait(&s->cond, &s->lock, &ts) == ETIMEDOUT)
            break;
    if (s->head) {
        t = s->head;
        s->head = t->next;
        if (!s->head)
            s->tail = NULL;
        s->q_bytes -= t->len;
        s->q_tags--;
    }
    pthread_mutex_unlock(&s->lock);
    return t;
}

static void *rtmp_thread(void *arg)
{
    struct rtmp_sink *s = arg;
    char err[200];

    prctl(PR_SET_NAME, "cc3-rtmp");
    while (!__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE)) {
        struct in_state st;
        struct tag *t;
        bool first = true;

        set_state(s, "connecting");
        if (!rtmp_connect(s, err, sizeof(err))) {
            log_warn("rtmp %s:%d/%s/%s: %s", s->host, s->port, s->app, s->stream, err);
            atomic_fetch_add(&s->errors, 1);
            for (int i = 0; i < RECONNECT_MS / 100 && !__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE); i++)
                usleep(100000);
            continue;
        }
        log_info("rtmp %s:%d/%s/%s: publishing", s->host, s->port, s->app, s->stream);
        set_state(s, "publishing");
        atomic_store(&s->resync, true);     /* the sequence headers go again on every connection */
        memset(&st, 0, sizeof(st));
        while ((t = next_tag(s, 500)) || !__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE)) {
            if (!t) {
                /* idle: look at what the server says (a ping, a close) */
                struct pollfd pf = { s->fd, POLLIN, 0 };

                if (poll(&pf, 1, 0) > 0) {
                    struct in_msg *m = malloc(sizeof(*m));
                    int rc = m ? read_message(s, &st, m, 2000) : 0;

                    free(m);
                    if (rc == 0)
                        break;              /* the server closed, or stopped making sense */
                }
                continue;
            }
            if (first && t->type != 18) {
                free(t);                    /* a connection starts with the headers
                                             * (onMetaData comes first), then a key frame */
                continue;
            }
            first = false;
            {
                int csid = t->type == 8 ? CSID_AUDIO : t->type == 9 ? CSID_VIDEO : CSID_DATA;
                bool ok = send_message(s, csid, t->type, t->ts_ms, s->stream_id, t->data, t->len);

                if (ok) {
                    atomic_fetch_add(&s->bytes, t->len);
                    atomic_fetch_add(&s->packets, 1);
                }
                free(t);
                if (!ok)
                    break;
            }
            /* drain what the server sends (acks, pings) without waiting */
            {
                struct pollfd pf = { s->fd, POLLIN, 0 };

                while (poll(&pf, 1, 0) > 0 && (pf.revents & POLLIN)) {
                    struct in_msg *m = malloc(sizeof(*m));
                    int rc = m ? read_message(s, &st, m, 100) : 0;

                    free(m);
                    if (rc == 0)
                        break;
                }
            }
        }
        for (int i = 0; i < 64; i++)
            free(st.cs[i].buf);
        pthread_mutex_lock(&s->lock);
        close(s->fd);
        s->fd = -1;
        pthread_mutex_unlock(&s->lock);
        if (!__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE)) {
            struct tag *x;

            log_warn("rtmp %s:%d/%s/%s: connection lost, connecting again", s->host, s->port, s->app, s->stream);
            atomic_fetch_add(&s->reconnects, 1);
            pthread_mutex_lock(&s->lock);   /* what was queued for the old link is stale */
            while ((x = s->head)) {
                s->head = x->next;
                free(x);
            }
            s->tail = NULL;
            s->q_bytes = 0;
            s->q_tags = 0;
            pthread_mutex_unlock(&s->lock);
            for (int i = 0; i < RECONNECT_MS / 100 && !__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE); i++)
                usleep(100000);
        }
    }
    return NULL;
}

/* ---- the sink ------------------------------------------------------------- */

static bool rtmp_takes(const char *url)
{
    return !strncmp(url, "rtmp://", 7);
}

static void *rtmp_open(const char *url, char *err, size_t errlen)
{
    struct rtmp_sink *s = calloc(1, sizeof(*s));
    const char *p, *slash, *q, *colon;
    char v[32];
    size_t hl;
    static const struct demux_cb cb = { on_program, on_unit, on_pcr };

    if (!s) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    /* rtmp://host[:port]/app[/more]/stream[?query] */
    p = url + 7;
    q = strchr(p, '?');
    slash = strchr(p, '/');
    if (!slash || (q && slash > q)) {
        snprintf(err, errlen, "rtmp://host[:port]/app/stream expected");
        free(s);
        return NULL;
    }
    hl = (size_t)(slash - p);
    colon = memchr(p, ':', hl);
    s->port = 1935;
    if (colon) {
        s->port = atoi(colon + 1);
        hl = (size_t)(colon - p);
    }
    if (hl == 0 || hl >= sizeof(s->host) || s->port <= 0 || s->port > 65535) {
        snprintf(err, errlen, "rtmp://host[:port]/app/stream expected");
        free(s);
        return NULL;
    }
    memcpy(s->host, p, hl);
    s->host[hl] = 0;
    {
        const char *path = slash + 1;
        const char *end = q ? q : path + strlen(path);
        const char *last = end;

        while (last > path && last[-1] != '/')
            last--;
        if (last == path || last == end || (size_t)(last - path - 1) >= sizeof(s->app) ||
            (size_t)(end - last) >= sizeof(s->stream)) {
            snprintf(err, errlen, "rtmp://host[:port]/app/stream expected");
            free(s);
            return NULL;
        }
        memcpy(s->app, path, (size_t)(last - path - 1));
        s->app[last - path - 1] = 0;
        memcpy(s->stream, last, (size_t)(end - last));
        s->stream[end - last] = 0;
    }
    s->program = sink_url_param(url, "program", v, sizeof(v)) ? atoi(v) : 0;
    s->base_dts = NOPTS;
    s->fd = -1;
    s->last_vdts = NOPTS;
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);
    snprintf(s->state, sizeof(s->state), "connecting");
    s->dmx = demux_new(s->program, &cb, s);
    if (!s->dmx) {
        snprintf(err, errlen, "out of memory");
        free(s);
        return NULL;
    }
    if (pthread_create(&s->th, NULL, rtmp_thread, s) != 0) {
        snprintf(err, errlen, "cannot start the RTMP thread");
        demux_free(s->dmx);
        free(s);
        return NULL;
    }
    return s;
}

static void rtmp_write(void *ctx, const uint8_t (*pkt)[TS_SIZE], int n)
{
    struct rtmp_sink *s = ctx;

    for (int i = 0; i < n; i++)
        demux_packet(s->dmx, pkt[i]);
}

static void rtmp_stats(void *ctx, struct sink_stats *st)
{
    struct rtmp_sink *s = ctx;

    pthread_mutex_lock(&s->lock);
    snprintf(st->state, sizeof(st->state), "%s", s->state);
    pthread_mutex_unlock(&s->lock);
    st->packets = atomic_load(&s->packets);     /* FLV tags, for this type */
    st->bytes = atomic_load(&s->bytes);
    st->errors = atomic_load(&s->errors);
    st->reconnects = atomic_load(&s->reconnects);
}

static void rtmp_close(void *ctx)
{
    struct rtmp_sink *s = ctx;
    struct tag *t;

    /* What the demultiplexer still holds goes out, given a moment. */
    demux_resolve_now(s->dmx);
    demux_flush(s->dmx);
    for (int i = 0; i < 20; i++) {
        bool empty;

        pthread_mutex_lock(&s->lock);
        empty = s->head == NULL;
        pthread_mutex_unlock(&s->lock);
        if (empty)
            break;
        usleep(50000);
    }
    pthread_mutex_lock(&s->lock);
    s->stop = true;
    pthread_cond_broadcast(&s->cond);
    if (s->fd >= 0)
        shutdown(s->fd, SHUT_RDWR);         /* a send or connect in progress returns at once */
    pthread_mutex_unlock(&s->lock);
    pthread_join(s->th, NULL);
    demux_free(s->dmx);
    pend_clear(s);
    while ((t = s->head)) {
        s->head = t->next;
        free(t);
    }
    pthread_cond_destroy(&s->cond);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

const struct sink_ops sink_rtmp_ops = {
    .name = "rtmp", .takes = rtmp_takes, .open = rtmp_open, .write = rtmp_write,
    .stats = rtmp_stats, .close = rtmp_close,
};
