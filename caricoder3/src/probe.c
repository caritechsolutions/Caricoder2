/*
 * caricoder3 probe - what is in a source: programs, streams, rates
 * Copyright (c) 2026 CariTech Solutions
 *
 *   caricoder3 probe URL [--seconds N] [--program N]
 *
 * Reads the source for N seconds (default 3; a file is read whole, up to
 * a limit) and prints JSON: every program of the multiplex with its name,
 * PCR PID and streams; for each stream the PID, type, kind (video, audio,
 * subtitles, teletext, data), language, bit rate, and what the bytes say
 * about it: picture size, interlace and frame rate for video, sample
 * rate and channels for audio (through libavcodec's parsers, which read
 * headers without decoding). The GUI's "Scan source" and the channel
 * setup use it to choose a program and the streams to keep.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "cc3.h"
#include "input.h"
#include "log.h"
#include "psi.h"
#include "udp.h"

#ifdef HAVE_AVCODEC
#include <libavcodec/avcodec.h>
#endif

#define MAX_PROBE_PROGRAMS 64
#define FILE_LIMIT (64u << 20)          /* a file: this much at most */
#define PROBE_SECTION_MAX 4096
#define PES_KEEP 65536                  /* of each PES, for the parsers */

struct pid_stat {
    uint64_t packets, bytes, cc_errors, scrambled, pes;
    int cc;                             /* last continuity counter, -1 = none */
    int64_t pts_min, pts_max;           /* 90 kHz, within the window; NOPTS = none */
    bool pusi_seen;
    /* section assembly (PAT, PMT, SDT) */
    uint8_t *sec;
    int sec_len, sec_want;
    /* PES assembly, the head of each */
    uint8_t *pes_buf;
    int pes_len;
    bool pes_open;
#ifdef HAVE_AVCODEC
    AVCodecParserContext *parser;
    AVCodecContext *actx;
    int parsed;                         /* PES units handed to the parser */
#endif
    /* what the parsers found */
    int width, height;
    int interlaced;                     /* -1 unknown, 0 no, 1 yes */
    int sample_rate, channels;
    const char *profile;
    bool is_adts;
    int64_t pcr_first, pcr_last;        /* 27 MHz, on this PID */
};

struct probe {
    struct pid_stat *pid[PID_COUNT];
    struct program progs[MAX_PROBE_PROGRAMS];
    bool pmt_seen[MAX_PROBE_PROGRAMS];
    int n_progs;
    uint16_t ts_id;
    bool pat_seen;
    uint64_t packets, bytes, sync_errors;
    int only_program;
};

static struct pid_stat *ps(struct probe *p, int pid)
{
    if (!p->pid[pid]) {
        p->pid[pid] = calloc(1, sizeof(struct pid_stat));
        if (p->pid[pid]) {
            p->pid[pid]->cc = -1;
            p->pid[pid]->pts_min = p->pid[pid]->pts_max = NOPTS;
            p->pid[pid]->interlaced = -1;
            p->pid[pid]->pcr_first = p->pid[pid]->pcr_last = -1;
        }
    }
    return p->pid[pid];
}

/* ---- tables --------------------------------------------------------- */

static int prog_index(struct probe *p, uint16_t number)
{
    for (int i = 0; i < p->n_progs; i++)
        if (p->progs[i].number == number)
            return i;
    return -1;
}

static void on_pat(struct probe *p, const uint8_t *sec, int len)
{
    uint16_t numbers[MAX_PROBE_PROGRAMS], pmts[MAX_PROBE_PROGRAMS];
    int n;

    if (!psi_section_ok(sec, len, 0x00))
        return;
    n = psi_pat_others(sec, len, -1, numbers, pmts, MAX_PROBE_PROGRAMS);
    p->ts_id = (uint16_t)((sec[3] << 8) | sec[4]);
    for (int i = 0; i < n; i++) {
        if (numbers[i] == 0)
            continue;                   /* the NIT */
        if (prog_index(p, numbers[i]) >= 0)
            continue;
        if (p->n_progs >= MAX_PROBE_PROGRAMS)
            break;
        memset(&p->progs[p->n_progs], 0, sizeof(p->progs[0]));
        p->progs[p->n_progs].number = numbers[i];
        p->progs[p->n_progs].pmt_pid = pmts[i];
        p->progs[p->n_progs].ts_id = p->ts_id;
        p->n_progs++;
    }
    p->pat_seen = true;
}

