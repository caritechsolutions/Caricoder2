/*
 * caricoder3 - command line, threads, statistics
 * Copyright (c) 2026 CariTech Solutions
 *
 * One process handles one channel. Three threads in live running:
 *
 *   input   blocks in recvmmsg(), feeds the demux
 *   output  paces the mux onto the wire
 *   main    once a second: statistics file, log line, systemd watchdog
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
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "cc3.h"
#include "clock.h"
#include "codec.h"
#include "engine.h"
#include "log.h"
#include "udp.h"

#define RX_BATCH        64
#define RX_SIZE         16384
#define SINK_DGRAMS     640
#define DGRAM_MAX       (7 * TS_SIZE)
#define VBR_DEFAULT_RATE 40000000LL
#define DELAY_AUDIO_MS  800             /* default --delay when audio is transcoded */
#define DELAY_VIDEO_MS  3000            /* ... and when video is */

static atomic_int g_run = 1;             /* cleared by SIGINT / SIGTERM */

struct options {
    const char *input, *output;
    const char *stats_file;
    int stats_interval;
    int rt_prio;
    struct engine_cfg eng;
};

/* ---- output sink ---------------------------------------------------- */

struct sink {
    FILE *fp;                   /* file output, else NULL */
    int fd;                     /* UDP output */
    int pkt;                    /* TS packets per datagram */
    uint8_t (*dg)[DGRAM_MAX];
    struct mmsghdr *mm;
    struct iovec *iov;
    int n_full, fill;
    _Atomic uint64_t datagrams;
    _Atomic uint64_t errors;
};

static void sink_flush(struct sink *s, bool partial)
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
            atomic_fetch_add(&s->datagrams, (uint64_t)rc);
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

static void sink_out(void *opaque, const uint8_t *pkt)
{
    struct sink *s = opaque;

    if (s->fp) {
        if (fwrite(pkt, 1, TS_SIZE, s->fp) != TS_SIZE)
            atomic_fetch_add(&s->errors, 1);
        return;
    }
    memcpy(s->dg[s->n_full] + (size_t)s->fill * TS_SIZE, pkt, TS_SIZE);
    if (++s->fill == s->pkt) {
        s->fill = 0;
        if (++s->n_full == SINK_DGRAMS - 1)
            sink_flush(s, false);
    }
}

static bool sink_init(struct sink *s)
{
    s->dg = malloc((size_t)SINK_DGRAMS * DGRAM_MAX);
    s->mm = calloc(SINK_DGRAMS, sizeof(*s->mm));
    s->iov = calloc(SINK_DGRAMS, sizeof(*s->iov));
    return s->dg && s->mm && s->iov;
}

/* ---- threads -------------------------------------------------------- */

struct ctx {
    struct options *opt;
    struct engine *eng;
    struct sink sink;
    int rx_fd;
    _Atomic int64_t beat_in, beat_out;      /* thread heartbeats, ns */
    _Atomic int64_t last_rx;                /* last datagram received, ns */
    _Atomic uint64_t rx_overflow;           /* kernel socket buffer drops */
    _Atomic uint64_t rx_truncated;
    _Atomic int fatal;
};

static void *input_thread(void *arg)
{
    struct ctx *c = arg;
    uint8_t *bufs = malloc((size_t)RX_BATCH * RX_SIZE);
    struct mmsghdr mm[RX_BATCH];
    struct iovec iov[RX_BATCH];
    char ctl[RX_BATCH][CMSG_SPACE(sizeof(uint32_t))];

    if (!bufs) {
        atomic_store(&c->fatal, 1);
        return NULL;
    }
    while (atomic_load(&g_run)) {
        for (int i = 0; i < RX_BATCH; i++) {
            iov[i].iov_base = bufs + (size_t)i * RX_SIZE;
            iov[i].iov_len = RX_SIZE;
            memset(&mm[i], 0, sizeof(mm[i]));
            mm[i].msg_hdr.msg_iov = &iov[i];
            mm[i].msg_hdr.msg_iovlen = 1;
            mm[i].msg_hdr.msg_control = ctl[i];
            mm[i].msg_hdr.msg_controllen = sizeof(ctl[i]);
        }

        int n = recvmmsg(c->rx_fd, mm, RX_BATCH, MSG_WAITFORONE, NULL);
        int64_t now = clock_now_ns();

        atomic_store(&c->beat_in, now);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;                   /* receive timeout: no input */
            log_err("input: recvmmsg: %s", strerror(errno));
            atomic_store(&c->fatal, 1);
            break;
        }
        for (int i = 0; i < n; i++) {
            struct msghdr *h = &mm[i].msg_hdr;

            for (struct cmsghdr *cm = CMSG_FIRSTHDR(h); cm; cm = CMSG_NXTHDR(h, cm)) {
                if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SO_RXQ_OVFL) {
                    uint32_t drops;

                    memcpy(&drops, CMSG_DATA(cm), sizeof(drops));
                    atomic_store(&c->rx_overflow, drops);
                }
            }
            if (h->msg_flags & MSG_TRUNC)
                atomic_fetch_add(&c->rx_truncated, 1);
            engine_push_datagram(c->eng, iov[i].iov_base, mm[i].msg_len, now);
        }
        if (n > 0)
            atomic_store(&c->last_rx, now);
    }
    free(bufs);
    return NULL;
}

