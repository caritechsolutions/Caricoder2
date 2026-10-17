/*
 * caricoder3 - command line, threads, statistics
 * Copyright (c) 2026 CariTech Solutions
 *
 * One process handles one channel. Threads in live running:
 *
 *   input    blocks in recvmmsg(), feeds the demux
 *   output   paces the mux into the packet ring
 *   one per  output (sink.c): from the ring to UDP, a file, SRT ...
 *   control  the Unix socket (control.c): outputs come and go here
 *   main     once a second: statistics file, log line, systemd watchdog
 */
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "cc3.h"
#include "clock.h"
#include "codec.h"
#include "control.h"
#include "engine.h"
#include "input.h"
#include "json.h"
#include "log.h"
#include "ring.h"
#include "sink.h"
#include "source.h"
#include "thumb.h"
#include "udp.h"

#define RX_SIZE         16384
#define OUTPUTS_MAX     SINKS_MAX
#define SOCK_DIR        "/run/caricoder3"
#define VBR_DEFAULT_RATE 40000000LL
#define DELAY_AUDIO_MS  800             /* default --delay when audio is transcoded */
#define DELAY_VIDEO_MS  3000            /* ... and when video is */

static atomic_int g_run = 1;             /* cleared by SIGINT / SIGTERM */
static atomic_int g_hup = 0;             /* SIGHUP: re-read the config file */

struct options {
    const char *input;                      /* the primary (inputs[0]) */
    const char *inputs[SOURCES_MAX];
    int n_input;
    int switch_ms, retry_s;                 /* failover: see source.h */
    const char *output[OUTPUTS_MAX];
    int n_output;
    const char *control, *name, *config, *run_dir;
    const char *stats_file;
    const char *thumb;                      /* JPEG of the picture, every few seconds */
    int thumb_interval;
    int stats_interval;
    int rt_prio;
    struct engine_cfg eng;
};

/* ---- the mux's packets go into the ring ------------------------------ */

static void ring_out(void *opaque, const uint8_t *pkt)
{
    ring_push(opaque, pkt);
}

/* A file run has no output threads: the sinks are written as the mux goes. */
static void sync_out(void *opaque, const uint8_t *pkt)
{
    sinks_write_sync(opaque, pkt);
}

/* ---- threads -------------------------------------------------------- */

struct ctx {
    struct options *opt;
    struct engine *eng;
    struct ring ring;
    struct sinks sinks;
    struct control *ctl;
    struct sources *src;                    /* live: the input(s) */
    pthread_mutex_t stats_lock;             /* the last statistics as JSON, for the control socket */
    char *stats_json;
    size_t stats_len;
    _Atomic int64_t beat_in, beat_out;      /* thread heartbeats, ns */
    _Atomic int64_t last_rx;                /* last datagram received, ns */
    _Atomic int fatal;
};

static void *input_thread(void *arg)
{
    struct ctx *c = arg;
    uint8_t *buf = malloc(RX_SIZE);
    int64_t last_tick = 0;

    if (!buf) {
        atomic_store(&c->fatal, 1);
        return NULL;
    }
    while (atomic_load(&g_run)) {
        int n = sources_recv(c->src, buf, RX_SIZE, clock_now_ns());
        int64_t now = clock_now_ns();

        atomic_store(&c->beat_in, now);
        if (n < 0) {
            log_err("input: finished");
            atomic_store(&c->fatal, 1);
            break;
        }
        if (n > 0) {
            engine_push_datagram(c->eng, buf, (size_t)n, now);
            atomic_store(&c->last_rx, now);
        }
        if (now - last_tick > 1000000000LL) {
            last_tick = now;
            sources_tick(c->src, now);
        }
    }
    free(buf);
    return NULL;
}

static void *output_thread(void *arg)
{
    struct ctx *c = arg;
    uint64_t head, published = 0;
    /* Wake up when asked, not up to 50 us later (the default slack). */
    prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);
    if (c->opt->rt_prio > 0) {
        struct sched_param sp = { .sched_priority = c->opt->rt_prio };

        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
            log_warn("output: could not get real-time priority %d "
                     "(needs CAP_SYS_NICE); continuing without", c->opt->rt_prio);
    }

    while (atomic_load(&g_run)) {
        int64_t now = clock_now_ns();
        bool flush;
        /* The wake-up aims at the packet that completes a datagram of
         * seven (what every UDP output sends unless its URL says otherwise). */
        int64_t wake = engine_run(c->eng, now, 7, (int)(ring_now(&c->ring) % 7), &flush);

        /* The outputs are woken once a datagram's worth is in (or at a
         * gap in a VBR stream, when a part-built datagram must go out). */
        head = ring_now(&c->ring);
        if (flush || head / 7 != published / 7) {
            ring_publish(&c->ring, flush);
            published = head;
        }
        atomic_store(&c->beat_out, now);

        now = clock_now_ns();
        if (wake > now) {
            struct timespec ts;

            if (wake > now + 20000000)      /* stay responsive to shutdown */
                wake = now + 20000000;
            ts.tv_sec = wake / 1000000000LL;
            ts.tv_nsec = wake % 1000000000LL;
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
        }
    }
    ring_publish(&c->ring, true);
    return NULL;
}

/* ---- systemd notification (no libsystemd needed) -------------------- */

static void sd_notify_msg(const char *msg)
{
    const char *path = getenv("NOTIFY_SOCKET");
    struct sockaddr_un sa;
    size_t len;
    int fd;

    if (!path || (path[0] != '/' && path[0] != '@'))
        return;
    len = strlen(path);
    if (len >= sizeof(sa.sun_path))
        return;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, path, len);
    if (path[0] == '@')
        sa.sun_path[0] = '\0';              /* abstract socket */
    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;
    sendto(fd, msg, strlen(msg), MSG_NOSIGNAL, (struct sockaddr *)&sa,
           (socklen_t)(offsetof(struct sockaddr_un, sun_path) + len));
    close(fd);
}

/* ---- statistics ----------------------------------------------------- */

static void json_str(FILE *f, const char *s)
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

/* What a stream is, for the GUI: video, audio, subtitles, teletext, data; and its language. */
static const char *es_kind(const struct program *p, int idx, char *lang, size_t langlen)
{
    int st = p->es[idx].stream_type;
    const uint8_t *d = p->blob + p->es[idx].desc_off;
    int n = p->es[idx].desc_len;

    lang[0] = 0;
    for (int i = 0; i + 2 <= n; ) {
        int tag = d[i], dl = d[i + 1];

        if (i + 2 + dl > n)
            break;
        if (tag == 0x0A && dl >= 3)
            snprintf(lang, langlen, "%.3s", (const char *)d + i + 2);
        i += 2 + dl;
    }
    switch (st) {
    case 0x01: case 0x02: case 0x10: case 0x1B: case 0x24: return "video";
    case 0x03: case 0x04: case 0x0F: case 0x11: case 0x81: case 0x87: return "audio";
    case 0x06:
        if (program_es_has_desc(p, idx, 0x6A, NULL) || program_es_has_desc(p, idx, 0x7A, NULL) ||
            program_es_has_desc(p, idx, 0x05, "AC-3"))
            return "audio";
        if (program_es_has_desc(p, idx, 0x59, NULL))
            return "subtitles";
        if (program_es_has_desc(p, idx, 0x56, NULL) || program_es_has_desc(p, idx, 0x46, NULL))
            return "teletext";
        return "data";
    default: return "data";
    }
}