static void on_pmt(struct probe *p, int pid, const uint8_t *sec, int len)
{
    if (!psi_section_ok(sec, len, 0x02))
        return;
    for (int i = 0; i < p->n_progs; i++) {
        if (p->progs[i].pmt_pid != pid)
            continue;
        if (p->pmt_seen[i])
            return;
        {
            struct program tmp = p->progs[i];

            if (psi_parse_pmt(sec, len, &tmp) && tmp.number == p->progs[i].number) {
                p->progs[i] = tmp;
                p->pmt_seen[i] = true;
            }
        }
    }
}

static void on_sdt(struct probe *p, const uint8_t *sec, int len)
{
    if (!psi_section_ok(sec, len, 0x42))
        return;
    for (int i = 0; i < p->n_progs; i++)
        psi_parse_sdt(sec, len, &p->progs[i]);
}

static void section_done(struct probe *p, int pid, const uint8_t *sec, int len)
{
    if (pid == 0)
        on_pat(p, sec, len);
    else if (pid == 0x11)
        on_sdt(p, sec, len);
    else
        on_pmt(p, pid, sec, len);
}

static void sections_feed(struct probe *p, int pid, struct pid_stat *s, const uint8_t *d, int n, bool pusi)
{
    if (!s->sec && !(s->sec = malloc(PROBE_SECTION_MAX)))
        return;
    if (pusi) {
        int ptr = d[0];

        if (1 + ptr > n)
            return;
        /* the tail of the previous section first */
        if (s->sec_want > 0 && s->sec_len + ptr >= s->sec_want) {
            memcpy(s->sec + s->sec_len, d + 1, (size_t)(s->sec_want - s->sec_len));
            section_done(p, pid, s->sec, s->sec_want);
        }
        d += 1 + ptr;
        n -= 1 + ptr;
        s->sec_len = s->sec_want = 0;
        /* sections start here, possibly several */
        while (n >= 3 && d[0] != 0xFF) {
            int want = 3 + (((d[1] & 0x0F) << 8) | d[2]);

            if (want > PROBE_SECTION_MAX)
                return;
            if (want <= n) {
                section_done(p, pid, d, want);
                d += want;
                n -= want;
            } else {
                memcpy(s->sec, d, (size_t)n);
                s->sec_len = n;
                s->sec_want = want;
                return;
            }
        }
    } else if (s->sec_want > 0) {
        int take = s->sec_want - s->sec_len;

        if (take > n)
            take = n;
        memcpy(s->sec + s->sec_len, d, (size_t)take);
        s->sec_len += take;
        if (s->sec_len >= s->sec_want) {
            section_done(p, pid, s->sec, s->sec_want);
            s->sec_len = s->sec_want = 0;
        }
    }
}

/* ---- streams -------------------------------------------------------- */

enum { KIND_VIDEO, KIND_AUDIO, KIND_SUBTITLES, KIND_TELETEXT, KIND_DATA };

static int es_kind(const struct program *p, int idx)
{
    int st = p->es[idx].stream_type;

    switch (st) {
    case 0x01: case 0x02: case 0x10: case 0x1B: case 0x24:
        return KIND_VIDEO;
    case 0x03: case 0x04: case 0x0F: case 0x11: case 0x81: case 0x87:
        return KIND_AUDIO;
    case 0x06:
        if (program_es_has_desc(p, idx, 0x6A, NULL) || program_es_has_desc(p, idx, 0x7A, NULL) ||
            program_es_has_desc(p, idx, 0x05, "AC-3"))
            return KIND_AUDIO;
        if (program_es_has_desc(p, idx, 0x59, NULL))
            return KIND_SUBTITLES;
        if (program_es_has_desc(p, idx, 0x56, NULL) || program_es_has_desc(p, idx, 0x46, NULL))
            return KIND_TELETEXT;
        return KIND_DATA;
    default:
        return KIND_DATA;
    }
}