static void *output_thread(void *arg)
{
    struct ctx *c = arg;
    struct sink *s = &c->sink;
    bool vbr = c->opt->eng.mux.vbr;

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
        int64_t wake = engine_run(c->eng, now, s->pkt, s->fill);

        if (s->fp) {
            fflush(s->fp);
        } else {
            /* VBR: do not sit on a part-filled datagram until more turns up. */
            sink_flush(s, vbr && wake > now + 300000);
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

struct rates {
    double in_bps, out_bps, null_pct;
    double uptime;
    int64_t input_age_ms;       /* -1 when not applicable */
    uint64_t rx_overflow, rx_truncated, send_errors;
};

static void write_stats(const struct options *o, const struct engine_stats *st,
                        const struct rates *r)
{
    char tmp[1024];
    FILE *f;

    if (!o->stats_file)
        return;
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", o->stats_file) >= (int)sizeof(tmp))
        return;
    f = fopen(tmp, "w");
    if (!f)
        return;

    fprintf(f, "{\n  \"version\": \"%s\",\n  \"uptime_s\": %.1f,\n", CC3_VERSION, r->uptime);
    fprintf(f, "  \"state\": \"%s\",\n",
            !st->have_program ? "waiting_for_input" :
            !st->started ? "waiting_for_clock" : "running");

    fprintf(f, "  \"input\": {\n    \"url\": ");
    json_str(f, o->input);
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
               "    \"socket_overflow\": %llu,\n    \"truncated_datagrams\": %llu\n  },\n",
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

    fprintf(f, "  \"service\": {\n    \"number\": %u,\n    \"ts_id\": %u,\n"
               "    \"pmt_pid\": %u,\n    \"pcr_pid\": %u,\n    \"changes\": %llu,\n"
               "    \"streams\": [",
            st->prog.number, st->prog.ts_id, st->prog.pmt_pid, st->prog.pcr_pid,
            (unsigned long long)st->in.program_changes);
    for (int i = 0; st->have_program && i < st->prog.n_es; i++) {
        fprintf(f, "%s\n      {\"pid\": %u, \"stream_type\": %u, \"codec\": ",
                i ? "," : "", st->prog.es[i].pid, st->prog.es[i].stream_type);
        json_str(f, stream_type_name(st->prog.es[i].stream_type));
        const struct xcode_stats *x = NULL;

        for (int k = 0; k < st->n_xc; k++)
            if (st->xc[k].pid == st->prog.es[i].pid)
                x = &st->xc[k];
        if (!x) {
            fprintf(f, ", \"mode\": \"copy\"}");
            continue;
        }
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
        fprintf(f, "}");
    }
    fprintf(f, "\n    ]\n  },\n");

    fprintf(f, "  \"clock\": {\n    \"locked\": %s,\n    \"drift_ppm\": %.2f,\n"
               "    \"jitter_us\": %.1f,\n    \"ts_offset_ms\": %.3f,\n"
               "    \"discontinuities\": %llu,\n    \"held_units\": %u\n  },\n",
            st->clock_locked ? "true" : "false", st->clock_ppm, st->clock_jitter_us,
            (double)st->ts_offset / STC_PER_MS, (unsigned long long)st->discontinuities,
            st->held_units);

    fprintf(f, "  \"output\": {\n    \"url\": ");
    json_str(f, o->output);
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

    if (fclose(f) == 0)
        rename(tmp, o->stats_file);         /* readers never see half a file */
}

/* ---- running -------------------------------------------------------- */

static void on_signal(int sig)
{
    (void)sig;
    atomic_store(&g_run, 0);
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
        r.rx_overflow = atomic_load(&c->rx_overflow);
        r.rx_truncated = atomic_load(&c->rx_truncated);
        r.send_errors = atomic_load(&c->sink.errors);
        write_stats(o, &st, &r);

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
    if (c->sink.fp && fflush(c->sink.fp) != 0)
        atomic_fetch_add(&c->sink.errors, 1);

    engine_get_stats(c->eng, &st);
    memset(&r, 0, sizeof(r));
    r.input_age_ms = -1;
    r.send_errors = atomic_load(&c->sink.errors);
    if (st.out.packets)
        r.null_pct = 100.0 * (double)st.out.nulls / (double)st.out.packets;
    write_stats(o, &st, &r);

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
"Usage: caricoder3 -i INPUT -o OUTPUT [options]\n"
"\n"
"  -i, --input URL          udp://[source@]group:port[?iface=eth0&buf=BYTES]\n"
"                           rtp://... (RTP is also detected automatically)\n"
"                           or a .ts file\n"
"  -o, --output URL         udp://host:port[?ttl=8&iface=eth0&pkt=7] or a file\n"
"  -p, --program N          service to take from the input (default: first)\n"
"  -r, --muxrate RATE       constant bit rate output, e.g. 5M, 4500k, 5000000\n"
"      --vbr                variable bit rate: no null packets. This is the\n"
"                           default when --muxrate is not given; with\n"
"                           --muxrate it sets the peak rate\n"
"  -d, --delay MS           input-to-output buffer. Default 500; 800 when audio\n"
"                           is transcoded, 3000 when video is\n"
"      --audio SPEC         transcode audio: aac, mp2 or mp3, with an optional\n"
"                           bit rate (aac:128k). PID=SPEC for one stream only\n"
"                           (1653=aac:96k); may be repeated. Default: copy\n"
"      --video SPEC         transcode video: h264 or hevc with a bit rate\n"
"                           (h264:3M). Default: copy\n"
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
"      --stats-interval S   log a status line every S seconds (default 10, 0 off)\n"
"      --rt-prio N          SCHED_FIFO priority for the output thread\n"
"      --backends           list codec backends and exit\n"
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

static bool is_udp(const char *s)
{
    return !strncmp(s, "udp://", 6) || !strncmp(s, "rtp://", 6);
}

static const char *file_path(const char *s)
{
    return !strncmp(s, "file:", 5) ? s + 5 : s;
}

enum {
    OPT_VBR = 1000, OPT_PCR, OPT_PSI, OPT_SDT, OPT_NAME, OPT_PROVIDER,
    OPT_STATS_FILE, OPT_STATS_INT, OPT_RT, OPT_BACKENDS,
    OPT_AUDIO, OPT_VIDEO, OPT_DECODER, OPT_ENCODER, OPT_GPU, OPT_PRESET,
    OPT_GOP, OPT_VBV,
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
        { "video", required_argument, 0, OPT_VIDEO },
        { "decoder", required_argument, 0, OPT_DECODER },
        { "encoder", required_argument, 0, OPT_ENCODER },
        { "gpu", required_argument, 0, OPT_GPU },
        { "preset", required_argument, 0, OPT_PRESET },
        { "gop", required_argument, 0, OPT_GOP },
        { "vbv", required_argument, 0, OPT_VBV },
        { "verbose", no_argument, 0, 'v' },
        { "quiet", no_argument, 0, 'q' },
        { "version", no_argument, 0, 'V' },
        { "help", no_argument, 0, 'h' },
        { 0, 0, 0, 0 }
    };
    struct options o;
    struct ctx c;
    struct udp_addr in_addr, out_addr;
    struct sigaction sa;
    char err[256];
    bool vbr_flag = false, delay_given = false;
    FILE *in_fp = NULL;
    int opt, rc;

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
        case 'i': o.input = optarg; break;
        case 'o': o.output = optarg; break;
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
        case OPT_BACKENDS: codec_list(stdout); return 0;
        case OPT_AUDIO:
        case OPT_VIDEO: {
            bool v = opt == OPT_VIDEO;
            int max = v ? 4 : 8;
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
        case OPT_GPU: o.eng.gpu = atoi(optarg); break;
        case OPT_PRESET: o.eng.preset = optarg; break;
        case OPT_GOP: o.eng.gop_ms = atoi(optarg); break;
        case OPT_VBV: o.eng.vbv_ms = atoi(optarg); break;
        case 'v': log_level = LOG_DBG; break;
        case 'q': log_level = LOG_WARN; break;
        case 'V': printf("caricoder3 %s\n", CC3_VERSION); return 0;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
    }
    if (!o.input || !o.output || optind < argc) {
        usage(stderr);
        return 2;
    }

    /* Decoding and encoding take time, and an encoder looks ahead: what
     * is transcoded cannot leave as soon as what is merely copied. */
    {
        bool xa = false, xv = false;

        for (int i = 0; i < o.eng.n_audio; i++)
            xa |= o.eng.audio[i].codec != CODEC_NONE;
        for (int i = 0; i < o.eng.n_video; i++)
            xv |= o.eng.video[i].codec != CODEC_NONE;
        if (!delay_given)
            o.eng.delay_ms = xv ? DELAY_VIDEO_MS : xa ? DELAY_AUDIO_MS : 500;
    }
    if (o.eng.gpu < 0 || o.eng.gpu > 63 || o.eng.gop_ms < 0 || o.eng.gop_ms > 20000 ||
        o.eng.vbv_ms < 0 || o.eng.vbv_ms > 5000) {
        fprintf(stderr, "option out of range (gpu 0..63, gop 0..20000 ms, vbv 0..5000 ms)\n");
        return 2;
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
    signal(SIGPIPE, SIG_IGN);

    o.eng.live = is_udp(o.input);
    c.opt = &o;
    c.rx_fd = -1;
    c.sink.fd = -1;
    c.sink.pkt = 7;

    if (!o.eng.live && is_udp(o.output)) {
        fprintf(stderr, "a file input can only be written to a file in this version\n");
        return 2;
    }

    /* Output first: nothing is read until there is somewhere to put it. */
    if (is_udp(o.output)) {
        if (udp_parse(o.output, &out_addr, err, sizeof(err)) < 0 ||
            (c.sink.fd = udp_open_tx(&out_addr, err, sizeof(err))) < 0) {
            log_err("output: %s", err);
            return 1;
        }
        c.sink.pkt = out_addr.pkt;
        if (!sink_init(&c.sink)) {
            log_err("out of memory");
            return 1;
        }
    } else {
        c.sink.fp = fopen(file_path(o.output), "wb");
        if (!c.sink.fp) {
            log_err("output: %s: %s", file_path(o.output), strerror(errno));
            return 1;
        }
        setvbuf(c.sink.fp, NULL, _IOFBF, 1 << 20);
    }

    if (o.eng.live) {
        if (udp_parse(o.input, &in_addr, err, sizeof(err)) < 0 ||
            (c.rx_fd = udp_open_rx(&in_addr, err, sizeof(err))) < 0) {
            log_err("input: %s", err);
            return 1;
        }
    } else {
        in_fp = fopen(file_path(o.input), "rb");
        if (!in_fp) {
            log_err("input: %s: %s", file_path(o.input), strerror(errno));
            return 1;
        }
    }

    c.eng = engine_new(&o.eng, sink_out, &c.sink);
    if (!c.eng) {
        log_err("out of memory");
        return 1;
    }

    log_info("caricoder3 %s: %s -> %s, %s %.3f Mb/s, delay %d ms",
             CC3_VERSION, o.input, o.output,
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
    if (c.sink.fp && fclose(c.sink.fp) != 0) {
        log_err("output: write failed: %s", strerror(errno));
        rc = 1;
    }
    if (c.rx_fd >= 0)
        close(c.rx_fd);
    if (c.sink.fd >= 0)
        close(c.sink.fd);
    engine_free(c.eng);
    free(c.sink.dg);
    free(c.sink.mm);
    free(c.sink.iov);
    log_info("stopped");
    return rc;
}