struct rates {
    double in_bps, out_bps, null_pct;
    double uptime;
    int64_t input_age_ms;       /* -1 when not applicable */
    uint64_t rx_overflow, rx_truncated, send_errors;
    const struct engine_stats *prev;    /* the previous sample, for per-stream rates; NULL in a file run */
    double dt;
};

/* bits per second of a byte counter between two samples; -1 when unknown */
static double rate_of(uint64_t now, uint64_t was, double dt)
{
    return dt > 0 && now >= was ? (double)(now - was) * 8.0 / dt : -1;
}

static void stats_json(struct ctx *c, const struct engine_stats *st,
                       const struct rates *r, FILE *f)
{
    const struct options *o = c->opt;
    char url[SINK_URL_MAX];

    fprintf(f, "{\n  \"version\": \"%s\",\n  \"uptime_s\": %.1f,\n", CC3_VERSION, r->uptime);
    fprintf(f, "  \"state\": \"%s\",\n",
            !st->have_program ? "waiting_for_input" :
            !st->started ? "waiting_for_clock" : "running");

    struct source_stats ss;

    memset(&ss, 0, sizeof(ss));
    if (c->src)
        sources_stats(c->src, &ss);
    fprintf(f, "  \"input\": {\n    \"url\": ");
    json_str(f, c->src ? sources_url(c->src, ss.active) : o->input);
    if (c->src) {
        fprintf(f, ",\n    \"active\": %d,\n    \"switches\": %llu,\n    \"sources\": ",
                ss.active, (unsigned long long)ss.switches);
        sources_json(c->src, f, json_str);
    }
    fprintf(f, ",\n    \"bitrate\": %.0f,\n    \"last_packet_age_ms\": %lld,\n",
            r->in_bps, (long long)r->input_age_ms);
    fprintf(f, "    \"rtp\": %s,\n", st->rtp_datagrams ? "true" : "false");
    fprintf(f, "    \"packets\": %llu,\n    \"sync_errors\": %llu,\n"
               "    \"transport_errors\": %llu,\n    \"cc_errors\": %llu,\n"
               "    \"cc_errors_other_services\": %llu,\n    \"duplicates\": %llu,\n"
               "    \"scrambled\": %llu,\n    \"psi_errors\": %llu,\n"
               "    \"pes_units\": %llu,\n    \"pes_dropped\": %llu,\n"
               "    \"pes_invalid\": %llu,\n    \"pes_length_mismatch\": %llu,\n"
               "    \"sections\": %llu,\n    \"pcr_count\": %llu,\n"
               "    \"pcr_jumps\": %llu,\n    \"pcr_glitches\": %llu,\n"
               "    \"pcr_missing\": %llu,\n"
               "    \"timestamps_out_of_range\": %llu,\n"
               "    \"socket_overflow\": %llu,\n    \"truncated_datagrams\": %llu",
            (unsigned long long)st->in.packets, (unsigned long long)st->in.sync_errors,
            (unsigned long long)st->in.tei, (unsigned long long)st->in.cc_errors,
            (unsigned long long)st->in.cc_errors_other,
            (unsigned long long)st->in.duplicates, (unsigned long long)st->in.scrambled,
            (unsigned long long)st->in.psi_errors,
            (unsigned long long)st->in.pes_units, (unsigned long long)st->in.pes_dropped,
            (unsigned long long)st->in.pes_invalid,
            (unsigned long long)st->in.pes_len_mismatch,
            (unsigned long long)st->in.sections, (unsigned long long)st->in.pcr_count,
            (unsigned long long)st->in.pcr_jumps,
            (unsigned long long)st->in.pcr_glitches,
            (unsigned long long)st->in.pcr_missing,
            (unsigned long long)st->in.ts_out_of_range,
            (unsigned long long)r->rx_overflow, (unsigned long long)r->rx_truncated);
    if (ss.link) {
        fprintf(f, ",\n    \"link\": {\n      \"transport\": \"%s\",\n      \"state\": ", ss.link_transport);
        json_str(f, ss.link_state);
        fprintf(f, ",\n      \"connected\": %s,\n      \"rtt_ms\": %.1f,\n"
                   "      \"lost\": %lld,\n      \"recovered\": %lld,\n"
                   "      \"reconnects\": %llu,\n      \"errors\": %llu\n    }",
                ss.link_connected ? "true" : "false", ss.rtt_ms, (long long)ss.lost,
                (long long)ss.recovered, (unsigned long long)ss.reconnects,
                (unsigned long long)ss.link_errors);
    }
    fprintf(f, "\n  },\n");

    fprintf(f, "  \"service\": {\n    \"number\": %u,\n    \"ts_id\": %u,\n"
               "    \"pmt_pid\": %u,\n    \"pcr_pid\": %u,\n    \"changes\": %llu,\n"
               "    \"programs\": %d,\n"
               "    \"streams\": [",
            st->prog.number, st->prog.ts_id, st->prog.pmt_pid, st->prog.pcr_pid,
            (unsigned long long)st->in.program_changes, st->n_prog);
    /* One entry per input stream, and after it one per further rendition
     * of that stream (its own output PID). */
    int n_out = 0;

    for (int i = 0; st->have_program && i < st->prog.n_es; i++)
    for (int k = -1; k < st->n_xc; k++) {
        const struct xcode_stats *x = NULL;
        uint16_t in_pid = st->prog.es[i].pid;

        if (k >= 0) {
            if (st->xc[k].pid != in_pid || st->xc[k].out_pid == in_pid)
                continue;                   /* not this stream, or its first rendition */
            x = &st->xc[k];
        } else {
            for (int j = 0; j < st->n_xc; j++)
                if (st->xc[j].pid == in_pid && st->xc[j].out_pid == in_pid)
                    x = &st->xc[j];
        }
        {
            char lang[8];
            const char *kind = es_kind(&st->prog, i, lang, sizeof(lang));

            fprintf(f, "%s\n      {\"pid\": %u, \"in_pid\": %u, \"stream_type\": %u, \"kind\": \"%s\", \"language\": ",
                    n_out++ ? "," : "", x ? x->out_pid : in_pid, in_pid, st->prog.es[i].stream_type, kind);
            json_str(f, lang);
            fprintf(f, ", \"codec\": ");
        }
        json_str(f, stream_type_name(st->prog.es[i].stream_type));
        if (r->prev && r->prev->have_program) {
            /* the same stream in the previous sample */
            int pi = -1, pk = -1;

            for (int j = 0; j < r->prev->prog.n_es; j++)
                if (r->prev->prog.es[j].pid == in_pid)
                    pi = j;
            if (x)
                for (int j = 0; j < r->prev->n_xc; j++)
                    if (r->prev->xc[j].out_pid == x->out_pid)
                        pk = j;
            if (pi >= 0 && k < 0)
                fprintf(f, ", \"bitrate_in\": %.0f", rate_of(st->es_in_bytes[i], r->prev->es_in_bytes[pi], r->dt));
            if (!x && pi >= 0)
                fprintf(f, ", \"bitrate\": %.0f", rate_of(st->es_out_bytes[i], r->prev->es_out_bytes[pi], r->dt));
            else if (x && pk >= 0)
                fprintf(f, ", \"bitrate\": %.0f", rate_of(st->xc_out_bytes[k >= 0 ? k : (int)(x - st->xc)], r->prev->xc_out_bytes[pk], r->dt));
        }
        if (!x) {
            fprintf(f, ", \"mode\": \"copy\"}");
            continue;
        }
        if (x->out_pid != in_pid)
            fprintf(f, ", \"from_pid\": %u", in_pid);
        if (x->width > 0)
            fprintf(f, ", \"size\": \"%dx%d\"", x->width, x->height);
        fprintf(f, ", \"mode\": \"transcode\", \"from\": \"%s\", \"decoder\": \"%s\", "
                   "\"encoder\": \"%s\",\n       \"units_in\": %llu, \"units_out\": %llu, "
                   "\"frames_in\": %llu, \"frames_out\": %llu,\n       "
                   "\"decode_errors\": %llu, \"input_dropped\": %llu, "
                   "\"skipped_bytes\": %llu, \"silence_ms\": %llu, \"cut_ms\": %llu,\n       "
                   "\"restarts\": %llu, \"queued\": %u, \"failed\": %s",
                codec_name(x->in), x->decoder, x->encoder,
                (unsigned long long)x->units_in, (unsigned long long)x->units_out,
                (unsigned long long)x->frames_in, (unsigned long long)x->frames_out,
                (unsigned long long)x->errors, (unsigned long long)x->overflow,
                (unsigned long long)x->skipped_bytes, (unsigned long long)x->silence_ms,
                (unsigned long long)x->cut_ms, (unsigned long long)x->restarts,
                x->queued, x->failed ? "true" : "false");
        if (x->slack_min != INT64_MAX)
            fprintf(f, ", \"slack_ms\": %.1f", (double)x->slack_min / STC_PER_MS);
        if (x->slack_ever != INT64_MAX)
            fprintf(f, ", \"slack_min_ms\": %.1f", (double)x->slack_ever / STC_PER_MS);
        fprintf(f, "}");
    }
    fprintf(f, "\n    ]\n  },\n");

    fprintf(f, "  \"clock\": {\n    \"locked\": %s,\n    \"drift_ppm\": %.2f,\n"
               "    \"jitter_us\": %.1f,\n    \"ts_offset_ms\": %.3f,\n"
               "    \"discontinuities\": %llu,\n    \"held_units\": %u\n  },\n",
            st->clock_locked ? "true" : "false", st->clock_ppm, st->clock_jitter_us,
            (double)st->ts_offset / STC_PER_MS, (unsigned long long)st->discontinuities,
            st->held_units);

    fprintf(f, "  \"outputs\": ");
    sinks_json(&c->sinks, f, 4);
    fprintf(f, ",\n  \"output\": {\n    \"url\": ");
    json_str(f, sinks_first_url(&c->sinks, url, sizeof(url)));
    fprintf(f, ",\n    \"mode\": \"%s\",\n    \"muxrate\": %lld,\n    \"bitrate\": %.0f,\n"
               "    \"null_percent\": %.2f,\n    \"packets\": %llu,\n    \"nulls\": %llu,\n"
               "    \"psi\": %llu,\n    \"pcr_only\": %llu,\n    \"units\": %llu,\n"
               "    \"dropped_units\": %llu,\n    \"late_units\": %llu,\n"
               "    \"clock_skips\": %llu,\n"
               "    \"max_late_ms\": %.2f,\n    \"queued_units\": %u,\n"
               "    \"queued_bytes\": %llu,\n    \"send_errors\": %llu\n  }\n}\n",
            o->eng.mux.vbr ? "vbr" : "cbr", (long long)o->eng.mux.muxrate, r->out_bps,
            r->null_pct, (unsigned long long)st->out.packets,
            (unsigned long long)st->out.nulls, (unsigned long long)st->out.psi,
            (unsigned long long)st->out.pcr_only, (unsigned long long)st->out.units,
            (unsigned long long)st->out.dropped_units,
            (unsigned long long)st->out.late_units,
            (unsigned long long)st->out.skips,
            (double)st->out.max_late / STC_PER_MS, st->out.queued_units,
            (unsigned long long)st->out.queued_bytes, (unsigned long long)r->send_errors);
}