static const char *kind_name(int k)
{
    static const char *const names[] = { "video", "audio", "subtitles", "teletext", "data" };
    return names[k];
}

/* The codec name caricoder3 uses for a stream it can decode, else "" */
static const char *es_codec(const struct program *p, int idx)
{
    switch (p->es[idx].stream_type) {
    case 0x01: case 0x02: return "mpeg2";
    case 0x03: case 0x04: return "mpa";
    case 0x0F: return "aac";
    case 0x11: return "aac-latm";
    case 0x1B: return "h264";
    case 0x24: return "hevc";
    case 0x81: return "ac3";
    case 0x87: return "eac3";
    case 0x06:
        if (program_es_has_desc(p, idx, 0x7A, NULL))
            return "eac3";
        if (program_es_has_desc(p, idx, 0x6A, NULL) || program_es_has_desc(p, idx, 0x05, "AC-3"))
            return "ac3";
        return "";
    default: return "";
    }
}

/* The ISO 639 language descriptor (0x0A): first language */
static void es_language(const struct program *p, int idx, char *out, size_t len)
{
    const uint8_t *d = p->blob + p->es[idx].desc_off;
    int n = p->es[idx].desc_len;

    out[0] = 0;
    for (int i = 0; i + 2 <= n; ) {
        int tag = d[i], dl = d[i + 1];

        if (i + 2 + dl > n)
            break;
        if (tag == 0x0A && dl >= 3) {
            snprintf(out, len, "%.3s", (const char *)d + i + 2);
            return;
        }
        i += 2 + dl;
    }
}

#ifdef HAVE_AVCODEC
static enum AVCodecID av_id(const char *codec)
{
    if (!strcmp(codec, "h264")) return AV_CODEC_ID_H264;
    if (!strcmp(codec, "hevc")) return AV_CODEC_ID_HEVC;
    if (!strcmp(codec, "mpeg2")) return AV_CODEC_ID_MPEG2VIDEO;
    if (!strcmp(codec, "aac")) return AV_CODEC_ID_AAC;
    if (!strcmp(codec, "aac-latm")) return AV_CODEC_ID_AAC_LATM;
    if (!strcmp(codec, "mpa")) return AV_CODEC_ID_MP3;      /* the mpegaudio parser takes layers I-III */
    if (!strcmp(codec, "ac3")) return AV_CODEC_ID_AC3;
    if (!strcmp(codec, "eac3")) return AV_CODEC_ID_EAC3;
    return AV_CODEC_ID_NONE;
}

static void parser_open(struct pid_stat *s, const char *codec)
{
    enum AVCodecID id = av_id(codec);
    const AVCodec *c;

    if (id == AV_CODEC_ID_NONE || s->parser)
        return;
    av_log_set_level(AV_LOG_QUIET);
    c = avcodec_find_decoder(id);
    s->parser = av_parser_init(id);
    s->actx = c ? avcodec_alloc_context3(c) : NULL;
    if (!s->parser || !s->actx) {
        if (s->parser)
            av_parser_close(s->parser);
        avcodec_free_context(&s->actx);
        s->parser = NULL;
    }
}