/* The statistics: kept for the control socket, written to --stats-file. */
static void write_stats(struct ctx *c, const struct engine_stats *st, const struct rates *r)
{
    const struct options *o = c->opt;
    char *text = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&text, &len);

    if (!f)
        return;
    stats_json(c, st, r, f);
    fclose(f);
    if (o->stats_file) {
        char tmp[1024];

        if (snprintf(tmp, sizeof(tmp), "%s.tmp", o->stats_file) < (int)sizeof(tmp) &&
            (f = fopen(tmp, "w")) != NULL) {
            fwrite(text, 1, len, f);
            if (fclose(f) == 0)
                rename(tmp, o->stats_file); /* readers never see half a file */
        }
    }
    pthread_mutex_lock(&c->stats_lock);
    free(c->stats_json);
    c->stats_json = text;
    c->stats_len = len;
    pthread_mutex_unlock(&c->stats_lock);
}

/* ---- running -------------------------------------------------------- */

static void on_signal(int sig)
{
    if (sig == SIGHUP)
        atomic_store(&g_hup, 1);
    else
        atomic_store(&g_run, 0);
}

/* ---- the config file: the outputs that survive a restart -------------- */

/* Reads --config and makes the outputs match it: ones not yet running
 * are added, saved ones no longer listed are removed. Outputs added at
 * run time without "save" are left alone. */
/* The config file is read by the main thread (SIGHUP) and the control
 * thread (reload), and written by the control thread: one at a time. */
static pthread_mutex_t config_lock = PTHREAD_MUTEX_INITIALIZER;