static void parser_feed(struct pid_stat *s, const uint8_t *d, int n)
{
    uint8_t *out;
    int out_len;

    if (!s->parser || s->parsed > 400)
        return;
    while (n > 0) {
        int used = av_parser_parse2(s->parser, s->actx, &out, &out_len, d, n,
                                    AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (used < 0)
            break;
        d += used;
        n -= used;
        if (out_len > 0) {
            s->parsed++;
            if (s->parser->width > 0 && s->parser->height > 0) {
                s->width = s->parser->width;
                s->height = s->parser->height;
                s->interlaced = s->parser->field_order == AV_FIELD_UNKNOWN ? -1 :
                                s->parser->field_order != AV_FIELD_PROGRESSIVE;
            }
            if (s->actx->sample_rate > 0) {
                s->sample_rate = s->actx->sample_rate;
                s->channels = s->actx->ch_layout.nb_channels;
            }
            if (s->actx->profile != AV_PROFILE_UNKNOWN && !s->profile && s->actx->codec)
                s->profile = avcodec_profile_name(s->actx->codec_id, s->actx->profile);
        }
        if (used == 0)
            break;
    }
}
#endif

/* The PES header: PTS, and where the payload starts. */
static int pes_header(const uint8_t *d, int n, int64_t *pts)
{
    *pts = NOPTS;
    if (n < 9 || d[0] != 0 || d[1] != 0 || d[2] != 1)
        return -1;
    {
        int sid = d[3];
        /* streams without the optional header (padding, private_stream_2 ...) */
        if (sid == 0xBC || sid == 0xBE || sid == 0xBF || sid == 0xF0 || sid == 0xF1 ||
            sid == 0xFF || sid == 0xF2 || sid == 0xF8)
            return 6;
    }
    if ((d[6] & 0xC0) != 0x80)
        return -1;
    {
        int hl = 9 + d[8];

        if ((d[7] & 0x80) && n >= 14)
            *pts = ((int64_t)(d[9] & 0x0E) << 29) | ((int64_t)d[10] << 22) | ((int64_t)(d[11] & 0xFE) << 14) |
                   ((int64_t)d[12] << 7) | (d[13] >> 1);
        return hl;
    }
}

/* AAC in ADTS: libavcodec's parser leaves the rate to the decoder, so the
 * header is read here (syncword, then the sampling frequency index and
 * the channel configuration). */
static void adts_look(struct pid_stat *s, const uint8_t *d, int n)
{
    static const int rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
                                 16000, 12000, 11025, 8000, 7350, 0, 0, 0 };
    for (int i = 0; i + 7 <= n && i < 64; i++) {
        if (d[i] == 0xFF && (d[i + 1] & 0xF6) == 0xF0) {
            int sr = rates[(d[i + 2] >> 2) & 0x0F];
            int ch = ((d[i + 2] & 1) << 2) | (d[i + 3] >> 6);

            if (sr) {
                s->sample_rate = sr;
                s->channels = ch == 7 ? 8 : ch;
            }
            return;
        }
    }
}

static void pes_done(struct probe *p, int pid, struct pid_stat *s)
{
    int64_t pts;
    int off;

    (void)p; (void)pid;
    if (!s->pes_open || s->pes_len < 9)
        return;
    off = pes_header(s->pes_buf, s->pes_len, &pts);
    s->pes++;
    if (s->is_adts && off > 0 && off < s->pes_len && !s->sample_rate)
        adts_look(s, s->pes_buf + off, s->pes_len - off);
    if (pts != NOPTS) {
        /* the window's span; a wrap in it is unlikely in a few seconds */
        if (s->pts_min == NOPTS || pts < s->pts_min)
            s->pts_min = pts;
        if (s->pts_max == NOPTS || pts > s->pts_max)
            s->pts_max = pts;
    }
#ifdef HAVE_AVCODEC
    if (off > 0 && off < s->pes_len)
        parser_feed(s, s->pes_buf + off, s->pes_len - off);
#else
    (void)off;
#endif
    s->pes_open = false;
    s->pes_len = 0;
}