static bool config_load_locked(struct ctx *c, char *err, size_t errlen)
{
    struct json_doc *doc = malloc(sizeof(*doc));
    char *text = malloc(JSON_TEXT_MAX);
    FILE *f = fopen(c->opt->config, "r");
    size_t n;
    int root, outs, i;
    bool ok = true;
    const char *keep[OUTPUTS_MAX];
    int n_keep = 0;
    char id[SINK_ID_MAX], e[200];

    if (!doc || !text) {
        free(doc);
        free(text);
        if (f)
            fclose(f);
        snprintf(err, errlen, "out of memory");
        return false;
    }
    if (!f) {
        free(doc);
        free(text);
        if (errno == ENOENT) {
            log_info("config %s: not there yet", c->opt->config);
            return true;
        }
        snprintf(err, errlen, "%s: %s", c->opt->config, strerror(errno));
        return false;
    }
    n = fread(text, 1, JSON_TEXT_MAX - 1, f);
    fclose(f);
    text[n] = 0;
    root = json_parse(doc, text, err, errlen);
    if (root < 0) {
        free(doc);
        free(text);
        return false;
    }
    outs = json_get(doc, root, "outputs");
    if (outs < 0 || doc->node[outs].type != JSON_ARRAY) {
        snprintf(err, errlen, "%s: no \"outputs\" array", c->opt->config);
        free(doc);
        free(text);
        return false;
    }
    for (i = doc->node[outs].first_child; i >= 0; i = doc->node[i].next) {
        const char *oid = json_get_str(doc, i, "id", NULL);
        const char *url = json_get_str(doc, i, "url", NULL);

        if (!oid || !url) {
            log_warn("config: an output without \"id\" or \"url\" is skipped");
            ok = false;
            continue;
        }
        if (n_keep < OUTPUTS_MAX)
            keep[n_keep++] = oid;
        if (sinks_has(&c->sinks, oid))
            continue;                       /* already running */
        if (!sinks_add(&c->sinks, oid, url, true, e, sizeof(e))) {
            log_err("config: output %s: %s", oid, e);
            ok = false;
        }
    }
    /* Saved outputs that the file no longer lists go away. */
    while (sinks_saved_not_in(&c->sinks, keep, n_keep, id, sizeof(id)))
        sinks_remove(&c->sinks, id, e, sizeof(e));
    if (!ok)
        snprintf(err, errlen, "some outputs in %s could not be started", c->opt->config);
    free(doc);
    free(text);
    return ok;
}

static bool config_load(struct ctx *c, char *err, size_t errlen)
{
    bool ok;

    pthread_mutex_lock(&config_lock);
    ok = config_load_locked(c, err, errlen);
    pthread_mutex_unlock(&config_lock);
    return ok;
}

/* Writes the saved outputs back, atomically. */
static bool config_save(struct ctx *c, char *err, size_t errlen)
{
    char tmp[1024];
    FILE *f;
    bool ok = false;

    if (!c->opt->config) {
        snprintf(err, errlen, "no --config file to save to");
        return false;
    }
    pthread_mutex_lock(&config_lock);
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", c->opt->config) >= (int)sizeof(tmp) ||
        !(f = fopen(tmp, "w"))) {
        snprintf(err, errlen, "%s: %s", c->opt->config, strerror(errno));
    } else {
        fprintf(f, "{\n  \"outputs\": ");
        sinks_json_saved(&c->sinks, f, 4);
        fprintf(f, "\n}\n");
        if (fclose(f) != 0 || rename(tmp, c->opt->config) != 0) {
            snprintf(err, errlen, "%s: %s", c->opt->config, strerror(errno));
            unlink(tmp);
        } else {
            ok = true;
        }
    }
    pthread_mutex_unlock(&config_lock);
    return ok;
}

/* ---- the control socket ------------------------------------------------ */

static void reply_error(FILE *f, const char *msg)
{
    char q[600];

    fprintf(f, "{\"ok\": false, \"error\": %s}", json_quote(msg, q, sizeof(q)));
}

static void on_control(void *opaque, const struct json_doc *req, FILE *f)
{
    struct ctx *c = opaque;
    const char *cmd = json_get_str(req, 0, "cmd", "");
    char err[300];

    if (!strcmp(cmd, "list-outputs")) {
        fprintf(f, "{\"ok\": true, \"outputs\": ");
        sinks_json(&c->sinks, f, 2);
        fprintf(f, "}");
    } else if (!strcmp(cmd, "add-output")) {
        const char *url = json_get_str(req, 0, "url", NULL);
        const char *id = json_get_str(req, 0, "id", "");
        bool save = json_get_bool(req, 0, "save", false);
        char made[SINK_ID_MAX], q[SINK_ID_MAX * 2 + 2];

        if (!url) {
            reply_error(f, "add-output needs \"url\"");
            return;
        }
        if (!strstr(url, "://")) {
            reply_error(f, "a URL with a scheme is expected here (file:///path for a file)");
            return;
        }
        if (save && !c->opt->config) {
            reply_error(f, "cannot save: the channel runs without --config");
            return;
        }
        if (!sinks_add(&c->sinks, id, url, save, err, sizeof(err))) {
            reply_error(f, err);
            return;
        }
        sinks_last_id(&c->sinks, made, sizeof(made));
        if (save && !config_save(c, err, sizeof(err)))
            log_err("config: %s", err);
        fprintf(f, "{\"ok\": true, \"id\": %s}", json_quote(made, q, sizeof(q)));
    } else if (!strcmp(cmd, "remove-output")) {
        const char *id = json_get_str(req, 0, "id", NULL);
        bool saved;

        if (!id) {
            reply_error(f, "remove-output needs \"id\"");
            return;
        }
        saved = sinks_is_saved(&c->sinks, id);
        if (!sinks_remove(&c->sinks, id, err, sizeof(err))) {
            reply_error(f, err);
            return;
        }
        if (saved && c->opt->config && !config_save(c, err, sizeof(err)))
            log_err("config: %s", err);
        fprintf(f, "{\"ok\": true}");
    } else if (!strcmp(cmd, "stats")) {
        pthread_mutex_lock(&c->stats_lock);
        if (c->stats_json) {
            fprintf(f, "{\"ok\": true, \"stats\": ");
            fwrite(c->stats_json, 1, c->stats_len, f);
            fprintf(f, "}");
        } else {
            reply_error(f, "no statistics yet");
        }
        pthread_mutex_unlock(&c->stats_lock);
    } else if (!strcmp(cmd, "reload")) {
        if (c->opt->config && !config_load(c, err, sizeof(err)))
            reply_error(f, err);
        else
            fprintf(f, "{\"ok\": true}");
    } else {
        reply_error(f, "unknown cmd (list-outputs, add-output, remove-output, stats, reload)");
    }
}

/* `caricoder3 ctl SOCKET|NAME cmd [args]`: the command-line client. */
int super_main(int argc, char **argv);      /* super.c */
int probe_main(int argc, char **argv);      /* probe.c */

static int ctl_main(int argc, char **argv)
{
    char path[256], req[2048], q[1200];
    const char *where = argc > 2 ? argv[2] : NULL;
    const char *cmd = argc > 3 ? argv[3] : NULL;

    if (!where || !cmd || !strcmp(where, "-h") || !strcmp(where, "--help")) {
        fprintf(stderr,
"Usage: caricoder3 ctl SOCKET|NAME COMMAND\n"
"  list-outputs\n"
"  add-output URL [ID] [--save]    (--save: into the channel's config file)\n"
"  remove-output ID\n"
"  stats\n"
"  reload                          (re-read the config file)\n"
"NAME stands for " SOCK_DIR "/NAME.sock\n");
        return 2;
    }
    if (strchr(where, '/'))
        snprintf(path, sizeof(path), "%s", where);
    else
        snprintf(path, sizeof(path), SOCK_DIR "/%s.sock", where);
    if (!strcmp(cmd, "add-output")) {
        const char *id = "";
        bool save = false;
        size_t at;

        if (argc < 5) {
            fprintf(stderr, "add-output needs a URL\n");
            return 2;
        }
        for (int i = 5; i < argc; i++) {
            if (!strcmp(argv[i], "--save"))
                save = true;
            else
                id = argv[i];
        }
        snprintf(req, sizeof(req), "{\"cmd\": \"add-output\", \"url\": %s, ",
                 json_quote(argv[4], q, sizeof(q)));
        at = strlen(req);
        snprintf(req + at, sizeof(req) - at, "\"id\": %s, \"save\": %s}",
                 json_quote(id, q, sizeof(q)), save ? "true" : "false");
    } else if (!strcmp(cmd, "remove-output")) {
        if (argc < 5) {
            fprintf(stderr, "remove-output needs an ID\n");
            return 2;
        }
        snprintf(req, sizeof(req), "{\"cmd\": \"remove-output\", \"id\": %s}",
                 json_quote(argv[4], q, sizeof(q)));
    } else {
        snprintf(req, sizeof(req), "{\"cmd\": %s}", json_quote(cmd, q, sizeof(q)));
    }
    return control_client(path, req, stdout);
}

static int run_live(struct options *o, struct ctx *c)
{
    pthread_t tin, tout;
    struct engine_stats st, prev;
    struct rates r;
    int64_t t_start = clock_now_ns(), t_prev = t_start;
    int since_log = 0;

    memset(&prev, 0, sizeof(prev));
    atomic_store(&c->beat_in, t_start);
    atomic_store(&c->beat_out, t_start);

    if (pthread_create(&tin, NULL, input_thread, c) != 0) {
        log_err("cannot start the input thread");
        return 1;
    }
    if (pthread_create(&tout, NULL, output_thread, c) != 0) {
        log_err("cannot start the output thread");
        atomic_store(&g_run, 0);
        pthread_join(tin, NULL);            /* it must not outlive the engine */
        return 1;
    }
    sd_notify_msg("READY=1");

    while (atomic_load(&g_run) && !atomic_load(&c->fatal)) {
        for (int i = 0; i < 10 && atomic_load(&g_run); i++)
            usleep(100000);

        int64_t now = clock_now_ns();
        double dt = (double)(now - t_prev) / 1e9;
        int64_t last_rx = atomic_load(&c->last_rx);

        engine_get_stats(c->eng, &st);
        memset(&r, 0, sizeof(r));
        r.uptime = (double)(now - t_start) / 1e9;
        r.in_bps = (double)(st.in_bytes - prev.in_bytes) * 8.0 / dt;
        r.out_bps = (double)(st.out.packets - prev.out.packets) * TS_SIZE * 8.0 / dt;
        if (st.out.packets > prev.out.packets)
            r.null_pct = 100.0 * (double)(st.out.nulls - prev.out.nulls) /
                         (double)(st.out.packets - prev.out.packets);
        r.input_age_ms = last_rx ? (now - last_rx) / 1000000 : -1;
        r.prev = &prev;
        r.dt = dt;
        {
            struct source_stats ss;

            sources_stats(c->src, &ss);
            r.rx_overflow = ss.rx_overflow;
            r.rx_truncated = ss.rx_truncated;
        }
        r.send_errors = sinks_errors(&c->sinks);
        write_stats(c, &st, &r);

        if (atomic_exchange(&g_hup, 0) && o->config) {
            char e[300];

            log_info("config: re-reading %s", o->config);
            if (!config_load(c, e, sizeof(e)))
                log_err("config: %s", e);
        }

        if (o->stats_interval > 0 && ++since_log >= o->stats_interval) {
            since_log = 0;
            log_info("in %.2f Mb/s cc=%llu drop=%llu | out %.2f Mb/s null %.1f%% "
                     "late %.1f ms queue %u | clock %+.1f ppm jitter %.0f us disc=%llu",
                     r.in_bps / 1e6, (unsigned long long)st.in.cc_errors,
                     (unsigned long long)(st.in.pes_dropped + st.out.dropped_units),
                     r.out_bps / 1e6, r.null_pct,
                     (double)st.out.max_late / STC_PER_MS, st.out.queued_units,
                     st.clock_ppm, st.clock_jitter_us,
                     (unsigned long long)st.discontinuities);
        }

        /* Tell systemd we are alive only if both workers really are. */
        if (now - atomic_load(&c->beat_in) < 2000000000LL &&
            now - atomic_load(&c->beat_out) < 2000000000LL)
            sd_notify_msg("WATCHDOG=1");

        prev = st;
        t_prev = now;
    }

    atomic_store(&g_run, 0);
    sd_notify_msg("STOPPING=1");
    pthread_join(tin, NULL);
    pthread_join(tout, NULL);
    sinks_finish(&c->sinks);                /* what is still in the ring goes out */
    return atomic_load(&c->fatal) ? 1 : 0;
}

static int run_file(struct options *o, struct ctx *c, FILE *in)
{
    static uint8_t buf[65536];
    struct engine_stats st;
    struct rates r;
    size_t n;

    while (atomic_load(&g_run) && (n = fread(buf, 1, sizeof(buf), in)) > 0)
        engine_push_stream(c->eng, buf, n);
    engine_finish(c->eng);
    sinks_finish(&c->sinks);

    engine_get_stats(c->eng, &st);
    memset(&r, 0, sizeof(r));
    r.input_age_ms = -1;
    r.send_errors = sinks_errors(&c->sinks);
    if (st.out.packets)
        r.null_pct = 100.0 * (double)st.out.nulls / (double)st.out.packets;
    write_stats(c, &st, &r);
    (void)o;

    log_info("input : %llu packets, cc errors %llu, sync errors %llu, "
             "PES %llu (dropped %llu, invalid %llu), sections %llu",
             (unsigned long long)st.in.packets, (unsigned long long)st.in.cc_errors,
             (unsigned long long)st.in.sync_errors, (unsigned long long)st.in.pes_units,
             (unsigned long long)st.in.pes_dropped, (unsigned long long)st.in.pes_invalid,
             (unsigned long long)st.in.sections);
    log_info("output: %llu packets (%llu null, %llu PSI, %llu PCR-only), "
             "units %llu, dropped %llu, late %llu, discontinuities %llu",
             (unsigned long long)st.out.packets, (unsigned long long)st.out.nulls,
             (unsigned long long)st.out.psi, (unsigned long long)st.out.pcr_only,
             (unsigned long long)st.out.units, (unsigned long long)st.out.dropped_units,
             (unsigned long long)st.out.late_units,
             (unsigned long long)st.discontinuities);

    if (!st.have_program) {
        log_err("no service found in the input");
        return 2;
    }
    return r.send_errors ? 1 : 0;
}

/* ---- command line --------------------------------------------------- */