static void pes_feed(struct probe *p, int pid, struct pid_stat *s, const uint8_t *d, int n, bool pusi)
{
    if (!s->pes_buf && !(s->pes_buf = malloc(PES_KEEP)))
        return;
    if (pusi) {
        pes_done(p, pid, s);
        s->pes_open = true;
        s->pes_len = 0;                 /* also after a head too short to be a PES */
    }
    if (!s->pes_open)
        return;
    if (s->pes_len + n <= PES_KEEP) {
        memcpy(s->pes_buf + s->pes_len, d, (size_t)n);
        s->pes_len += n;
    }
}

/* ---- packets -------------------------------------------------------- */

static bool pid_is_pmt(const struct probe *p, int pid)
{
    for (int i = 0; i < p->n_progs; i++)
        if (p->progs[i].pmt_pid == pid)
            return true;
    return false;
}

/* The program and index a PID belongs to (the first program listing it). */
static bool pid_es(const struct probe *p, int pid, int *prog, int *idx)
{
    for (int i = 0; i < p->n_progs; i++)
        for (int k = 0; k < p->progs[i].n_es; k++)
            if (p->progs[i].es[k].pid == pid) {
                *prog = i;
                *idx = k;
                return true;
            }
    return false;
}

static void packet(struct probe *p, const uint8_t *pkt)
{
    int pid = ((pkt[1] & 0x1F) << 8) | pkt[2];
    bool pusi = pkt[1] & 0x40, scrambled = (pkt[3] & 0xC0) != 0;
    int afc = (pkt[3] >> 4) & 3, cc = pkt[3] & 0x0F;
    int off = 4;
    struct pid_stat *s;

    p->packets++;
    p->bytes += TS_SIZE;
    if (pkt[0] != 0x47) {
        p->sync_errors++;
        return;
    }
    if (pid == PID_NULL)
        return;
    s = ps(p, pid);
    if (!s)
        return;
    s->packets++;
    s->bytes += TS_SIZE;
    if (scrambled)
        s->scrambled++;
    if (afc & 2) {
        int al = pkt[4];

        off = 5 + al;
        if (al >= 7 && (pkt[5] & 0x10)) {       /* a PCR */
            int64_t base = ((int64_t)pkt[6] << 25) | ((int64_t)pkt[7] << 17) | ((int64_t)pkt[8] << 9) |
                           ((int64_t)pkt[9] << 1) | (pkt[10] >> 7);
            int64_t pcr = base * 300 + (((pkt[10] & 1) << 8) | pkt[11]);

            if (s->pcr_first < 0)
                s->pcr_first = pcr;
            s->pcr_last = pcr;
        }
    }
    if (afc & 1) {
        if (s->cc >= 0 && ((s->cc + 1) & 0x0F) != cc)
            s->cc_errors++;
        s->cc = cc;
    }
    if (!(afc & 1) || off >= TS_SIZE)
        return;
    if (scrambled)
        return;
    if (pid == 0 || pid == 0x11 || pid_is_pmt(p, pid)) {
        sections_feed(p, pid, s, pkt + off, TS_SIZE - off, pusi);
        return;
    }
    {
        int prog, idx;

        if (pid_es(p, pid, &prog, &idx)) {
            int k = es_kind(&p->progs[prog], idx);

            if (k == KIND_VIDEO || k == KIND_AUDIO) {
                s->is_adts = p->progs[prog].es[idx].stream_type == 0x0F;
#ifdef HAVE_AVCODEC
                parser_open(s, es_codec(&p->progs[prog], idx));
#endif
                pes_feed(p, pid, s, pkt + off, TS_SIZE - off, pusi);
            } else if (p->progs[prog].es[idx].kind != ES_SECTIONS && pusi) {
                s->pes++;
            }
        }
    }
}

static void feed(struct probe *p, const uint8_t *d, size_t n)
{
    /* RTP (RFC 2250) is skipped the way the engine does it */
    if (n >= 12 + TS_SIZE && (d[0] & 0xC0) == 0x80 && d[12] == 0x47) {
        size_t hdr = 12 + 4u * (d[0] & 0x0F);

        if ((d[0] & 0x10) && n >= hdr + 4)
            hdr += 4 + 4u * (((size_t)d[hdr + 2] << 8) | d[hdr + 3]);
        if (hdr < n) {
            d += hdr;
            n -= hdr;
        }
    }
    for (; n >= TS_SIZE; d += TS_SIZE, n -= TS_SIZE)
        packet(p, d);
}