static void usage(FILE *f)
{
    fprintf(f,
"caricoder3 %s - MPEG-TS to MPEG-TS live processor\n"
"\n"
"Usage: caricoder3 -i INPUT -o OUTPUT [-o OUTPUT ...] [options]\n"
"       caricoder3 ctl SOCKET|NAME COMMAND ...   (outputs at run time; ctl -h)\n"
"       caricoder3 serve [--config PATH]          (the supervisor, API and GUI; serve -h)\n"
"       caricoder3 probe URL [--seconds N]        (what is in a source, as JSON; probe -h)\n"
"\n"
"  -i, --input URL          udp://[source@]group:port[?iface=eth0&buf=BYTES]\n"
"                           rtp://... (RTP is also detected automatically)\n"
"                           srt://host:port[?latency=120&passphrase=..] (caller)\n"
"                           srt://:port (listener), rist://@:port (listener),\n"
"                           rist://host:port (caller), or a .ts file.\n"
"                           Given more than once: the first is used, the others\n"
"                           stand by and take over when it delivers nothing\n"
"      --switch-after MS    ... for this long (default 3000); the primary is tried\n"
"      --retry-primary S    again every S seconds (default 60) and taken back\n"
"  -o, --output URL         udp://host:port[?ttl=8&iface=eth0&pkt=7] or a file;\n"
"                           may be repeated. Each output runs on its own: a slow\n"
"                           one loses packets, the others do not notice\n"
"      --control PATH       Unix socket for adding and removing outputs while\n"
"                           the channel runs (caricoder3 ctl PATH ...)\n"
"      --name NAME          the same, at " SOCK_DIR "/NAME.sock (--run-dir DIR changes the directory)\n"
"      --config FILE        JSON file of outputs: read at start and on SIGHUP,\n"
"                           written when one is added with --save\n"
"  -p, --program N          service to take from the input (default: first)\n"
"  -r, --muxrate RATE       constant bit rate output, e.g. 5M, 4500k, 5000000\n"
"      --vbr                variable bit rate: no null packets. This is the\n"
"                           default when --muxrate is not given; with\n"
"                           --muxrate it sets the peak rate\n"
"  -d, --delay MS           input-to-output buffer. Default 500; 800 when audio\n"
"                           is transcoded, 3000 when video is\n"
"      --drop PID[,PID..]   leave these streams of the service out of the output\n"
"                           (an audio track not wanted, say; caricoder3 probe lists them)\n"
"      --audio SPEC         transcode audio: aac, mp2 or mp3, with an optional\n"
"                           bit rate (aac:128k). PID=SPEC for one stream only\n"
"                           (1653=aac:96k); may be repeated. Default: copy\n"
"      --video SPEC         transcode video: h264 or hevc with a bit rate\n"
"                           (h264:3M); WxH in front makes a rendition at that\n"
"                           size (1280x720:h264:2M); may be repeated: each is\n"
"                           a program of the output. Default: copy\n"
"      --decoder WHERE      video decoding on: auto, nvidia or cpu (default auto)\n"
"      --encoder WHERE      video encoding on: auto, nvidia or cpu (default auto)\n"
"      --gpu N              which NVIDIA card (default 0)\n"
"      --preset NAME        encoder speed, in the encoder's own words\n"
"                           (x264/x265: ultrafast ... medium; nvenc: p1 ... p7)\n"
"      --gop MS             longest gap between key frames (default 2000)\n"
"      --vbv MS             video decoder buffer as time (default 1000)\n"
"      --pcr-interval MS    default 30\n"
"      --psi-interval MS    PAT/PMT repetition (default 100)\n"
"      --sdt-interval MS    default 500\n"
"      --service-name TEXT  replace the service name in the SDT\n"
"      --service-provider TEXT\n"
"      --stats-file PATH    JSON statistics, rewritten every second\n"
"      --thumb PATH         a JPEG of the picture, 320 wide, rewritten every\n"
"      --thumb-interval S   S seconds (default 5)\n"
"      --stats-interval S   log a status line every S seconds (default 10, 0 off)\n"
"      --rt-prio N          SCHED_FIFO priority for the output thread\n"
"      --backends           list codec backends and exit\n"
"      --gpu-test           try the NVIDIA decoder and encoder on made-up pictures\n"
"                           (card chosen with --gpu) and report each step\n"
"  -v, --verbose            more logging\n"
"  -q, --quiet              errors and warnings only\n"
"  -V, --version\n"
"  -h, --help\n"
"\n"
"A UDP input runs live, paced by the clock recovered from the input PCR.\n"
"A file input with a file output runs as fast as the disk allows and is\n"
"fully deterministic, which is what the tests use.\n",
        CC3_VERSION);
}

static bool parse_rate(const char *s, int64_t *out)
{
    char *end;
    double v = strtod(s, &end);

    if (end == s || v <= 0)
        return false;
    if (*end == 'k' || *end == 'K') {
        v *= 1e3;
        end++;
    } else if (*end == 'm' || *end == 'M') {
        v *= 1e6;
        end++;
    }
    if (*end)
        return false;
    *out = (int64_t)(v + 0.5);
    return true;
}

static const char *file_path(const char *s)
{
    return !strncmp(s, "file:", 5) ? s + 5 : s;
}

enum {
    OPT_VBR = 1000, OPT_PCR, OPT_PSI, OPT_SDT, OPT_NAME, OPT_PROVIDER,
    OPT_STATS_FILE, OPT_STATS_INT, OPT_RT, OPT_BACKENDS,
    OPT_AUDIO, OPT_VIDEO, OPT_DECODER, OPT_ENCODER, OPT_GPU, OPT_PRESET, OPT_DROP,
    OPT_SWITCH_AFTER, OPT_RETRY_PRIMARY, OPT_THUMB, OPT_THUMB_INTERVAL,
    OPT_GOP, OPT_VBV, OPT_GPU_TEST, OPT_CONTROL, OPT_NAME_SOCK, OPT_CONFIG, OPT_RUN_DIR,
};

static bool parse_where(const char *s, enum codec_where *out)
{
    if (!strcmp(s, "auto"))
        *out = WHERE_AUTO;
    else if (!strcmp(s, "cpu"))
        *out = WHERE_CPU;
    else if (!strcmp(s, "nvidia") || !strcmp(s, "gpu"))
        *out = WHERE_NVIDIA;
    else
        return false;
    return true;
}

int main(int argc, char **argv)
{
    static const struct option longopts[] = {
        { "input", required_argument, 0, 'i' },
        { "output", required_argument, 0, 'o' },
        { "program", required_argument, 0, 'p' },
        { "muxrate", required_argument, 0, 'r' },
        { "vbr", no_argument, 0, OPT_VBR },
        { "delay", required_argument, 0, 'd' },
        { "pcr-interval", required_argument, 0, OPT_PCR },
        { "psi-interval", required_argument, 0, OPT_PSI },
        { "sdt-interval", required_argument, 0, OPT_SDT },
        { "service-name", required_argument, 0, OPT_NAME },
        { "service-provider", required_argument, 0, OPT_PROVIDER },
        { "stats-file", required_argument, 0, OPT_STATS_FILE },
        { "stats-interval", required_argument, 0, OPT_STATS_INT },
        { "rt-prio", required_argument, 0, OPT_RT },
        { "backends", no_argument, 0, OPT_BACKENDS },
        { "audio", required_argument, 0, OPT_AUDIO },
        { "drop", required_argument, 0, OPT_DROP },
        { "switch-after", required_argument, 0, OPT_SWITCH_AFTER },
        { "thumb", required_argument, 0, OPT_THUMB },
        { "thumb-interval", required_argument, 0, OPT_THUMB_INTERVAL },
        { "retry-primary", required_argument, 0, OPT_RETRY_PRIMARY },
        { "video", required_argument, 0, OPT_VIDEO },
        { "decoder", required_argument, 0, OPT_DECODER },
        { "encoder", required_argument, 0, OPT_ENCODER },
        { "gpu", required_argument, 0, OPT_GPU },
        { "preset", required_argument, 0, OPT_PRESET },
        { "gop", required_argument, 0, OPT_GOP },
        { "vbv", required_argument, 0, OPT_VBV },
        { "gpu-test", no_argument, 0, OPT_GPU_TEST },
        { "control", required_argument, 0, OPT_CONTROL },
        { "name", required_argument, 0, OPT_NAME_SOCK },
        { "config", required_argument, 0, OPT_CONFIG },
        { "run-dir", required_argument, 0, OPT_RUN_DIR },
        { "verbose", no_argument, 0, 'v' },
        { "quiet", no_argument, 0, 'q' },
        { "version", no_argument, 0, 'V' },
        { "help", no_argument, 0, 'h' },
        { 0, 0, 0, 0 }
    };
    struct options o;
    struct ctx c;
    char sock_path[256];
    struct sigaction sa;
    char err[256];
    bool vbr_flag = false, delay_given = false, gpu_test = false;
    FILE *in_fp = NULL;
    int opt, rc;

    if (argc > 1 && !strcmp(argv[1], "ctl"))
        return ctl_main(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "serve"))
        return super_main(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "probe"))
        return probe_main(argc, argv);

    memset(&o, 0, sizeof(o));
    memset(&c, 0, sizeof(c));
    o.stats_interval = 10;
    o.eng.delay_ms = 500;
    o.eng.mux.pcr_interval_ms = 30;
    o.eng.mux.psi_interval_ms = 100;
    o.eng.mux.sdt_interval_ms = 500;
    o.eng.mux.max_late_ms = 1000;

    while ((opt = getopt_long(argc, argv, "i:o:p:r:d:vqVh", longopts, NULL)) != -1) {
        switch (opt) {
        case 'i':
            if (o.n_input >= SOURCES_MAX) {
                fprintf(stderr, "at most %d inputs\n", SOURCES_MAX);
                return 2;
            }
            o.inputs[o.n_input++] = optarg;
            o.input = o.inputs[0];
            break;
        case OPT_SWITCH_AFTER: o.switch_ms = atoi(optarg); break;
        case OPT_THUMB: o.thumb = optarg; break;
        case OPT_THUMB_INTERVAL: o.thumb_interval = atoi(optarg); break;
        case OPT_RETRY_PRIMARY: o.retry_s = atoi(optarg); break;
        case 'o':
            if (o.n_output == OUTPUTS_MAX) {
                fprintf(stderr, "no more than %d outputs\n", OUTPUTS_MAX);
                return 2;
            }
            o.output[o.n_output++] = optarg;
            break;
        case OPT_CONTROL: o.control = optarg; break;
        case OPT_NAME_SOCK: o.name = optarg; break;
        case OPT_CONFIG: o.config = optarg; break;
        case OPT_RUN_DIR: o.run_dir = optarg; break;
        case 'p': o.eng.program = atoi(optarg); break;
        case 'r':
            if (!parse_rate(optarg, &o.eng.mux.muxrate)) {
                fprintf(stderr, "bad --muxrate '%s'\n", optarg);
                return 2;
            }
            break;
        case OPT_VBR: vbr_flag = true; break;
        case 'd': o.eng.delay_ms = atoi(optarg); delay_given = true; break;
        case OPT_PCR: o.eng.mux.pcr_interval_ms = atoi(optarg); break;
        case OPT_PSI: o.eng.mux.psi_interval_ms = atoi(optarg); break;
        case OPT_SDT: o.eng.mux.sdt_interval_ms = atoi(optarg); break;
        case OPT_NAME: o.eng.service_name = optarg; break;
        case OPT_PROVIDER: o.eng.service_provider = optarg; break;
        case OPT_STATS_FILE: o.stats_file = optarg; break;
        case OPT_STATS_INT: o.stats_interval = atoi(optarg); break;
        case OPT_RT: o.rt_prio = atoi(optarg); break;
        case OPT_BACKENDS:
            codec_list(stdout);
            printf("Input types: %s, or a .ts file\n", input_transports());
            sink_list(stdout);
            return 0;
        case OPT_AUDIO:
        case OPT_VIDEO: {
            bool v = opt == OPT_VIDEO;
            int max = v ? MAX_PROGRAMS : 8;
            int *n = v ? &o.eng.n_video : &o.eng.n_audio;
            struct xcode_spec *list = v ? o.eng.video : o.eng.audio;

            if (*n >= max) {
                fprintf(stderr, "too many --%s options\n", v ? "video" : "audio");
                return 2;
            }
            if (!xcode_parse_spec(optarg, v, &list[*n], err, sizeof(err))) {
                fprintf(stderr, "--%s %s\n", v ? "video" : "audio", err);
                return 2;
            }
            (*n)++;
            break;
        }
        case OPT_DECODER:
        case OPT_ENCODER:
            if (!parse_where(optarg, opt == OPT_DECODER ? &o.eng.decoder : &o.eng.encoder)) {
                fprintf(stderr, "--%s must be auto, nvidia or cpu\n",
                        opt == OPT_DECODER ? "decoder" : "encoder");
                return 2;
            }
            break;
        case OPT_DROP: {
            /* PID[,PID...], decimal or 0x hex; may be repeated */
            for (const char *p = optarg; *p; ) {
                char *end;
                long pid = strtol(p, &end, 0);

                if (end == p || pid < 0x10 || pid > 0x1FFE || (*end && *end != ',')) {
                    fprintf(stderr, "--drop: PIDs between 0x10 and 0x1FFE, separated by commas\n");
                    return 2;
                }
                if (o.eng.n_drop >= MAX_ES) {
                    fprintf(stderr, "--drop: too many PIDs\n");
                    return 2;
                }
                o.eng.drop[o.eng.n_drop++] = (uint16_t)pid;
                p = *end == ',' ? end + 1 : end;
            }
            break;
        }
        case OPT_GPU: o.eng.gpu = atoi(optarg); break;
        case OPT_PRESET: o.eng.preset = optarg; break;
        case OPT_GOP: o.eng.gop_ms = atoi(optarg); break;
        case OPT_VBV: o.eng.vbv_ms = atoi(optarg); break;
        case OPT_GPU_TEST: gpu_test = true; break;
        case 'v': log_level = LOG_DBG; break;
        case 'q': log_level = LOG_WARN; break;
        case 'V': printf("caricoder3 %s\n", CC3_VERSION); return 0;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
    }
    if (gpu_test)
        return nv_selftest(stdout, o.eng.gpu);
    if (!o.input || (!o.n_output && !o.config) || optind < argc) {
        usage(stderr);
        return 2;
    }
    if (o.name && !o.control) {
        if (strchr(o.name, '/') || strlen(o.name) > 64) {
            fprintf(stderr, "--name is a plain name, without '/' and up to 64 characters\n");
            return 2;
        }
        snprintf(sock_path, sizeof(sock_path), "%s/%s.sock", o.run_dir ? o.run_dir : SOCK_DIR, o.name);
        o.control = sock_path;
    }

    /* Decoding and encoding take time, and an encoder looks ahead: what
     * is transcoded cannot leave as soon as what is merely copied. */
    {
        bool xa = false, xv = false;

        for (int i = 0; i < o.eng.n_audio; i++)
            xa |= o.eng.audio[i].codec != CODEC_NONE;
        for (int i = 0; i < o.eng.n_video; i++)
            xv |= o.eng.video[i].codec != CODEC_NONE;
        if (o.eng.gpu < 0 || o.eng.gpu > 63 || o.eng.gop_ms < 0 || o.eng.gop_ms > 20000 ||
            o.eng.vbv_ms < 0 || o.eng.vbv_ms > 5000) {
            fprintf(stderr, "option out of range (gpu 0..63, gop 0..20000 ms, vbv 0..5000 ms)\n");
            return 2;
        }
        /* New video is sent one encoder buffer (--vbv) before it is due,
         * and exists only after the codecs have held it back: the delay
         * has to cover both. */
        if (!delay_given) {
            o.eng.delay_ms = xv ? DELAY_VIDEO_MS : xa ? DELAY_AUDIO_MS : 500;
            if (xv && o.eng.vbv_ms > 1000)
                o.eng.delay_ms += o.eng.vbv_ms - 1000;
        } else if (xv && o.eng.delay_ms < (o.eng.vbv_ms ? o.eng.vbv_ms : 1000) + 500) {
            fprintf(stderr, "--delay %lld is too short for a video buffer (--vbv) of %d ms: "
                    "new video is sent that long before it is due. Use --delay %d or more, "
                    "or leave it out (%d).\n", (long long)o.eng.delay_ms,
                    o.eng.vbv_ms ? o.eng.vbv_ms : 1000,
                    (o.eng.vbv_ms ? o.eng.vbv_ms : 1000) + 500,
                    DELAY_VIDEO_MS + (o.eng.vbv_ms > 1000 ? o.eng.vbv_ms - 1000 : 0));
            return 2;
        }
    }

    if (o.eng.mux.muxrate == 0) {
        o.eng.mux.muxrate = VBR_DEFAULT_RATE;
        o.eng.mux.vbr = true;
    } else {
        o.eng.mux.vbr = vbr_flag;
    }
    if (o.eng.mux.muxrate < 20000 || o.eng.mux.muxrate > 400000000LL) {
        fprintf(stderr, "--muxrate must be between 20k and 400M\n");
        return 2;
    }
    if (o.eng.delay_ms < 20 || o.eng.delay_ms > 10000 ||
        o.eng.mux.pcr_interval_ms < 5 || o.eng.mux.pcr_interval_ms > 100 ||
        o.eng.mux.psi_interval_ms < 20 || o.eng.mux.psi_interval_ms > 1000 ||
        o.eng.mux.sdt_interval_ms < 100 || o.eng.mux.sdt_interval_ms > 10000 ||
        o.eng.program < 0 || o.eng.program > 65535) {
        fprintf(stderr, "option out of range (delay 20..10000, pcr-interval 5..100, "
                        "psi-interval 20..1000, sdt-interval 100..10000)\n");
        return 2;
    }
    if (o.eng.mux.pcr_interval_ms > 40)
        log_warn("PCR interval above 40 ms is outside the DVB recommendation");

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    o.eng.live = input_is_live(o.input);
    c.opt = &o;
    pthread_mutex_init(&c.stats_lock, NULL);

    if (!o.eng.live) {
        for (int i = 0; i < o.n_output; i++)
            if (strstr(o.output[i], "://") && strncmp(o.output[i], "file://", 7)) {
                fprintf(stderr, "a file input can only be written to a file in this version\n");
                return 2;
            }
        if (o.control || o.config) {
            fprintf(stderr, "--control, --name and --config are for a live input\n");
            return 2;
        }
    }

    if (o.thumb)
        thumb_init(o.thumb, o.thumb_interval);
    if (o.name) {
        static char http_sock[256];

        mkdir(o.run_dir ? o.run_dir : SOCK_DIR, 0775);
        snprintf(http_sock, sizeof(http_sock), "%s/%s.http", o.run_dir ? o.run_dir : SOCK_DIR, o.name);
        sink_hls_set_socket(http_sock);     /* an hls:// output serves on it */
    }

    /* Outputs first: nothing is read until there is somewhere to put it. */
    if (!ring_init(&c.ring)) {
        log_err("out of memory");
        return 1;
    }
    sinks_init(&c.sinks, &c.ring, o.eng.live);
    for (int i = 0; i < o.n_output; i++) {
        if (!sinks_add(&c.sinks, NULL, o.output[i], false, err, sizeof(err))) {
            log_err("output: %s", err);
            return 1;
        }
    }
    if (o.config && !config_load(&c, err, sizeof(err)))
        log_err("config: %s", err);
    if (o.control) {
        if (o.name)
            mkdir(o.run_dir ? o.run_dir : SOCK_DIR, 0775);
        c.ctl = control_start(o.control, on_control, &c, err, sizeof(err));
        if (!c.ctl) {
            log_err("control: %s", err);
            return 1;
        }
        log_info("control: %s", o.control);
    }

    if (o.eng.live) {
        c.src = sources_new(o.switch_ms, o.retry_s);
        if (!c.src) {
            log_err("out of memory");
            return 1;
        }
        for (int i = 0; i < o.n_input; i++)
            if (!sources_add(c.src, o.inputs[i], err, sizeof(err))) {
                log_err("input: %s", err);
                return 1;
            }
        if (!sources_start(c.src, err, sizeof(err))) {
            log_err("input: %s", err);
            return 1;
        }
    } else {
        if (o.n_input > 1) {
            log_err("input: a file input cannot have a backup");
            return 1;
        }
        in_fp = fopen(file_path(o.input), "rb");
        if (!in_fp) {
            log_err("input: %s: %s", file_path(o.input), strerror(errno));
            return 1;
        }
    }

    c.eng = o.eng.live ? engine_new(&o.eng, ring_out, &c.ring)
                       : engine_new(&o.eng, sync_out, &c.sinks);
    if (!c.eng) {
        log_err("out of memory");
        return 1;
    }

    log_info("caricoder3 %s: %s -> %s%s, %s %.3f Mb/s, delay %d ms",
             CC3_VERSION, o.input, sinks_first_url(&c.sinks, err, sizeof(err)),
             sinks_count(&c.sinks) > 1 ? " and more" : "",
             o.eng.mux.vbr ? "VBR up to" : "CBR",
             (double)o.eng.mux.muxrate / 1e6, o.eng.delay_ms);

    if (o.eng.live) {
        if (o.rt_prio > 0)
            mlockall(MCL_CURRENT | MCL_FUTURE);
        rc = run_live(&o, &c);
    } else {
        rc = run_file(&o, &c, in_fp);
    }

    if (in_fp)
        fclose(in_fp);
    control_stop(c.ctl);
    sources_free(c.src);
    engine_free(c.eng);
    sinks_destroy(&c.sinks);
    ring_destroy(&c.ring);
    free(c.stats_json);
    log_info("stopped");
    return rc;
}