/* ---- the report ----------------------------------------------------- */

static void jstr(FILE *f, const char *s)
{
    fputc('"', f);
    for (; s && *s; s++) {
        unsigned char ch = (unsigned char)*s;

        if (ch == '"' || ch == '\\')
            fprintf(f, "\\%c", ch);
        else if (ch < 0x20)
            fprintf(f, "\\u%04x", ch);
        else
            fputc(ch, f);
    }
    fputc('"', f);
}

static void report(struct probe *p, FILE *f, const char *url, double seconds, bool live)
{
    double span = seconds;              /* over which the bytes were counted */

    if (!live) {
        /* a file: the longest PCR span of any one PID (each program has
         * a clock of its own, so not across PIDs) */
        span = 0;
        for (int pid = 0; pid < PID_NULL; pid++) {
            const struct pid_stat *s = p->pid[pid];

            if (s && s->pcr_first >= 0 && (double)(s->pcr_last - s->pcr_first) / 27e6 > span)
                span = (double)(s->pcr_last - s->pcr_first) / 27e6;
        }
    }
    if (span <= 0)
        span = 1;

    fprintf(f, "{\n  \"ok\": true,\n  \"url\": ");
    jstr(f, url);
    fprintf(f, ",\n  \"seconds\": %.1f,\n  \"bitrate\": %.0f,\n  \"packets\": %llu,\n  \"sync_errors\": %llu,\n"
               "  \"ts_id\": %u,\n  \"programs\": [",
            span, (double)p->bytes * 8.0 / span, (unsigned long long)p->packets,
            (unsigned long long)p->sync_errors, p->ts_id);
    for (int i = 0, np = 0; i < p->n_progs; i++) {
        const struct program *pr = &p->progs[i];
        char name[SERVICE_TEXT_MAX + 1];
        uint64_t pbytes = 0;

        if (p->only_program && pr->number != p->only_program)
            continue;
        program_service_name(pr, name, sizeof(name));
        for (int k = 0; k < pr->n_es; k++)
            if (p->pid[pr->es[k].pid])
                pbytes += p->pid[pr->es[k].pid]->bytes;
        fprintf(f, "%s\n    {\n      \"number\": %u,\n      \"pmt_pid\": %u,\n      \"pcr_pid\": %u,\n"
                   "      \"name\": ", np++ ? "," : "", pr->number, pr->pmt_pid, pr->pcr_pid);
        jstr(f, name);
        fprintf(f, ",\n      \"pmt_seen\": %s,\n      \"scrambled\": %s,\n      \"bitrate\": %.0f,\n      \"streams\": [",
                p->pmt_seen[i] ? "true" : "false", pr->free_ca ? "true" : "false",
                (double)pbytes * 8.0 / span);
        for (int k = 0; k < pr->n_es; k++) {
            const struct pid_stat *s = p->pid[pr->es[k].pid];
            char lang[8];
            int kind = es_kind(pr, k);
            const char *codec = es_codec(pr, k);

            es_language(pr, k, lang, sizeof(lang));
            fprintf(f, "%s\n        {\"pid\": %u, \"type\": %u, \"type_name\": ", k ? "," : "",
                    pr->es[k].pid, pr->es[k].stream_type);
            jstr(f, stream_type_name(pr->es[k].stream_type));
            fprintf(f, ", \"kind\": \"%s\", \"codec\": \"%s\", \"language\": ", kind_name(kind), codec);
            jstr(f, lang);
            fprintf(f, ", \"bitrate\": %.0f, \"packets\": %llu, \"cc_errors\": %llu, \"scrambled\": %s",
                    s ? (double)s->bytes * 8.0 / span : 0.0, (unsigned long long)(s ? s->packets : 0),
                    (unsigned long long)(s ? s->cc_errors : 0), s && s->scrambled ? "true" : "false");
            if (s) {
                if (s->width > 0)
                    fprintf(f, ", \"width\": %d, \"height\": %d, \"interlaced\": %s", s->width, s->height,
                            s->interlaced < 0 ? "null" : s->interlaced ? "true" : "false");
                if (kind == KIND_VIDEO && s->pes >= 3 && s->pts_min != NOPTS && s->pts_max > s->pts_min) {
                    /* units per second of PTS span; snapped to a standard rate
                     * when within 3 % (the window's edges hold pictures whose
                     * neighbours in presentation order are outside it) */
                    static const double std_rates[] = { 23.976, 24, 25, 29.97, 30, 50, 59.94, 60, 0 };
                    double fps = (double)(s->pes - 1) * 90000.0 / (double)(s->pts_max - s->pts_min);

                    for (int r = 0; std_rates[r] > 0; r++)
                        if (fps > std_rates[r] * 0.97 && fps < std_rates[r] * 1.03 &&
                            !(std_rates[r] == 24 && fps < 23.99) && !(std_rates[r] == 30 && fps < 29.98) &&
                            !(std_rates[r] == 60 && fps < 59.96)) {
                            fps = std_rates[r];
                            break;
                        }
                    fprintf(f, ", \"fps\": %.3f", fps);
                }
                if (s->sample_rate > 0)
                    fprintf(f, ", \"sample_rate\": %d, \"channels\": %d", s->sample_rate, s->channels);
                if (s->profile)
                    fprintf(f, ", \"profile\": \"%s\"", s->profile);
            }
            fputc('}', f);
        }
        fprintf(f, "\n      ]\n    }");
    }
    fprintf(f, "\n  ],\n  \"other_pids\": [");
    {
        int n = 0;

        for (int pid = 1; pid < PID_NULL; pid++) {
            int prog, idx;

            if (!p->pid[pid] || pid == 0x11 || pid_is_pmt(p, pid) || pid_es(p, pid, &prog, &idx))
                continue;
            fprintf(f, "%s{\"pid\": %d, \"bitrate\": %.0f}", n++ ? ", " : "", pid,
                    (double)p->pid[pid]->bytes * 8.0 / span);
        }
    }
    fprintf(f, "]\n}\n");
}

/* ---- main ------------------------------------------------------------ */

static void usage(FILE *f)
{
    fprintf(f, "Usage: caricoder3 probe URL [--seconds N] [--program N]\n"
               "  URL       udp://, rtp://, srt://, rist://, http:// as for --input, or a .ts file\n"
               "  --seconds how long to listen (default 3)\n"
               "  --program report this service only\n"
               "Prints JSON: programs, streams, rates, picture and sound formats.\n");
}

static int fail(const char *fmt, const char *arg)
{
    char msg[300];

    snprintf(msg, sizeof(msg), fmt, arg);
    printf("{\n  \"ok\": false,\n  \"error\": ");
    jstr(stdout, msg);
    printf("\n}\n");
    return 1;
}

int probe_main(int argc, char **argv);
int probe_main(int argc, char **argv)
{
    struct probe *p;
    const char *url = NULL;
    double seconds = 3.0;
    int only = 0;
    char err[256];
    bool live;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = atof(argv[++i]);
        else if (!strncmp(argv[i], "--seconds=", 10))
            seconds = atof(argv[i] + 10);
        else if (!strcmp(argv[i], "--program") && i + 1 < argc)
            only = atoi(argv[++i]);
        else if (!strncmp(argv[i], "--program=", 10))
            only = atoi(argv[i] + 10);
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout);
            return 0;
        } else if (argv[i][0] == '-' || url) {
            usage(stderr);
            return 2;
        } else {
            url = argv[i];
        }
    }
    if (!url) {
        usage(stderr);
        return 2;
    }
    if (seconds < 0.5 || seconds > 120)
        return fail("--seconds must be 0.5..120", NULL);
    log_level = LOG_ERR;                /* the libraries' chatter stays out of the JSON */

    p = calloc(1, sizeof(*p));
    if (!p)
        return fail("out of memory", NULL);
    p->only_program = only;
    live = input_is_live(url);

    if (!live) {
        const char *path = !strncmp(url, "file:", 5) ? url + 5 : url;
        FILE *f = fopen(path, "rb");
        static uint8_t buf[65536];
        size_t n, total = 0;

        if (!f)
            return fail("%s", strerror(errno));
        /* sync: the first 0x47 that is followed by another 188 bytes on,
         * then whole packets from there */
        n = fread(buf, 1, sizeof(buf), f);
        {
            size_t start = 0;

            while (start + 2 * TS_SIZE < n && !(buf[start] == 0x47 && buf[start + TS_SIZE] == 0x47))
                start++;
            fseek(f, (long)start, SEEK_SET);
        }
        while (total < FILE_LIMIT && (n = fread(buf, 1, sizeof(buf) / TS_SIZE * TS_SIZE, f)) > 0) {
            feed(p, buf, n / TS_SIZE * TS_SIZE);
            total += n;
        }
        fclose(f);
    } else {
        const struct input_ops *ops = input_ops_for(url);
        void *ctx = NULL;
        struct udp_addr addr;
        int fd = -1;
        uint8_t *buf = malloc(65536);
        struct timespec t0, now;
        double elapsed = 0;

        if (!buf)
            return fail("out of memory", NULL);
        if (ops) {
            ctx = ops->open(url, err, sizeof(err));
            if (!ctx)
                return fail("%s", err);
        } else if (!strncmp(url, "srt://", 6) || !strncmp(url, "rist://", 7)) {
            return fail("this build has no %s input", !strncmp(url, "srt://", 6) ? "SRT" : "RIST");
        } else {
            if (udp_parse(url, &addr, err, sizeof(err)) < 0 || (fd = udp_open_rx(&addr, err, sizeof(err))) < 0)
                return fail("%s", err);
        }
        clock_gettime(CLOCK_MONOTONIC, &t0);
        /* A link input: the clock starts when the first datagram is in,
         * so connecting does not eat the window. */
        bool started = fd >= 0;

        while (elapsed < seconds) {
            int n;

            if (ops)
                n = ops->recv(ctx, buf, INPUT_DGRAM_MAX);
            else
                n = (int)recv(fd, buf, 65536, 0);
            if (n > 0) {
                if (!started) {
                    clock_gettime(CLOCK_MONOTONIC, &t0);
                    started = true;
                }
                feed(p, buf, (size_t)n);
            } else if (n < 0 && fd >= 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                break;
            } else if (n < 0 && ops) {
                break;
            }
            clock_gettime(CLOCK_MONOTONIC, &now);
            elapsed = (double)(now.tv_sec - t0.tv_sec) + (double)(now.tv_nsec - t0.tv_nsec) / 1e9;
            if (!started && elapsed > seconds + 10)
                break;              /* nothing ever came */
        }
        if (ops)
            ops->close(ctx);
        if (fd >= 0)
            close(fd);
        free(buf);
        if (p->packets == 0)
            return fail("nothing received from %s", url);
    }
    for (int pid = 0; pid < PID_COUNT; pid++)
        if (p->pid[pid])
            pes_done(p, pid, p->pid[pid]);
    if (!p->pat_seen)
        return fail("no PAT in what was received from %s", url);
    report(p, stdout, url, seconds, live);
    for (int pid = 0; pid < PID_COUNT; pid++) {
        struct pid_stat *s = p->pid[pid];

        if (!s)
            continue;
#ifdef HAVE_AVCODEC
        if (s->parser)
            av_parser_close(s->parser);
        avcodec_free_context(&s->actx);
#endif
        free(s->sec);
        free(s->pes_buf);
        free(s);
    }
    free(p);
    return 0;
}
