/*
 * caricoder3 - the supervisor: runs the channels, serves the API and the GUI
 * Copyright (c) 2026 CariTech Solutions
 *
 *   caricoder3 serve [--config /etc/caricoder3/caricoder3.json]
 *
 * One process for the machine. It keeps one caricoder3 channel process
 * per file in the channels directory, starts them, restarts them when
 * they die (with a back-off), stops them on request, keeps the last
 * lines each one wrote, and serves on one TCP port (8550 unless the
 * config says otherwise):
 *
 *   /                          the GUI (plain HTML and JavaScript, built in)
 *   /api/system                version, uptime, what this build can do
 *   /api/channels              every channel with its state and a summary
 *   /api/channels/NAME         GET: config, state, statistics, outputs
 *                              PUT: the config (JSON); a running channel restarts
 *                              DELETE: stop and remove
 *   /api/channels/NAME/start | stop | restart     POST
 *   /api/channels/NAME/outputs                    POST {url, id}: add (saved)
 *   /api/channels/NAME/outputs/ID                 DELETE
 *   /api/channels/NAME/log                        the last lines it wrote
 *   /api/channels/NAME/thumb.jpg                  a picture of the channel, a few seconds old
 *   /api/probe                 POST {url, seconds, program}: what is in a source
 *                              (caricoder3 probe, run as a child; up to 30 s)
 *   /hls/NAME/...              the channel's HLS output, proxied
 *
 * Everything but /hls needs HTTP basic authentication: user "admin",
 * the password in the system config (made up and written there on the
 * first run, printed in the log).
 *
 * A channel file (channels/NAME.json) holds what the command line would:
 * {"input": "udp://...", "backup_inputs": ["srt://..."], "program": 0, "muxrate": "5M", "vbr": false,
 *  "delay": 500, "video": ["h264:3M", "640x360:h264:1M"], "audio": ["aac:128k"], "drop": [258],
 *  "decoder": "auto", "encoder": "auto", "gpu": 0, "preset": "", "gop": 2000,
 *  "vbv": 1000, "service_name": "", "service_provider": "", "enabled": true}
 * Its outputs live in channels/NAME.outputs.json, which the channel
 * itself reads and writes (--config), so outputs added while it runs
 * survive a restart.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cc3.h"
#include "clock.h"
#include "codec.h"
#include "control.h"
#include "http.h"
#include "json.h"
#include "log.h"
#include "sink.h"
#include "sysstat.h"
#include "history.h"
#include "watchdog.h"
#include "memwatch.h"
#include "disk.h"
#include "peer.h"
#include "ird.h"
#include "tech.h"

#include <stdarg.h>

#define CHANNELS_MAX    256
#define SHARES_MAX      32
#define LOG_LINES       200
#define LOG_LINE_MAX    240
#define STOP_GRACE_MS   6000
#define HIST_N          60                  /* samples kept */
#define HIST_STEP_S     2
#define PREVIEW_IDLE_S  60                  /* no request for this long: the preview stops */
#define DEFAULT_PORT    8550
#define DEFAULT_CONFIG  "/etc/caricoder3/caricoder3.json"

extern const unsigned char gui_html[];
extern const unsigned int gui_html_len;
extern const unsigned char gui_hls_js[];       /* hls.js, Apache-2.0 (gui/hls.js.LICENSE) */
extern const unsigned int gui_hls_js_len;

enum chan_state { CH_STOPPED, CH_STARTING, CH_RUNNING, CH_STOPPING, CH_FAILED };

struct chan {
    char name[64];
    char *cfg;                              /* the JSON text of its file */
    bool enabled;
    bool want;                              /* should be running */
    enum chan_state state;
    pid_t pid;
    int64_t started_ns, stop_asked_ns, next_start_ns;
    int restarts;
    int exit_code;
    char why[160];                          /* how it ended last */
    int log_fd;
    char log[LOG_LINES][LOG_LINE_MAX];
    int log_n, log_head;
    char log_part[LOG_LINE_MAX];
    int log_part_len;

    /* the last two minutes of bit rates, a sample every HIST_STEP_S */
    float hist_in[HIST_N], hist_out[HIST_N];
    int hist_n, hist_head;
    int64_t hist_last_ns;

    /* the preview: a small H.264 copy of the channel for the GUI's player,
     * made by a child process while someone watches */
    pid_t pv_pid;
    int pv_log_fd;
    int64_t pv_started_ns, pv_last_hit_ns;
    char pv_drop[128];                      /* the --drop list it runs with */
    char pv_why[120];
    bool pv_restart;                        /* start again with pv_next once the old one is gone */
    bool pv_tap;                            /* the channel has the "pvtap" output for it */
    char pv_next[128];

    /* a shared input: the channel reads its program from the share's
     * reader on this loopback port (0 = reads its input itself) */
    uint32_t sh_hash;                       /* of cfg, when the fields below were worked out */
    bool sh_able;                           /* its input is UDP/RTP and sharing is not turned off */
    char sh_url[512];
    int sh_program;
    int share_port;

    /* the record: the 2 s samples summed up into 10 s rows */
    double h_in_sum, h_out_sum;
    int h_n;
    int64_t h_cc, h_ovf, h_drop, h_late, h_lost, h_sw;      /* counters at the last row */
    int64_t c_cc, c_ovf, c_drop, c_late, c_lost, c_sw;      /* as last read */
    int h_restarts;
    bool h_have;                            /* the counters above are from this run */
    bool h_no_input, h_backup;              /* as last seen: for the events */
    int h_active;
    int64_t h_ev_last;                      /* last warning from its log that was kept */
    char h_ev_text[160];
    int h_ev_repeats;

    /* the watchdog (watchdog.h): counters over the last minute, when it died */
    struct { int64_t t, cc, loss, drop; } wd_win[40];
    int wd_wn, wd_wh;
    int64_t wd_died[16];
    int wd_nd;
    char wd_group[512];                     /* its source, for grouping */
    char lang_url[300];                     /* an output of it the language check can read beside it */
    struct memtrack *mem;                   /* its process's memory, a sample a minute (memwatch.h) */
    char *wd_stats;                         /* the statistics the watchdog last judged (main thread) */
    char out_dirs[4][256];                  /* folders its outputs write to (HLS dir=, file://) */
    int64_t wd_flow_since;                  /* input arriving since (0: it is not) */
    int64_t ev_last;                        /* the last recording made for it without being asked */
    /* the language check (Whisper, caricoder3 listen) */
    int64_t lang_t, lang_due;               /* last answer; next check (wall clock) */
    char lang_heard[8];                     /* the last confident answer ("eng") */
    double lang_p, lang_vol;
    int lang_wrong_n;                       /* confident answers in a row that were not what it should be */
    int64_t lang_wrong_since;               /* ... the first of them */
    bool lang_wrong;                        /* two of them: an incident */
    bool lang_busy;
    char lang_note[160];                    /* the last answer in words */
    int n_out_dirs;
};

/* A tool at work (the evidence tools): a child, or an output of a channel for a while. */
#define JOBS_MAX 8
struct job {
    bool used;
    pid_t pid;                              /* 0: a recording through the channel's output,
                                             * or of what it kept of its input (kept) */
    bool kept;
    char tool[16];
    char chan[64];
    int64_t incident;
    char out[600];                          /* the child's output file; the output's id */
    char file[600];                         /* what it keeps */
    char args[700];                         /* JSON */
    int64_t started_ns, deadline_ns;
};

/* A filesystem's use over the last hour, a sample a minute: how fast it fills. */
struct disk_track {
    char mount[128];
    int64_t t[61];
    uint64_t used[61];
    int n, head;
};

/* A folder of interest and how much is in it (measured now and then). */
struct disk_path {
    char path[256];
    char what[48];
    int64_t bytes;
};

/* One reader of an input that several channels take programs from
 * (caricoder3 share): the input is read once. */
struct share {
    bool used;
    char url[512];
    pid_t pid;
    int64_t next_start_ns;
    int restarts;
    bool stopping;                          /* asked to end: nobody uses it */
    char *cfg_text;                         /* the outputs it was last given */
};

/* The card process of one NVIDIA card (caricoder3 card, card.h): the
 * video transcoding of the channels on that card. */
#define CARDS_MAX 8
struct cardp {
    int index;
    pid_t pid;
    int64_t next_start_ns;
    int64_t started_ns;
    int restarts;
    bool stopping;
    struct memtrack *mem;
};

struct super {
    char config_path[512];
    char listen[64];
    int port;
    char channels_dir[512];
    char run_dir[512];
    char password[80];
    char binary[512];
    char history_path[512];                 /* the record (history.h); "" = none */
    int history_max_mb;
    int64_t hist_last_s;                    /* wall clock of the last 10 s row */
    int64_t t_start;

    pthread_mutex_t lock;                   /* channels, from the web threads */
    struct chan ch[CHANNELS_MAX];
    int n;
    struct share sh[SHARES_MAX];
    char card_mode[8];                      /* "auto", "off", "test" (CPU, for tests) */
    char timezone[64];                      /* the site's, e.g. "America/Barbados" ("": the server's) */
    struct ird_conf irds[IRDS_MAX];         /* the satellite receivers (ird.h) */
    int n_irds;
    struct ird_opts ird_o;                  /* how often (tests set them short) */
    int lang_every_s;                       /* the language check, every (6 h; 0: off) */
    char lang_expect[8];                    /* what the channels speak unless theirs says ("eng") */
    double lang_min_p;                      /* Whisper this sure, or the answer is "not sure" (0.5) */
    int lang_clip_s;                        /* each clip (20 s) */
    int lang_confirm_s;                     /* wrong only if heard so over this long (600 s: longer than an
                                             * advertising break, which can be in another language) */
    int64_t lang_have_t;                    /* when Whisper's presence was last looked at */
    bool lang_have;
    struct cardp cards[CARDS_MAX];
    int n_cards;
    bool stopping_all;                      /* the supervisor is ending: card processes go last */
    struct memtrack *mem_self;              /* the supervisor's own memory */
    int64_t mem_last;
    int mem_test_s;                         /* tests: sample this often instead of a minute */
    struct http_server *http;

    /* the disks (disk.h), main thread, read by the web under the lock */
    struct disk_fs disks[DISK_MAX];
    int n_disks;
    struct disk_track dtr[DISK_MAX];
    struct disk_path dpath[32];
    int n_dpath;
    int64_t disk_last, du_last, squeeze_last, ev_trim_last;
    bool squeezed;

    /* the evidence tools */
    char evidence_dir[512];                 /* "none": nothing kept */
    int evidence_max_mb;
    bool evidence_auto;                     /* gathered when an incident opens */
    struct job jobs[JOBS_MAX];
    char probe_url[8][512];                 /* sources probed lately, not again for a minute */
    int64_t probe_t[8];

    /* the other transcoders (peer.h) */
    char name[64];                          /* what this one is called to them */
    char peer_key[128];
    struct peer_conf peers[PEERS_MAX];
    int n_peers;

    /* the technician (tech.h) */
    struct tech_conf tech;
};

static volatile sig_atomic_t g_stop, g_hup;

static void on_sig(int sig)
{
    if (sig == SIGHUP)
        g_hup = 1;
    else
        g_stop = 1;
}

/* ---- files ------------------------------------------------------------------ */

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "r");
    char *text;
    size_t n, cap = 65536;

    if (!f)
        return NULL;
    text = malloc(cap);
    if (!text) {
        fclose(f);
        return NULL;
    }
    n = fread(text, 1, cap - 1, f);
    fclose(f);
    text[n] = 0;
    if (len)
        *len = n;
    return text;
}

/* A whole file, of any size up to `max` (binary; a 0 after it all the same). */
static char *read_whole(const char *path, size_t *len, size_t max)
{
    FILE *f = fopen(path, "rb");
    struct stat sb;
    char *d;
    size_t n;

    if (!f)
        return NULL;
    if (fstat(fileno(f), &sb) != 0 || (size_t)sb.st_size > max || !(d = malloc((size_t)sb.st_size + 1))) {
        fclose(f);
        return NULL;
    }
    n = fread(d, 1, (size_t)sb.st_size, f);
    fclose(f);
    d[n] = 0;
    if (len)
        *len = n;
    return d;
}

static bool write_file(const char *path, const char *text, char *err, size_t errlen)
{
    char tmp[600];
    FILE *f;

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f) {
        snprintf(err, errlen, "%s: %s", path, strerror(errno));
        return false;
    }
    fputs(text, f);
    if (fclose(f) != 0 || rename(tmp, path) != 0) {
        snprintf(err, errlen, "%s: %s", path, strerror(errno));
        unlink(tmp);
        return false;
    }
    return true;
}

static bool name_ok(const char *name)
{
    size_t n = strlen(name);

    if (n == 0 || n > 48)
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_'))
            return false;
    }
    return true;
}

/* ---- the system config -------------------------------------------------------- */

static void make_password(char *out, size_t len)
{
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    FILE *f = fopen("/dev/urandom", "r");
    unsigned char r[16];
    size_t n = len - 1 < 14 ? len - 1 : 14;

    if (!f || fread(r, 1, sizeof(r), f) != sizeof(r)) {
        for (size_t i = 0; i < sizeof(r); i++)
            r[i] = (unsigned char)(rand() ^ (int)time(NULL));
    }
    if (f)
        fclose(f);
    for (size_t i = 0; i < n; i++)
        out[i] = alphabet[r[i] % (sizeof(alphabet) - 1)];
    out[n] = 0;
}

/* One IRD from the config or the API: {"name", "address", "kind", "raw",
 * "outputs": [...]}. False (with a reason) when it is not usable. */
static bool ird_conf_read(const struct json_doc *d, int j, struct ird_conf *c, char *err, size_t errlen)
{
    int o = json_get(d, j, "outputs");
    const char *raw;

    memset(c, 0, sizeof(*c));
    snprintf(c->name, sizeof(c->name), "%s", json_get_str(d, j, "name", ""));
    snprintf(c->address, sizeof(c->address), "%s", json_get_str(d, j, "address", ""));
    snprintf(c->kind, sizeof(c->kind), "%s", json_get_str(d, j, "kind", "upcom"));
    raw = json_get_str(d, j, "raw", "");
    snprintf(c->raw, sizeof(c->raw), "%s", raw);
    for (int k = o >= 0 ? d->node[o].first_child : -1; k >= 0 && c->n_out < IRD_OUTS; k = d->node[k].next)
        if (d->node[k].type == JSON_STRING && d->node[k].str[0])
            snprintf(c->outputs[c->n_out++], sizeof(c->outputs[0]), "%s", d->node[k].str);
    for (const char *p = c->name; *p; p++)
        if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == '.')) {
            if (err)
                snprintf(err, errlen, "IRD name %s: letters, digits, - _ . only", c->name);
            return false;
        }
    if (!c->name[0] || !c->address[0]) {
        if (err)
            snprintf(err, errlen, "an IRD needs a name and an address");
        return false;
    }
    {
        /* the tuners in use: ...?tuners=1 (the others are not judged) */
        const char *q = strstr(c->address, "?tuners=");

        c->tuners = 0;
        for (const char *p = q ? q + 8 : NULL; p;) {
            char *end;
            long n = strtol(p, &end, 10);

            if (end == p)
                break;
            if (n >= 1 && n <= IRD_TUNERS)
                c->tuners |= 1u << (n - 1);
            p = *end == ',' ? end + 1 : NULL;
        }
    }
    {
        /* the kind may be said in the address: mcr://admin:admin@192.168.110.15 */
        const char *sep = strstr(c->address, "://");

        if (sep && sep - c->address < (long)sizeof(c->kind) && strncmp(c->address, "http://", 7))
            snprintf(c->kind, sizeof(c->kind), "%.*s", (int)(sep - c->address), c->address);
    }
    if (strcmp(c->kind, "upcom") && strcmp(c->kind, "mcr")) {
        if (err)
            snprintf(err, errlen, "IRD %s: kind %s is not known (upcom, mcr)", c->name, c->kind);
        return false;
    }
    return true;
}

/* "udp://224.100.1.5:49500?..." and "udp://@224.100.1.5:49500": the same */
static bool same_source(const char *a, const char *b)
{
    size_t la, lb;

    if (!strncmp(a, "udp://", 6))
        a += 6 + (a[6] == '@');
    if (!strncmp(b, "udp://", 6))
        b += 6 + (b[6] == '@');
    la = strcspn(a, "?");
    lb = strcspn(b, "?");
    return la == lb && !strncmp(a, b, la);
}

/* ird.h's question: does one of our channels take this PID of this program
 * from this source? (Its input that, its program that or all of them, the
 * PID not dropped.) */
static bool chan_uses(const char *url, int program, int pid, void *op)
{
    struct super *s = op;
    static struct json_doc doc;         /* under the supervisor's lock */
    bool yes = false;

    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < s->n && !yes; i++) {
        char err[100];
        int root = s->ch[i].cfg ? json_parse(&doc, s->ch[i].cfg, err, sizeof(err)) : -1, d;
        int prog;

        if (root < 0 || !json_get_bool(&doc, root, "enabled", true))
            continue;
        if (!same_source(json_get_str(&doc, root, "input", ""), url))
            continue;
        prog = (int)json_get_num(&doc, root, "program", 0);
        if (prog && prog != program)
            continue;
        yes = true;
        d = json_get(&doc, root, "drop");
        for (int j = d >= 0 ? doc.node[d].first_child : -1; j >= 0; j = doc.node[j].next)
            if ((int)doc.node[j].num == pid)
                yes = false;
    }
    pthread_mutex_unlock(&s->lock);
    return yes;
}

static void irds_apply(struct super *s)
{
    s->ird_o.binary = s->binary;
    ird_set(s->irds, s->n_irds, &s->ird_o);
}

/* The site's time zone, for the times the technician is told and writes
 * in tickets (this server's clock may be on UTC). A name in the system's
 * zone files, or "" for the server's own. Returns false for an unknown one. */
static bool set_timezone(struct super *s, const char *tz)
{
    char path[200];

    if (*tz && (strstr(tz, "..") || *tz == '/' || strlen(tz) >= sizeof(s->timezone)))
        return false;
    snprintf(path, sizeof(path), "/usr/share/zoneinfo/%s", tz);
    if (*tz && access(path, R_OK))
        return false;
    snprintf(s->timezone, sizeof(s->timezone), "%s", tz);
    if (*tz)
        setenv("TZ", tz, 1);
    else
        unsetenv("TZ");
    tzset();
    return true;
}

static void save_system_config(struct super *s)
{
    char text[16384], q[13][600], err[200];

    snprintf(text, sizeof(text),
             "{\n  \"listen\": %s,\n  \"port\": %d,\n  \"channels_dir\": %s,\n"
             "  \"run_dir\": %s,\n  \"password\": %s,\n  \"history\": %s,\n  \"history_max_mb\": %d,\n"
             "  \"evidence\": %s,\n  \"evidence_max_mb\": %d,\n  \"evidence_auto\": %s,\n"
             "  \"name\": %s,\n  \"peer_key\": %s,\n  \"card_process\": %s,\n  \"timezone\": %s,\n"
             "  \"technician\": {\"enabled\": %s, \"gpu\": %s, \"threads\": %d, \"model\": %s, \"idle_s\": %d, "
             "\"server\": %s, \"context\": %d, \"delay_s\": %d, \"port\": %d, \"knowledge\": %s}\n}\n",
             json_quote(s->listen, q[0], sizeof(q[0])), s->port,
             json_quote(s->channels_dir, q[1], sizeof(q[1])),
             json_quote(s->run_dir, q[2], sizeof(q[2])),
             json_quote(s->password, q[3], sizeof(q[3])),
             json_quote(s->history_path, q[4], sizeof(q[4])), s->history_max_mb,
             json_quote(s->evidence_dir, q[5], sizeof(q[5])), s->evidence_max_mb, s->evidence_auto ? "true" : "false",
             json_quote(s->name, q[6], sizeof(q[6])), json_quote(s->peer_key, q[7], sizeof(q[7])),
             s->card_mode[0] ? (!strcmp(s->card_mode, "off") ? "\"off\"" : !strcmp(s->card_mode, "test") ? "\"test\"" : "\"auto\"") : "\"auto\"",
             json_quote(s->timezone, q[12], sizeof(q[12])),
             s->tech.enabled ? "true" : "false", json_quote(s->tech.gpu, q[8], sizeof(q[8])), s->tech.threads,
             json_quote(s->tech.model, q[9], sizeof(q[9])), s->tech.idle_s,
             json_quote(s->tech.server, q[10], sizeof(q[10])), s->tech.ctx, s->tech.delay_s, s->tech.port,
             json_quote(s->tech.knowledge[1], q[11], sizeof(q[11])));
    {
        /* the peers, before the closing brace */
        size_t n = strlen(text);
        char pq[2][600];

        if (n > 3) {
            size_t at = n - 3;

            at += (size_t)snprintf(text + at, sizeof(text) - at, ",\n  \"peers\": [");
            for (int i = 0; i < s->n_peers && at < sizeof(text) - 700; i++)
                at += (size_t)snprintf(text + at, sizeof(text) - at, "%s{\"name\": %s, \"url\": %s}", i ? ", " : "",
                                       json_quote(s->peers[i].name, pq[0], sizeof(pq[0])),
                                       json_quote(s->peers[i].url, pq[1], sizeof(pq[1])));
            snprintf(text + at, sizeof(text) - at, "]\n}\n");
        }
    }
    {
        /* the IRDs, before the closing brace */
        size_t n = strlen(text);
        char iq[4][600];

        if (n > 3) {
            size_t at = n - 3;

            at += (size_t)snprintf(text + at, sizeof(text) - at, ",\n  \"irds\": [");
            for (int i = 0; i < s->n_irds && at < sizeof(text) - 3000; i++) {
                const struct ird_conf *c = &s->irds[i];

                at += (size_t)snprintf(text + at, sizeof(text) - at, "%s\n    {\"name\": %s, \"address\": %s, \"kind\": %s, "
                                       "\"raw\": %s, \"outputs\": [", i ? "," : "",
                                       json_quote(c->name, iq[0], sizeof(iq[0])), json_quote(c->address, iq[1], sizeof(iq[1])),
                                       json_quote(c->kind, iq[2], sizeof(iq[2])), json_quote(c->raw, iq[3], sizeof(iq[3])));
                for (int k = 0; k < c->n_out; k++)
                    at += (size_t)snprintf(text + at, sizeof(text) - at, "%s%s", k ? ", " : "",
                                           json_quote(c->outputs[k], iq[0], sizeof(iq[0])));
                at += (size_t)snprintf(text + at, sizeof(text) - at, "]}");
            }
            snprintf(text + at, sizeof(text) - at, "]\n}\n");
        }
    }
    {
        /* the language check, before the closing brace */
        size_t n = strlen(text);
        char lq[40];

        if (n > 3)
            snprintf(text + n - 3, sizeof(text) - (n - 3),
                     ",\n  \"language_check\": {\"every_h\": %g, \"expect\": %s, \"min_probability\": %.2f, \"clip_s\": %d}\n}\n",
                     s->lang_every_s / 3600.0, json_quote(s->lang_expect, lq, sizeof(lq)), s->lang_min_p, s->lang_clip_s);
    }
    {
        /* the watchdog's limits, before the closing brace */
        struct wd_limits L;
        char *wbuf = NULL;
        size_t wlen = 0;
        FILE *wf = open_memstream(&wbuf, &wlen);
        size_t n = strlen(text);

        wd_get_limits(&L);
        if (wf) {
            wd_limits_write(wf, &L);
            fclose(wf);
        }
        if (wbuf && n > 3 && n + wlen + 32 < sizeof(text)) {
            snprintf(text + n - 3, sizeof(text) - (n - 3), ",\n  \"watchdog\": %s\n}\n", wbuf);
        }
        free(wbuf);
    }
    if (!write_file(s->config_path, text, err, sizeof(err)))
        log_err("config: %s", err);
}

static bool load_system_config(struct super *s, char *err, size_t errlen)
{
    char *text = read_file(s->config_path, NULL);
    static struct json_doc doc;
    int root;

    snprintf(s->listen, sizeof(s->listen), "0.0.0.0");
    s->port = DEFAULT_PORT;
    snprintf(s->channels_dir, sizeof(s->channels_dir), "/etc/caricoder3/channels");
    snprintf(s->run_dir, sizeof(s->run_dir), "/run/caricoder3");
    s->password[0] = 0;
    snprintf(s->history_path, sizeof(s->history_path), "/var/lib/caricoder3/history.db");
    s->history_max_mb = 2048;
    snprintf(s->evidence_dir, sizeof(s->evidence_dir), "/var/lib/caricoder3/evidence");
    s->evidence_max_mb = 4096;
    s->evidence_auto = true;
    if (gethostname(s->name, sizeof(s->name) - 1) != 0)
        snprintf(s->name, sizeof(s->name), "caricoder3");
    s->peer_key[0] = 0;
    s->n_peers = 0;
    memset(&s->tech, 0, sizeof(s->tech));
    s->tech.enabled = true;
    snprintf(s->tech.server, sizeof(s->tech.server), "/usr/local/bin/caricoder3-llama-server");
    snprintf(s->tech.model, sizeof(s->tech.model), "/var/lib/caricoder3/models/Qwen2.5-7B-Instruct-Q4_K_M.gguf");
    snprintf(s->tech.knowledge[0], sizeof(s->tech.knowledge[0]), "/usr/local/share/caricoder3/knowledge");
    snprintf(s->tech.knowledge[1], sizeof(s->tech.knowledge[1]), "/etc/caricoder3/knowledge");
    snprintf(s->tech.gpu, sizeof(s->tech.gpu), "auto");
    s->tech.threads = 8;
    s->tech.ctx = 16384;
    s->tech.idle_s = 600;
    s->tech.delay_s = 45;
    s->tech.port = 8551;
    if (!text) {
        if (errno != ENOENT) {
            snprintf(err, errlen, "%.200s: %s", s->config_path, strerror(errno));
            return false;
        }
        return true;                        /* defaults; written below */
    }
    root = json_parse(&doc, text, err, errlen);
    free(text);
    if (root < 0)
        return false;
    snprintf(s->listen, sizeof(s->listen), "%s", json_get_str(&doc, root, "listen", "0.0.0.0"));
    s->port = (int)json_get_num(&doc, root, "port", DEFAULT_PORT);
    snprintf(s->channels_dir, sizeof(s->channels_dir), "%s",
             json_get_str(&doc, root, "channels_dir", "/etc/caricoder3/channels"));
    snprintf(s->run_dir, sizeof(s->run_dir), "%s", json_get_str(&doc, root, "run_dir", "/run/caricoder3"));
    snprintf(s->password, sizeof(s->password), "%s", json_get_str(&doc, root, "password", ""));
    snprintf(s->history_path, sizeof(s->history_path), "%s",
             json_get_str(&doc, root, "history", "/var/lib/caricoder3/history.db"));
    s->history_max_mb = (int)json_get_num(&doc, root, "history_max_mb", 2048);
    snprintf(s->evidence_dir, sizeof(s->evidence_dir), "%s",
             json_get_str(&doc, root, "evidence", "/var/lib/caricoder3/evidence"));
    s->evidence_max_mb = (int)json_get_num(&doc, root, "evidence_max_mb", 4096);
    s->evidence_auto = json_get_bool(&doc, root, "evidence_auto", true);
    {
        const char *nm = json_get_str(&doc, root, "name", "");

        if (*nm)
            snprintf(s->name, sizeof(s->name), "%s", nm);
    }
    snprintf(s->peer_key, sizeof(s->peer_key), "%s", json_get_str(&doc, root, "peer_key", ""));
    snprintf(s->card_mode, sizeof(s->card_mode), "%s", json_get_str(&doc, root, "card_process", "auto"));
    s->mem_test_s = (int)json_get_num(&doc, root, "memory_sample_s", 0);   /* for the tests */
    set_timezone(s, json_get_str(&doc, root, "timezone", ""));
    if (strcmp(s->card_mode, "off") && strcmp(s->card_mode, "test"))
        snprintf(s->card_mode, sizeof(s->card_mode), "auto");
    {
        int t = json_get(&doc, root, "technician");

        if (t >= 0) {
            s->tech.enabled = json_get_bool(&doc, t, "enabled", s->tech.enabled);
            snprintf(s->tech.server, sizeof(s->tech.server), "%s", json_get_str(&doc, t, "server", s->tech.server));
            snprintf(s->tech.model, sizeof(s->tech.model), "%s", json_get_str(&doc, t, "model", s->tech.model));
            snprintf(s->tech.gpu, sizeof(s->tech.gpu), "%s", json_get_str(&doc, t, "gpu", s->tech.gpu));
            s->tech.threads = (int)json_get_num(&doc, t, "threads", s->tech.threads);
            s->tech.ctx = (int)json_get_num(&doc, t, "context", s->tech.ctx);
            s->tech.idle_s = (int)json_get_num(&doc, t, "idle_s", s->tech.idle_s);
            s->tech.delay_s = (int)json_get_num(&doc, t, "delay_s", s->tech.delay_s);
            s->tech.port = (int)json_get_num(&doc, t, "port", s->tech.port);
            snprintf(s->tech.knowledge[1], sizeof(s->tech.knowledge[1]), "%s",
                     json_get_str(&doc, t, "knowledge", s->tech.knowledge[1]));
        }
    }
    {
        int a = json_get(&doc, root, "irds");

        s->n_irds = 0;
        for (int j = a >= 0 ? doc.node[a].first_child : -1; j >= 0 && s->n_irds < IRDS_MAX; j = doc.node[j].next)
            if (ird_conf_read(&doc, j, &s->irds[s->n_irds], NULL, 0))
                s->n_irds++;
        s->ird_o.poll_s = (int)json_get_num(&doc, root, "ird_poll_s", 60);          /* the tests make these short */
        s->ird_o.remux_s = (int)json_get_num(&doc, root, "ird_remux_s", 3 * 3600);   /* the tables are read every minute */
        s->ird_o.baseline_n = (int)json_get_num(&doc, root, "ird_baseline_n", 30);
        s->ird_o.probe_s = (int)json_get_num(&doc, root, "ird_probe_s", 5);
    }
    {
        int l = json_get(&doc, root, "language_check");

        s->lang_every_s = 6 * 3600;
        snprintf(s->lang_expect, sizeof(s->lang_expect), "eng");
        s->lang_min_p = 0.5;
        s->lang_clip_s = 20;
        s->lang_confirm_s = 600;
        if (l >= 0) {
            s->lang_every_s = (int)(json_get_num(&doc, l, "every_h", 6) * 3600);
            snprintf(s->lang_expect, sizeof(s->lang_expect), "%s", json_get_str(&doc, l, "expect", "eng"));
            s->lang_min_p = json_get_num(&doc, l, "min_probability", 0.5);
            s->lang_clip_s = (int)json_get_num(&doc, l, "clip_s", 20);
            if (json_get(&doc, l, "every_s") >= 0)          /* the tests */
                s->lang_every_s = (int)json_get_num(&doc, l, "every_s", 0);
            if (json_get(&doc, l, "confirm_s") >= 0)
                s->lang_confirm_s = (int)json_get_num(&doc, l, "confirm_s", 600);
        }
    }
    {
        int a = json_get(&doc, root, "peers");

        s->n_peers = 0;
        for (int j = a >= 0 ? doc.node[a].first_child : -1; j >= 0 && s->n_peers < PEERS_MAX; j = doc.node[j].next) {
            const char *u = json_get_str(&doc, j, "url", "");

            if (!*u)
                continue;
            snprintf(s->peers[s->n_peers].name, sizeof(s->peers[0].name), "%s", json_get_str(&doc, j, "name", u));
            snprintf(s->peers[s->n_peers].url, sizeof(s->peers[0].url), "%s", u);
            s->n_peers++;
        }
    }
    {
        struct wd_limits L;

        wd_defaults(&L);
        wd_limits_read(&doc, json_get(&doc, root, "watchdog"), &L);
        wd_set_limits(&L);
    }
    if (s->port < 1 || s->port > 65535) {
        snprintf(err, errlen, "port must be 1..65535");
        return false;
    }
    return true;
}

/* An event for the record (history.h): `chan` NULL for the server. */
static void ev(const char *chan, const char *level, const char *kind, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
static void ev(const char *chan, const char *level, const char *kind, const char *fmt, ...)
{
    char text[400];
    va_list ap;

    if (!history_enabled())
        return;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    history_event(time(NULL), chan, level, kind, text);
}

/* ---- the channel processes ------------------------------------------------------ */

static struct chan *find_chan(struct super *s, const char *name)
{
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->ch[i].name, name))
            return &s->ch[i];
    return NULL;
}

static void chan_log_line(struct chan *c, const char *line)
{
    int slot = (c->log_head + c->log_n) % LOG_LINES;

    if (c->log_n == LOG_LINES)
        c->log_head = (c->log_head + 1) % LOG_LINES;
    else
        c->log_n++;
    snprintf(c->log[slot], LOG_LINE_MAX, "%s", line);
}

/* ---- the card processes (card.h) ------------------------------------------- */

static bool cards_on(const struct super *s)
{
    return strcmp(s->card_mode, "off") != 0;
}

/* The NVIDIA cards there are, from their device files (/dev/nvidia0 ...):
 * one card process each. "test": one, card 0, transcoding on the CPU. */
static void cards_detect(struct super *s)
{
    s->n_cards = 0;
    if (!cards_on(s))
        return;
    if (!strcmp(s->card_mode, "test")) {
        s->cards[s->n_cards++].index = 0;
    } else {
        for (int i = 0; i < 64 && s->n_cards < CARDS_MAX; i++) {
            char dev[32];

            snprintf(dev, sizeof(dev), "/dev/nvidia%d", i);
            if (access(dev, F_OK) == 0)
                s->cards[s->n_cards++].index = i;
        }
    }
    for (int k = 0; k < s->n_cards; k++)
        s->cards[k].pid = 0;
    if (s->n_cards)
        log_info("card processes: %d (%s)", s->n_cards, !strcmp(s->card_mode, "test") ? "test, on the CPU" : "one per NVIDIA card");
}

static const struct cardp *card_of(const struct super *s, int gpu)
{
    for (int k = 0; k < s->n_cards; k++)
        if (s->cards[k].index == gpu)
            return &s->cards[k];
    return NULL;
}

static void card_paths(const struct super *s, int index, char *sock, size_t sl, char *stats, size_t stl)
{
    snprintf(sock, sl, "%.400s/card-%d.sock", s->run_dir, index);
    snprintf(stats, stl, "%.400s/card-%d.json", s->run_dir, index);
}

/* Started, and started again when one ends; stopped (SIGTERM) when
 * `stop` and no channel is left. */
static void cards_update(struct super *s, int64_t now, bool stop)
{
    for (int k = 0; k < s->n_cards; k++) {
        struct cardp *cp = &s->cards[k];
        int status;

        if (cp->pid > 0 && waitpid(cp->pid, &status, WNOHANG) == cp->pid) {
            if (!cp->stopping) {
                char what[200];
                int64_t up = (now - cp->started_ns) / 1000000000LL;

                snprintf(what, sizeof(what), "the card process of card %d ended (%s %d) after %lld s; "
                         "its channels' video stopped until it was started again", cp->index,
                         WIFSIGNALED(status) ? "signal" : "status",
                         WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status), (long long)up);
                log_err("%s", what);
                ev(NULL, "error", "card", "%s", what);
                {
                    char subj[32];

                    snprintf(subj, sizeof(subj), "server/card%d", cp->index);
                    wd_instant_server(time(NULL), subj, WD_CARD_ENDED, what);
                }
                cp->restarts++;
                /* soon, but not in a tight loop if it cannot start at all */
                cp->next_start_ns = now + (up < 10 ? 2000000000LL : 300000000LL);
            }
            cp->pid = 0;
            cp->stopping = false;
        }
        if (stop) {
            if (cp->pid > 0 && !cp->stopping) {
                kill(cp->pid, SIGTERM);
                cp->stopping = true;
            }
            continue;
        }
        if (cp->pid <= 0 && now >= cp->next_start_ns) {
            char sock[600], stats[600], idx[16];
            pid_t pid;

            card_paths(s, cp->index, sock, sizeof(sock), stats, sizeof(stats));
            snprintf(idx, sizeof(idx), "%d", cp->index);
            pid = fork();
            if (pid == 0) {
                sigset_t set;

                sigemptyset(&set);
                sigprocmask(SIG_SETMASK, &set, NULL);
                signal(SIGHUP, SIG_DFL);
                signal(SIGINT, SIG_DFL);
                signal(SIGTERM, SIG_DFL);
                execl(s->binary, s->binary, "card", "--card", idx, "--socket", sock,
                      "--stats-file", stats, (char *)NULL);
                _exit(127);
            }
            if (pid > 0) {
                cp->pid = pid;
                cp->started_ns = now;
                log_info("card %d: card process started (pid %d)", cp->index, (int)pid);
            }
        }
    }
}

static void chan_share_prepare(struct super *s, struct chan *c);
static bool evidence_on(const struct super *s);

/* Builds the command line from the channel's JSON. Returns the number
 * of arguments, -1 with a reason. */
static int chan_argv(struct super *s, struct chan *c, char **argv, int max, char *err, size_t errlen)
{
    static struct json_doc doc;            /* under the supervisor's lock */
    static char store[96][512];
    int root, n = 0, k = 0;
    const char *input;
#define ARG(fmt, ...) do { if (n + 1 >= max || k >= 96) goto full; \
        snprintf(store[k], sizeof(store[k]), fmt, __VA_ARGS__); argv[n++] = store[k++]; } while (0)
#define ARGS(str) do { if (n + 1 >= max) goto full; argv[n++] = (char *)(str); } while (0)

    root = json_parse(&doc, c->cfg, err, errlen);
    if (root < 0)
        return -1;
    input = json_get_str(&doc, root, "input", NULL);
    if (!input || !input[0]) {
        snprintf(err, errlen, "no \"input\"");
        return -1;
    }
    ARGS(s->binary);
    ARGS("-i");
    chan_share_prepare(s, c);
    if (c->share_port) {
        /* its program, from the share's reader */
        ARG("udp://127.0.0.1:%d", c->share_port);
        ARGS("--input-label");
        ARG("%s", input);
    } else {
        ARG("%s", input);
    }
    {
        /* "backup_inputs": ["udp://...", ...]: tried in turn when the input gives nothing */
        int a = json_get(&doc, root, "backup_inputs");

        if (a >= 0 && doc.node[a].type == JSON_ARRAY)
            for (int j = doc.node[a].first_child; j >= 0; j = doc.node[j].next)
                if (doc.node[j].type == JSON_STRING && doc.node[j].str[0]) {
                    ARGS("-i");
                    ARG("%s", doc.node[j].str);
                }
    }
    ARGS("--name");
    ARGS(c->name);
    ARGS("--run-dir");
    ARGS(s->run_dir);
    if (evidence_on(s))
        ARGS("--keep-input");               /* the last 90 s, for evidence (keep.h) */
    if (card_of(s, (int)json_get_num(&doc, root, "gpu", 0)) &&
        json_get_bool(&doc, root, "card_process", true)) {
        /* its video transcoded in the card process (card.h) */
        ARGS("--card-process");
        ARGS(s->run_dir);
        if (!strcmp(s->card_mode, "test"))
            ARGS("--card-process-all");
    }
    ARGS("--config");
    ARG("%.400s/%s.outputs.json", s->channels_dir, c->name);
    ARGS("--streams-file");
    ARG("%.400s/%s.streams", s->channels_dir, c->name);
    ARGS("--stats-file");
    ARG("%.400s/%s.json", s->run_dir, c->name);
    ARGS("--thumb");
    ARG("%.400s/%s.jpg", s->run_dir, c->name);
    ARGS("--thumb-interval");       /* the picture is watched at this rate too (watch.h) */
    ARGS("2");
    ARGS("--stats-interval");
    ARGS("60");
    {
        double v = json_get_num(&doc, root, "program", 0);

        if (v > 0)
            ARG("--program=%d", (int)v);
        {
            const char *rate = json_get_str(&doc, root, "muxrate", NULL);
            int ri = json_get(&doc, root, "muxrate");

            if (rate && rate[0])
                ARG("--muxrate=%s", rate);
            else if (ri >= 0 && doc.node[ri].type == JSON_NUMBER && doc.node[ri].num > 0)
                ARG("--muxrate=%.0f", doc.node[ri].num);
        }
        if (json_get_bool(&doc, root, "vbr", false))
            ARGS("--vbr");
        v = json_get_num(&doc, root, "delay", 0);
        if (v > 0)
            ARG("--delay=%d", (int)v);
        v = json_get_num(&doc, root, "gpu", -1);
        if (v >= 0)
            ARG("--gpu=%d", (int)v);
        v = json_get_num(&doc, root, "gop", 0);
        if (v > 0)
            ARG("--gop=%d", (int)v);
        v = json_get_num(&doc, root, "vbv", 0);
        if (v > 0)
            ARG("--vbv=%d", (int)v);
        v = json_get_num(&doc, root, "bframes", -1);
        if (v >= 0)
            ARG("--bframes=%d", (int)v);
        v = json_get_num(&doc, root, "pcr_interval", 0);
        if (v > 0)
            ARG("--pcr-interval=%d", (int)v);
        v = json_get_num(&doc, root, "psi_interval", 0);
        if (v > 0)
            ARG("--psi-interval=%d", (int)v);
        v = json_get_num(&doc, root, "switch_after", 0);
        if (v > 0)
            ARG("--switch-after=%d", (int)v);
        v = json_get_num(&doc, root, "retry_primary", 0);
        if (v > 0)
            ARG("--retry-primary=%d", (int)v);
        {
            /* "drop": [257, 258] -> --drop=257,258 */
            int a = json_get(&doc, root, "drop");
            char list[256] = "";
            size_t o = 0;

            if (a >= 0 && doc.node[a].type == JSON_ARRAY)
                for (int j = doc.node[a].first_child; j >= 0; j = doc.node[j].next)
                    if (doc.node[j].type == JSON_NUMBER) {
                        int need = snprintf(NULL, 0, "%s%d", o ? "," : "", (int)doc.node[j].num);

                        if (o + (size_t)need >= sizeof(list)) {
                            snprintf(err, errlen, "too many PIDs in \"drop\"");
                            return -1;
                        }
                        o += (size_t)snprintf(list + o, sizeof(list) - o, "%s%d", o ? "," : "",
                                              (int)doc.node[j].num);
                    }
            if (list[0])
                ARG("--drop=%s", list);
        }
        {
            /* the output's identifiers:
             * "remap": {"256": 481, ...} (input PID -> output PID), "order": [258, 256, 257],
             * "pmt_pid": 4096, "pcr_pid": 481 or "video", "service_id", "ts_id", "onid" */
            int a = json_get(&doc, root, "remap");
            char list[512] = "";
            size_t o = 0;

            if (a >= 0 && doc.node[a].type == JSON_OBJECT)
                for (int j = doc.node[a].first_child; j >= 0; j = doc.node[j].next)
                    if (doc.node[j].type == JSON_NUMBER && doc.node[j].key && o + 16 < sizeof(list))
                        o += (size_t)snprintf(list + o, sizeof(list) - o, "%s%ld=%d", o ? "," : "",
                                              strtol(doc.node[j].key, NULL, 0), (int)doc.node[j].num);
            if (list[0])
                ARG("--remap=%s", list);
            a = json_get(&doc, root, "order");
            list[0] = 0;
            o = 0;
            if (a >= 0 && doc.node[a].type == JSON_ARRAY)
                for (int j = doc.node[a].first_child; j >= 0; j = doc.node[j].next)
                    if (doc.node[j].type == JSON_NUMBER && o + 8 < sizeof(list))
                        o += (size_t)snprintf(list + o, sizeof(list) - o, "%s%d", o ? "," : "", (int)doc.node[j].num);
            if (list[0])
                ARG("--order=%s", list);
            static const char *const ids[][2] = {
                { "pmt_pid", "--pmt-pid=%d" }, { "service_id", "--service-id=%d" },
                { "ts_id", "--ts-id=%d" }, { "onid", "--onid=%d" }, { "pcr_pid", "--pcr-pid=%d" },
            };
            for (size_t j = 0; j < sizeof(ids) / sizeof(ids[0]); j++) {
                int nd = json_get(&doc, root, ids[j][0]);

                if (nd >= 0 && doc.node[nd].type == JSON_NUMBER)
                    ARG(ids[j][1], (int)doc.node[nd].num);
                else if (nd >= 0 && doc.node[nd].type == JSON_STRING && !strcmp(ids[j][0], "pcr_pid") &&
                         !strcmp(doc.node[nd].str, "video"))
                    ARGS("--pcr-pid=video");
            }
        }
    }
    {
        static const char *const strs[][2] = {
            { "decoder", "--decoder=%s" }, { "encoder", "--encoder=%s" }, { "preset", "--preset=%s" },
            { "profile", "--profile=%s" },
            { "service_name", "--service-name=%s" }, { "service_provider", "--service-provider=%s" },
        };

        for (size_t i = 0; i < sizeof(strs) / sizeof(strs[0]); i++) {
            const char *v = json_get_str(&doc, root, strs[i][0], NULL);

            if (v && v[0])
                ARG(strs[i][1], v);
        }
    }
    {
        static const char *const lists[][2] = { { "video", "--video=%s" }, { "audio", "--audio=%s" },
                                                { "extra_args", "%s" } };

        for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); i++) {
            int a = json_get(&doc, root, lists[i][0]);

            if (a < 0 || doc.node[a].type != JSON_ARRAY)
                continue;
            for (int j = doc.node[a].first_child; j >= 0; j = doc.node[j].next)
                if (doc.node[j].type == JSON_STRING && doc.node[j].str[0])
                    ARG(lists[i][1], doc.node[j].str);
        }
    }
    argv[n] = NULL;
    return n;
full:
    snprintf(err, errlen, "too many arguments");
    return -1;
#undef ARG
#undef ARGS
}

static void chan_start(struct super *s, struct chan *c)
{
    char *argv[120];
    char err[200];
    int pfd[2];
    pid_t pid;

    if (chan_argv(s, c, argv, 120, err, sizeof(err)) < 0) {
        snprintf(c->why, sizeof(c->why), "config: %.140s", err);
        c->state = CH_FAILED;
        c->want = false;
        chan_log_line(c, c->why);
        log_err("channel %s: %s", c->name, err);
        ev(c->name, "error", "config", "%s", err);
        return;
    }
    if (pipe2(pfd, O_CLOEXEC | O_NONBLOCK) < 0) {
        snprintf(c->why, sizeof(c->why), "pipe: %s", strerror(errno));
        c->state = CH_FAILED;
        return;
    }
    pid = fork();
    if (pid < 0) {
        snprintf(c->why, sizeof(c->why), "fork: %s", strerror(errno));
        c->state = CH_FAILED;
        close(pfd[0]);
        close(pfd[1]);
        return;
    }
    if (pid == 0) {
        /* the child: stderr to the pipe, a fresh signal state */
        sigset_t set;

        sigemptyset(&set);
        sigprocmask(SIG_SETMASK, &set, NULL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        dup2(pfd[1], 2);
        close(pfd[1]);
        close(pfd[0]);
        execv(argv[0], argv);
        fprintf(stderr, "cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(pfd[1]);
    c->pid = pid;
    c->log_fd = pfd[0];
    c->state = CH_RUNNING;
    c->started_ns = clock_now_ns();
    c->why[0] = 0;
    log_info("channel %s: started (pid %d)", c->name, (int)pid);
    ev(c->name, "info", "start", "started");
    c->h_have = false;
}

static void chan_stop(struct super *s, struct chan *c)
{
    (void)s;
    if (c->state != CH_RUNNING || c->pid <= 0)
        return;
    kill(c->pid, SIGTERM);
    c->state = CH_STOPPING;
    c->stop_asked_ns = clock_now_ns();
}

/* The lines a channel wrote since last time. */
/* A warning or error the channel wrote goes into the record too; the same
 * line again within a minute is counted, not repeated. */
static void chan_log_event(struct chan *c, const char *line)
{
    const char *lv = strlen(line) > 24 ? line + 24 : "";
    const char *level = !strncmp(lv, "WARN", 4) ? "warn" : !strncmp(lv, "ERROR", 5) ? "error" : NULL;
    const char *text;
    int64_t now = time(NULL);

    if (!level)
        return;
    text = lv + 5;
    while (*text == ' ')
        text++;
    if (!strncmp(text, "source PIDs changed: ", 21))
        wd_instant(now, c->name, c->wd_group, WD_PIDS, text + 21);
    if (!history_enabled())
        return;
    if (!strcmp(text, c->h_ev_text) && now - c->h_ev_last < 60) {
        c->h_ev_repeats++;
        return;
    }
    if (c->h_ev_repeats)
        ev(c->name, "info", "log", "(the line before %d more time%s)", c->h_ev_repeats,
           c->h_ev_repeats > 1 ? "s" : "");
    c->h_ev_repeats = 0;
    snprintf(c->h_ev_text, sizeof(c->h_ev_text), "%s", text);
    c->h_ev_last = now;
    ev(c->name, level, "log", "%s", text);
}

static void chan_read_log(struct chan *c)
{
    char buf[4096];

    if (c->log_fd < 0)
        return;
    for (;;) {
        ssize_t r = read(c->log_fd, buf, sizeof(buf));

        if (r <= 0)
            break;
        for (ssize_t i = 0; i < r; i++) {
            if (buf[i] == '\n') {
                c->log_part[c->log_part_len] = 0;
                chan_log_line(c, c->log_part);
                chan_log_event(c, c->log_part);
                fprintf(stderr, "[%s] %s\n", c->name, c->log_part);
                c->log_part_len = 0;
            } else if (c->log_part_len < LOG_LINE_MAX - 1) {
                c->log_part[c->log_part_len++] = buf[i];
            }
        }
    }
}

static void chan_reap(struct super *s, struct chan *c, int status)
{
    int64_t now = clock_now_ns();
    bool asked = c->state == CH_STOPPING;

    chan_read_log(c);
    if (c->log_fd >= 0)
        close(c->log_fd);
    c->log_fd = -1;
    c->pid = 0;
    if (WIFEXITED(status)) {
        c->exit_code = WEXITSTATUS(status);
        snprintf(c->why, sizeof(c->why), "exited with status %d", c->exit_code);
    } else if (WIFSIGNALED(status)) {
        c->exit_code = 128 + WTERMSIG(status);
        snprintf(c->why, sizeof(c->why), "killed by signal %d%s", WTERMSIG(status),
                 WTERMSIG(status) == SIGSEGV ? " (crash)" : "");
    }
    if (asked || !c->want) {
        c->state = CH_STOPPED;
        log_info("channel %s: stopped", c->name);
        ev(c->name, "info", "stop", "stopped");
        return;
    }
    /* it died on its own: back off, doubling, up to 30 s; a channel that
     * ran a good while gets a fresh start */
    c->state = CH_FAILED;
    if (now - c->started_ns > 60 * 1000000000LL)
        c->restarts = 0;
    {
        int64_t wait = 1000000000LL << (c->restarts < 5 ? c->restarts : 5);

        if (wait > 30000000000LL)
            wait = 30000000000LL;
        c->next_start_ns = now + wait;
        c->restarts++;
        if (c->wd_nd == (int)(sizeof(c->wd_died) / sizeof(c->wd_died[0]))) {
            memmove(c->wd_died, c->wd_died + 1, sizeof(c->wd_died) - sizeof(c->wd_died[0]));
            c->wd_nd--;
        }
        c->wd_died[c->wd_nd++] = time(NULL);
        log_warn("channel %s: %s; starting again in %lld s", c->name, c->why, (long long)(wait / 1000000000LL));
        ev(c->name, "error", "exit", "%s; starting again in %lld s", c->why, (long long)(wait / 1000000000LL));
    }
    (void)s;
}

/* Once around: logs, exits, restarts, stop timeouts. Called with the lock. */
static char *chan_stats(struct super *s, struct chan *c);

/* One bit-rate sample from the channel's statistics file. */
/* Evidence for a new incident: the channel's statistics as they are. */
static char *chan_stats(struct super *s, struct chan *c);
static struct chan *find_chan(struct super *s, const char *name);
static void chan_source(const struct chan *c, char *url, size_t len, int *program);

static void wd_snapshot(FILE *f, const char *chan, void *op)
{
    struct super *s = op;
    struct chan *c = chan ? find_chan(s, chan) : NULL;
    /* what the watchdog saw when it decided, not the file as it is now (it
     * may have changed since: a short silence already over) */
    char *st = !c || c->state != CH_RUNNING ? NULL : c->wd_stats ? strdup(c->wd_stats) : chan_stats(s, c);

    if (st) {
        size_t n = strlen(st);

        while (n && (st[n - 1] == '\n' || st[n - 1] == ' '))
            n--;
        if (n && st[0] == '{' && st[n - 1] == '}') {
            /* and the readings of the IRD it reads from, if it is one we know */
            char *ib = NULL;
            size_t il = 0;
            FILE *m = open_memstream(&ib, &il);
            char src[512];
            int prog;
            bool have;

            chan_source(c, src, sizeof(src), &prog);    /* its source: not a shared reader's port */
            have = m && ird_for_url(m, src);

            if (m)
                fclose(m);
            if (have && n > 2) {
                fwrite(st, 1, n - 1, f);
                fprintf(f, ", \"ird\": %s}", ib);
            } else {
                fwrite(st, 1, n, f);
            }
            free(ib);
        } else {
            fprintf(f, "{}");
        }
    } else if (!chan && s->n_irds) {
        /* the server's: how the IRDs are */
        fprintf(f, "{\"irds\": ");
        ird_json(f);
        fprintf(f, "}");
    } else {
        fprintf(f, "{}");
    }
    free(st);
}

static bool source_readable(const char *url);

/* The watchdog's view of one sample of a running channel (watchdog.h). */
static void wd_channel(struct chan *c, const struct json_doc *d, int root, double bi, double bo)
{
    struct wd_limits L;
    int64_t t = time(NULL);
    int in = json_get(d, root, "input"), out = json_get(d, root, "output"), clk = json_get(d, root, "clock");
    int svc = json_get(d, root, "service"), ct = json_get(d, root, "content");
    int ss = svc >= 0 ? json_get(d, svc, "streams") : -1;
    double up = json_get_num(d, root, "uptime_s", 0);
    double age = in >= 0 ? json_get_num(d, in, "last_packet_age_ms", -1) : -1;
    bool flowing = age >= 0 && age < 1000 && bi > 0;
    const char *G;
    char what[160];

    wd_get_limits(&L);
    if (in >= 0)
        snprintf(c->wd_group, sizeof(c->wd_group), "%s", json_get_str(d, in, "url", ""));
    {
        /* where its outputs write, for the disk watch */
        int outs = json_get(d, root, "outputs");

        c->n_out_dirs = 0;
        c->lang_url[0] = 0;
        for (int j = outs >= 0 ? d->node[outs].first_child : -1; j >= 0 && c->n_out_dirs < 4; j = d->node[j].next) {
            const char *u = json_get_str(d, j, "url", ""), *p = NULL;
            char dir[256];

            if (!c->lang_url[0] && !strncmp(u, "udp://", 6) && strncmp(json_get_str(d, j, "id", ""), "evrec", 5) &&
                source_readable(u))
                snprintf(c->lang_url, sizeof(c->lang_url), "%s", u);

            if (!strncmp(json_get_str(d, j, "id", ""), "evrec", 5))
                continue;                   /* an evidence recording: the evidence folder is watched already */

            if (!strncmp(u, "file://", 7)) {
                snprintf(dir, sizeof(dir), "%s", u + 7);
                char *sl = strrchr(dir, '/');

                if (sl && sl != dir)
                    *sl = 0;
                p = dir;
            } else if (!strncmp(u, "hls://", 6) && (p = strstr(u, "dir="))) {
                size_t n = strcspn(p + 4, "&");

                snprintf(dir, sizeof(dir), "%.*s", (int)(n < sizeof(dir) ? n : sizeof(dir) - 1), p + 4);
                p = dir;
            } else {
                p = NULL;
            }
            if (p && *p == '/')
                snprintf(c->out_dirs[c->n_out_dirs++], sizeof(c->out_dirs[0]), "%s", p);
        }
    }
    G = c->wd_group;
    /* the content rules count only while input arrives, and from when it began to */
    if (!flowing)
        c->wd_flow_since = 0;
    else if (!c->wd_flow_since)
        c->wd_flow_since = t;
#define SINCE_FLOW(b_) ((b_) > c->wd_flow_since ? (b_) : c->wd_flow_since)
    /* the counters a minute ago */
    {
        int k = (c->wd_wh + c->wd_wn) % 40;
        int64_t loss = c->c_ovf + c->c_lost, drop = c->c_drop + c->c_late;
        int64_t dcc = 0, dloss = 0, ddrop = 0;

        if (c->wd_wn == 40)
            c->wd_wh = (c->wd_wh + 1) % 40;
        else
            c->wd_wn++;
        c->wd_win[k].t = t;
        c->wd_win[k].cc = c->c_cc;
        c->wd_win[k].loss = loss;
        c->wd_win[k].drop = drop;
        for (int i = 0; i < c->wd_wn; i++) {
            int j = (c->wd_wh + i) % 40;

            if (t - c->wd_win[j].t <= 60) {
                /* a restarted channel counts from 0 again: then from there */
                dcc = c->c_cc >= c->wd_win[j].cc ? c->c_cc - c->wd_win[j].cc : c->c_cc;
                dloss = loss >= c->wd_win[j].loss ? loss - c->wd_win[j].loss : loss;
                ddrop = drop >= c->wd_win[j].drop ? drop - c->wd_win[j].drop : drop;
                break;
            }
        }
        snprintf(what, sizeof(what), "CC errors: %lld in a minute", (long long)dcc);
        /* (a channel's first minute is its start: a source joined part-way through a
         * packet run, encoders opening, the clock locking. Not judged.) */
        wd_observe(t, c->name, c->name, G, WD_CC, up >= L.warmup_s && dcc >= L.cc_per_min, what, 0);
        snprintf(what, sizeof(what), "packets lost on the way in: %lld in a minute", (long long)dloss);
        wd_observe(t, c->name, c->name, G, WD_LOSS, dloss > 0, what, 0);
        snprintf(what, sizeof(what), "dropped or late pictures: %lld in a minute", (long long)ddrop);
        wd_observe(t, c->name, c->name, G, WD_DROPPING, up >= L.warmup_s && ddrop > L.dropped_per_min, what, 0);
    }
    {
        bool none = age > L.no_input_s * 1000.0 || (up > 10 && bi == 0);

        wd_observe(t, c->name, c->name, G, WD_NO_INPUT, none, "no input",
                   none && age > 0 ? t - (int64_t)(age / 1000) : 0);
    }
    wd_observe(t, c->name, c->name, G, WD_OUT_STALLED, flowing && up > 10 && out >= 0 && bo == 0,
               "input arrives, nothing goes out", 0);
    {
        int active = in >= 0 ? (int)json_get_num(d, in, "active", 0) : 0;

        snprintf(what, sizeof(what), "running on backup source %d", active);
        wd_observe(t, c->name, c->name, G, WD_BACKUP, active > 0, what, 0);
    }
    wd_observe(t, c->name, c->name, G, WD_CLOCK,
               clk >= 0 && flowing && up > 15 && !json_get_bool(d, clk, "locked", true), "clock not locked", 0);
    /* the content (watch.h) */
    {
        int pic = ct >= 0 ? json_get(d, ct, "picture") : -1;
        /* while input arrives: without it the last picture stays as it was */
        bool fresh = flowing && pic >= 0 && json_get_num(d, pic, "age_ms", 1e9) < 30000;
        double black = fresh ? json_get_num(d, pic, "black_ms", 0) : 0;
        double frozen = fresh ? json_get_num(d, pic, "frozen_ms", 0) : 0;

        wd_observe(t, c->name, c->name, G, WD_BLACK, black > 0, "black picture", SINCE_FLOW(t - (int64_t)(black / 1000)));
        wd_observe(t, c->name, c->name, G, WD_FROZEN, frozen > 0, "frozen picture", SINCE_FLOW(t - (int64_t)(frozen / 1000)));
    }
    {
        /* silent audio: which, by the language the stream list gives it */
        int aa = ct >= 0 ? json_get(d, ct, "audio") : -1;
        double longest = 0;
        size_t n = 0;

        what[0] = 0;
        for (int j = aa >= 0 ? d->node[aa].first_child : -1; j >= 0; j = d->node[j].next) {
            double ms = json_get_num(d, j, "silent_ms", 0);
            int pid = (int)json_get_num(d, j, "pid", 0);
            const char *lang = "";

            if (ms <= 0)
                continue;
            for (int k = ss >= 0 ? d->node[ss].first_child : -1; k >= 0; k = d->node[k].next)
                if ((int)json_get_num(d, k, "in_pid", -1) == pid && json_get(d, k, "from_pid") < 0)
                    lang = json_get_str(d, k, "language", "");
            n += (size_t)snprintf(what + n, n < sizeof(what) ? sizeof(what) - n : 0, "%s%s (PID %d)", n ? ", " : "audio ",
                                  lang[0] ? lang : "no language", pid);
            if (ms > longest)
                longest = ms;
            if (n >= sizeof(what))
                break;
        }
        if (what[0])
            snprintf(what + strlen(what), sizeof(what) - strlen(what), " silent");
        wd_observe(t, c->name, c->name, G, WD_SILENT, flowing && longest > 0, what, SINCE_FLOW(t - (int64_t)(longest / 1000)));
    }
    {
        /* video or audio listed, nothing of it arriving */
        double longest = 0;
        size_t n = 0;

        what[0] = 0;
        for (int k = ss >= 0 ? d->node[ss].first_child : -1; k >= 0; k = d->node[k].next) {
            const char *kind = json_get_str(d, k, "kind", "");
            double idle = json_get_num(d, k, "idle_ms", -1);

            if (json_get(d, k, "from_pid") >= 0 || idle < 5000 || (strcmp(kind, "video") && strcmp(kind, "audio")))
                continue;
            n += (size_t)snprintf(what + n, n < sizeof(what) ? sizeof(what) - n : 0, "%s%s %s (PID %d)",
                                  n ? ", " : "", kind, json_get_str(d, k, "language", ""),
                                  (int)json_get_num(d, k, "in_pid", 0));
            if (idle > longest)
                longest = idle;
            if (n >= sizeof(what))
                break;
        }
        if (what[0])
            snprintf(what + strlen(what), sizeof(what) - strlen(what), ": in the PMT, no data");
        /* only while the rest arrives: no input at all is the other rule */
        wd_observe(t, c->name, c->name, G, WD_NO_DATA, up >= L.warmup_s && flowing && longest > 0, what,
                   SINCE_FLOW(t - (int64_t)(longest / 1000)));
    }
    {
        /* video the card process should transcode, transcoded by the channel itself:
         * it lost the card process too often (xc_remote.c), and uses more card memory */
        bool own = false;

        for (int k = ss >= 0 ? d->node[ss].first_child : -1; k >= 0; k = d->node[k].next)
            own |= !strcmp(json_get_str(d, k, "card_process", ""), "own");
        wd_observe(t, c->name, c->name, G, WD_CARD_OWN, own,
                   "video transcoded by the channel itself: it lost the card process too often "
                   "(restart the channel to go back to it)", 0);
    }
}

/* What the watchdog sees of a channel whatever its state: failing,
 * restarting; stopped on purpose, nothing. */
static void wd_channel_state(struct chan *c, int64_t t)
{
    struct wd_limits L;
    int recent = 0;
    char what[200];
    static const enum wd_rule running_rules[] = { WD_NO_INPUT, WD_CC, WD_LOSS, WD_DROPPING, WD_OUT_STALLED,
                                                  WD_BACKUP, WD_CLOCK, WD_BLACK, WD_FROZEN, WD_SILENT, WD_NO_DATA,
                                                  WD_CARD_OWN, WD_MEM_GROWING };

    if (!c->cfg || !c->want) {
        wd_forget(t, c->name);
        return;
    }
    wd_get_limits(&L);
    for (int i = 0; i < c->wd_nd; i++)
        recent += t - c->wd_died[i] <= L.restart_window_s;
    snprintf(what, sizeof(what), "stopped on its own %d times in %d min (last: %.120s)", recent,
             L.restart_window_s / 60, c->why);
    wd_observe(t, c->name, c->name, c->wd_group, WD_RESTARTS, recent >= L.restarts, what, 0);
    snprintf(what, sizeof(what), "channel failed: %s", c->why);
    wd_observe(t, c->name, c->name, c->wd_group, WD_FAILED, c->state == CH_FAILED, what, 0);
    snprintf(what, sizeof(what), "audio in the wrong language: %.150s", c->lang_note);
    wd_observe(t, c->name, c->name, c->wd_group, WD_LANGUAGE, c->state == CH_RUNNING && c->lang_wrong, what,
               c->lang_wrong ? c->lang_wrong_since : 0);
    if (c->state != CH_RUNNING)
        for (size_t i = 0; i < sizeof(running_rules) / sizeof(running_rules[0]); i++)
            wd_observe(t, c->name, c->name, c->wd_group, running_rules[i], false, NULL, 0);
}

static void chan_sample(struct super *s, struct chan *c, int64_t now)
{
    char *st;
    static struct json_doc doc;             /* main thread only */
    char err[100];
    int root, in, out;
    float bi = 0, bo = 0;

    if (c->state != CH_RUNNING || now - c->hist_last_ns < (int64_t)HIST_STEP_S * 1000000000LL)
        return;
    c->hist_last_ns = now;
    st = chan_stats(s, c);
    if (!st)
        return;
    root = json_parse(&doc, st, err, sizeof(err));
    if (root < 0) {
        free(st);
        return;
    }
    free(c->wd_stats);
    c->wd_stats = st;
    in = json_get(&doc, root, "input");
    out = json_get(&doc, root, "output");
    if (in >= 0)
        bi = (float)json_get_num(&doc, in, "bitrate", 0);
    if (out >= 0)
        bo = (float)json_get_num(&doc, out, "bitrate", 0);
    /* for the record: rates summed until the next 10 s row, counters as
     * they stand, and the changes worth an event */
    if (!c->h_have) {
        c->h_cc = c->h_ovf = c->h_drop = c->h_late = c->h_lost = c->h_sw = 0;
        c->h_no_input = false;
        c->h_backup = false;
        c->h_active = 0;
        c->h_have = true;
    }
    c->h_in_sum += bi;
    c->h_out_sum += bo;
    c->h_n++;
    if (in >= 0) {
        int link = json_get(&doc, in, "link");
        double age = json_get_num(&doc, in, "last_packet_age_ms", -1);
        double up = json_get_num(&doc, root, "uptime_s", 0);
        bool no_input = age > 2000 || (up > 10 && bi == 0);
        int active = (int)json_get_num(&doc, in, "active", 0);

        c->c_cc = (int64_t)json_get_num(&doc, in, "cc_errors", 0);
        c->c_ovf = (int64_t)json_get_num(&doc, in, "socket_overflow", 0);
        c->c_sw = (int64_t)json_get_num(&doc, in, "switches", 0);
        c->c_lost = link >= 0 ? (int64_t)json_get_num(&doc, link, "lost", 0) : 0;
        if (no_input != c->h_no_input && (up > 5 || no_input)) {
            if (no_input)
                ev(c->name, "warn", "input", "no input%s", age > 0 ? " for 2 s" : "");
            else
                ev(c->name, "info", "input", "input back");
            c->h_no_input = no_input;
        }
        if (active != c->h_active) {
            if (active)
                ev(c->name, "warn", "switch", "running on backup source %d", active);
            else
                ev(c->name, "info", "switch", "back on the main source");
            c->h_active = active;
        }
    }
    if (out >= 0) {
        c->c_drop = (int64_t)json_get_num(&doc, out, "dropped_units", 0);
        c->c_late = (int64_t)json_get_num(&doc, out, "late_units", 0);
    }
    wd_channel(c, &doc, root, bi, bo);
    {
        int k = (c->hist_head + c->hist_n) % HIST_N;

        if (c->hist_n == HIST_N)
            c->hist_head = (c->hist_head + 1) % HIST_N;
        else
            c->hist_n++;
        c->hist_in[k] = bi;
        c->hist_out[k] = bo;
    }
}

static char *chan_ctl(struct super *s, struct chan *c, const char *request);

/* The loopback output the preview reads when it cannot share the channel's
 * input, removed with the preview. */
static void preview_untap(struct super *s, struct chan *c)
{
    if (c->pv_tap) {
        c->pv_tap = false;
        if (c->state == CH_RUNNING)
            free(chan_ctl(s, c, "{\"cmd\": \"remove-output\", \"id\": \"pvtap\"}"));
    }
}

/* A multicast UDP/RTP input can be joined by a second receiver; a unicast
 * port cannot (the kernel hands each datagram to one socket), nor can an
 * SRT/RIST link. */
static bool input_shareable(const char *url)
{
    const char *h;
    char host[64];
    size_t n;
    struct in_addr a;

    if (strncmp(url, "udp://", 6) && strncmp(url, "rtp://", 6))
        return false;
    h = url + 6;
    if (*h == '@')
        h++;
    if (*h == '[')
        return !strncasecmp(h + 1, "ff", 2);
    n = strcspn(h, ":/?");
    if (n == 0 || n >= sizeof(host))
        return false;
    memcpy(host, h, n);
    host[n] = 0;
    return inet_pton(AF_INET, host, &a) == 1 && IN_MULTICAST(ntohl(a.s_addr));
}

static void preview_stop(struct chan *c)
{
    if (c->pv_pid > 0) {
        kill(c->pv_pid, SIGTERM);
        log_info("channel %s: preview stopped", c->name);
    }
}

static void preview_reap(struct super *s, struct chan *c)
{
    int status;

    if (c->pv_pid > 0 && waitpid(c->pv_pid, &status, WNOHANG) == c->pv_pid) {
        if (c->pv_log_fd >= 0) {
            /* its last words, for the API */
            char buf[512];
            ssize_t r = read(c->pv_log_fd, buf, sizeof(buf) - 1);

            if (r > 0) {
                buf[r] = 0;
                snprintf(c->pv_why, sizeof(c->pv_why), "%.119s", buf);
            }
            close(c->pv_log_fd);
            c->pv_log_fd = -1;
        }
        c->pv_pid = 0;
        preview_untap(s, c);
    }
}

/*
 * The preview process: the channel's source (its UDP input, or its first
 * UDP output when the input is a link only one process can hold) made
 * into 426x240 H.264 at 300 kb/s with AAC, served as HLS with 1 s
 * segments on run_dir/NAME.preview.http. `drop` names the audio streams
 * to leave out, so the player hears one.
 */
static bool preview_start(struct super *s, struct chan *c, const char *drop, char *err, size_t errlen)
{
    static struct json_doc doc;             /* under the supervisor's lock */
    char e[200];
    int root, a;
    const char *input;
    char src[SINK_URL_MAX] = "";
    int program = 0;
    char *argv[40];
    char store[8][SINK_URL_MAX];
    int n = 0, k = 0, pfd[2];
    pid_t pid;

    if (c->state != CH_RUNNING) {
        snprintf(err, errlen, "the channel is not running");
        return false;
    }
    if (!c->cfg || (root = json_parse(&doc, c->cfg, e, sizeof(e))) < 0 || !(input = json_get_str(&doc, root, "input", NULL))) {
        snprintf(err, errlen, "no input in the channel's config");
        return false;
    }
    if (input_shareable(input)) {
        snprintf(src, sizeof(src), "%s", input);
        program = (int)json_get_num(&doc, root, "program", 0);
    } else {
        /* the channel sends a copy of its output to a free loopback port */
        struct sockaddr_in sa;
        socklen_t sl = sizeof(sa);
        int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        char req[300], *reply;

        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
            getsockname(fd, (struct sockaddr *)&sa, &sl) < 0) {
            if (fd >= 0)
                close(fd);
            snprintf(err, errlen, "no loopback port for the preview: %s", strerror(errno));
            return false;
        }
        close(fd);
        snprintf(src, sizeof(src), "udp://127.0.0.1:%u", ntohs(sa.sin_port));
        free(chan_ctl(s, c, "{\"cmd\": \"remove-output\", \"id\": \"pvtap\"}"));
        snprintf(req, sizeof(req), "{\"cmd\": \"add-output\", \"url\": \"%s\", \"id\": \"pvtap\", \"save\": false}", src);
        reply = chan_ctl(s, c, req);
        if (!reply || !strstr(reply, "\"ok\": true")) {
            snprintf(err, errlen, "the channel did not take the preview's output%s%.100s", reply ? ": " : "", reply ? reply : "");
            free(reply);
            return false;
        }
        free(reply);
        c->pv_tap = true;
    }
#define ARGS(str) do { argv[n++] = (char *)(str); } while (0)
#define ARG(fmt, ...) do { snprintf(store[k], sizeof(store[k]), fmt, __VA_ARGS__); argv[n++] = store[k++]; } while (0)
    ARGS(s->binary);
    ARGS("-i");
    ARG("%s", src);
    if (program > 0)
        ARG("--program=%d", program);
    if (drop && drop[0])
        ARG("--drop=%s", drop);
    ARGS("--video");
    ARGS("426x240:h264:300k");
    ARGS("--audio");
    ARGS("aac:64k");
    ARGS("--gop");
    ARGS("1000");
    ARGS("--preset");
    ARGS("veryfast");
    ARGS("--delay");                        /* not the 3 s a channel keeps: quicker to start, */
    ARGS("1500");                           /* and a small picture encodes in far less */
    ARGS("--vbv");
    ARGS("500");
    a = (int)json_get_num(&doc, root, "gpu", -1);
    if (a >= 0)
        ARG("--gpu=%d", a);
    ARGS("--name");
    ARG("%s.preview", c->name);
    ARGS("--run-dir");
    ARGS(s->run_dir);
    ARGS("-o");
    ARGS("hls://?segment=1&window=5");
    ARGS("--stats-interval");
    ARGS("0");
    ARGS("-q");
    argv[n] = NULL;
#undef ARGS
#undef ARG
    if (pipe2(pfd, O_CLOEXEC | O_NONBLOCK) < 0) {
        snprintf(err, errlen, "pipe: %s", strerror(errno));
        preview_untap(s, c);
        return false;
    }
    pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        snprintf(err, errlen, "fork: %s", strerror(errno));
        preview_untap(s, c);
        return false;
    }
    if (pid == 0) {
        sigset_t set;

        sigemptyset(&set);
        sigprocmask(SIG_SETMASK, &set, NULL);
        signal(SIGHUP, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        dup2(pfd[1], 2);
        close(pfd[1]);
        close(pfd[0]);
        execv(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    c->pv_pid = pid;
    c->pv_log_fd = pfd[0];
    c->pv_started_ns = c->pv_last_hit_ns = clock_now_ns();
    snprintf(c->pv_drop, sizeof(c->pv_drop), "%s", drop ? drop : "");
    c->pv_why[0] = 0;
    log_info("channel %s: preview started (pid %d) from %s", c->name, (int)pid, src);
    return true;
}

/* ---- shared inputs ---------------------------------------------------------- */

static uint32_t text_hash(const char *t)
{
    uint32_t h = 5381;

    for (; t && *t; t++)
        h = h * 33 + (uint8_t)*t;
    return h;
}

/* Works out (again, when the file changed) whether the channel takes its
 * input through a share, and gives it a loopback port when it does. */
static void chan_share_prepare(struct super *s, struct chan *c)
{
    uint32_t h = text_hash(c->cfg);

    if (h != c->sh_hash || !c->sh_hash) {
        static struct json_doc doc;
        char e[200];
        int root = c->cfg ? json_parse(&doc, c->cfg, e, sizeof(e)) : -1;
        const char *in = root >= 0 ? json_get_str(&doc, root, "input", "") : "";

        c->sh_hash = h ? h : 1;
        c->sh_able = root >= 0 && (!strncmp(in, "udp://", 6) || !strncmp(in, "rtp://", 6)) &&
                     json_get_bool(&doc, root, "share_input", true);
        snprintf(c->sh_url, sizeof(c->sh_url), "%s", c->sh_able ? in : "");
        c->sh_program = root >= 0 ? (int)json_get_num(&doc, root, "program", 0) : 0;
    }
    if (!c->sh_able) {
        c->share_port = 0;
        return;
    }
    if (c->share_port == 0) {
        /* a free loopback port, kept for as long as the channel shares */
        struct sockaddr_in sa;
        socklen_t sl = sizeof(sa);
        int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (fd >= 0 && bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
            getsockname(fd, (struct sockaddr *)&sa, &sl) == 0)
            c->share_port = ntohs(sa.sin_port);
        if (fd >= 0)
            close(fd);
        for (int i = 0; i < s->n; i++)
            if (&s->ch[i] != c && s->ch[i].share_port == c->share_port)
                c->share_port = 0;          /* taken by another: tried again next time */
    }
}

static void share_stats_path(const struct super *s, int k, char *path, size_t len)
{
    snprintf(path, len, "%.400s/share-%d.json", s->run_dir, k);
}

/* One reader per input in use by channels that share; each told the
 * programs and ports it serves; started, told of changes (SIGHUP),
 * started again when it dies, stopped when nobody needs it. */
static void shares_update(struct super *s, int64_t now)
{
    for (int k = 0; k < SHARES_MAX; k++) {
        struct share *sh = &s->sh[k];
        int status;

        if (sh->pid > 0 && waitpid(sh->pid, &status, WNOHANG) == sh->pid) {
            if (!sh->stopping) {
                ev(NULL, "warn", "share", "shared input %s: reader ended, starting it again", sh->url);
                log_warn("shared input %s: reader ended (%s %d), starting it again", sh->url,
                         WIFSIGNALED(status) ? "signal" : "status",
                         WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
                sh->restarts++;
                sh->next_start_ns = now + 1000000000LL;
            }
            sh->pid = 0;
            sh->stopping = false;
            free(sh->cfg_text);             /* the next one is given its outputs afresh */
            sh->cfg_text = NULL;
        }
    }
    /* the inputs wanted */
    for (int i = 0; i < s->n; i++) {
        struct chan *c = &s->ch[i];
        bool found = false;
        int free_k = -1;

        if (!c->share_port || !(c->want || c->pid > 0))
            continue;
        for (int k = 0; k < SHARES_MAX; k++) {
            if (s->sh[k].used && !strcmp(s->sh[k].url, c->sh_url))
                found = true;
            if (!s->sh[k].used && s->sh[k].pid <= 0 && free_k < 0)
                free_k = k;
        }
        if (!found && free_k >= 0) {
            memset(&s->sh[free_k], 0, sizeof(s->sh[free_k]));
            s->sh[free_k].used = true;
            snprintf(s->sh[free_k].url, sizeof(s->sh[free_k].url), "%s", c->sh_url);
        } else if (!found) {
            log_err("channel %s: no room for another shared input (%d at most)", c->name, SHARES_MAX);
        }
    }
    for (int k = 0; k < SHARES_MAX; k++) {
        struct share *sh = &s->sh[k];
        char *text = NULL, path[600], q[1100];
        size_t len = 0;
        FILE *f;
        int n = 0;

        if (!sh->used)
            continue;
        f = open_memstream(&text, &len);
        if (!f)
            continue;
        fprintf(f, "{\"input\": %s, \"outputs\": [", json_quote(sh->url, q, sizeof(q)));
        for (int i = 0; i < s->n; i++) {
            const struct chan *c = &s->ch[i];

            if (c->share_port && (c->want || c->pid > 0) && !strcmp(c->sh_url, sh->url))
                fprintf(f, "%s\n  {\"program\": %d, \"port\": %d, \"channel\": %s}", n++ ? "," : "",
                        c->sh_program, c->share_port, json_quote(c->name, q, sizeof(q)));
        }
        fprintf(f, "\n]}\n");
        fclose(f);
        if (n == 0) {
            /* nobody left: stopped, and the slot freed once it is gone */
            if (sh->pid > 0 && !sh->stopping) {
                log_info("shared input %s: no channel uses it, stopping its reader", sh->url);
                kill(sh->pid, SIGTERM);
                sh->stopping = true;
            } else if (sh->pid <= 0) {
                char st[600];

                share_stats_path(s, k, st, sizeof(st));
                unlink(st);
                free(sh->cfg_text);
                sh->cfg_text = NULL;
                sh->used = false;
            }
            free(text);
            continue;
        }
        if (sh->stopping)
            continue;                       /* wanted again: started anew once the old one is gone */
        snprintf(path, sizeof(path), "%.400s/share-%d.cfg.json", s->run_dir, k);
        if (!sh->cfg_text || strcmp(sh->cfg_text, text)) {
            FILE *w = fopen(path, "w");

            if (w) {
                fputs(text, w);
                fclose(w);
            }
            free(sh->cfg_text);
            sh->cfg_text = text;
            text = NULL;
            if (sh->pid > 0)
                kill(sh->pid, SIGHUP);
        }
        free(text);
        if (sh->pid <= 0 && now >= sh->next_start_ns) {
            char stats[600];
            pid_t pid;

            share_stats_path(s, k, stats, sizeof(stats));
            pid = fork();
            if (pid == 0) {
                sigset_t set;

                sigemptyset(&set);
                sigprocmask(SIG_SETMASK, &set, NULL);
                signal(SIGHUP, SIG_DFL);
                signal(SIGINT, SIG_DFL);
                signal(SIGTERM, SIG_DFL);
                execl(s->binary, s->binary, "share", "--config", path, "--stats-file", stats, (char *)NULL);
                _exit(127);
            }
            if (pid > 0) {
                sh->pid = pid;
                log_info("shared input %s: reader started (pid %d) for %d channel(s)", sh->url, (int)pid, n);
                ev(NULL, "info", "share", "shared input %s: reader started for %d channel(s)", sh->url, n);
            }
        }
    }
}

static int64_t inc(int64_t now_v, int64_t *was)
{
    int64_t d = now_v >= *was ? now_v - *was : now_v;      /* a restarted channel counts from 0 */

    *was = now_v;
    return d;
}

static void metric_cb(const char *name, double v, void *op)
{
    int64_t t = *(int64_t *)op;
    struct wd_limits L;
    char subj[96], what[160];
    size_t n = strlen(name);

    history_metric(t, name, v);
    wd_get_limits(&L);
    if (!strcmp(name, "cpu.percent")) {
        snprintf(what, sizeof(what), "CPU at %.0f%%", v);
        wd_observe(t, "server/cpu", NULL, "", WD_CPU, v > L.cpu_pct, what, 0);
    } else if (!strncmp(name, "gpu", 3) && n > 8 && !strcmp(name + n - 8, ".encoder")) {
        snprintf(subj, sizeof(subj), "server/%.*s", (int)(n - 8), name);
        snprintf(what, sizeof(what), "%.*s video encoder at %.0f%%", (int)(n - 8), name, v);
        wd_observe(t, subj, NULL, "", WD_GPU, v > L.gpu_pct, what, 0);
    } else if (!strncmp(name, "gpu", 3) && n > 8 && !strcmp(name + n - 8, ".decoder")) {
        snprintf(subj, sizeof(subj), "server/%.*s.dec", (int)(n - 8), name);
        snprintf(what, sizeof(what), "%.*s video decoder at %.0f%%", (int)(n - 8), name, v);
        wd_observe(t, subj, NULL, "", WD_GPU, v > L.gpu_pct, what, 0);
    } else if (!strncmp(name, "net.", 4) && n > 12 && !strcmp(name + n - 8, ".rx_lost")) {
        /* the card had no room (missed, FIFO overrun): real loss. Not rx_dropped:
         * that counts frames the kernel set aside (no protocol for them, a VLAN
         * not configured), steady and harmless, seen at 0.5 a second for days */
        snprintf(subj, sizeof(subj), "server/%s", name);
        snprintf(what, sizeof(what), "network card %.*s losing packets: no room on the card (%.0f in 10 s)", (int)(n - 12),
                 name + 4, v);
        wd_observe(t, subj, NULL, "", WD_NIC_DROP, v > 0, what, 0);
    }
    /* (udp.rcvbuf_errors is kept as a figure, not judged: it counts every
     * program's sockets, Flussonic's among them. A channel's own socket
     * losing packets is its "loss" rule, from the socket itself.) */
}

/* Every 10 s of wall clock: a row per channel, and the server's figures. */
static void hist_tick(struct super *s)
{
    int64_t t = time(NULL) / 10 * 10;

    if (!history_enabled() || t == s->hist_last_s)
        return;
    s->hist_last_s = t;
    for (int i = 0; i < s->n; i++) {
        struct chan *c = &s->ch[i];
        struct hist_chan h;

        if (!c->cfg)
            continue;
        memset(&h, 0, sizeof(h));
        snprintf(h.name, sizeof(h.name), "%s", c->name);
        h.state = c->state == CH_RUNNING ? 1 : c->state == CH_FAILED ? 2 : 0;
        if (c->h_n) {
            h.in_bps = c->h_in_sum / c->h_n;
            h.out_bps = c->h_out_sum / c->h_n;
        }
        if (h.state == 1 && c->h_have) {
            h.cc = inc(c->c_cc, &c->h_cc);
            h.overflow = inc(c->c_ovf, &c->h_ovf);
            h.dropped = inc(c->c_drop, &c->h_drop);
            h.late = inc(c->c_late, &c->h_late);
            h.lost = inc(c->c_lost, &c->h_lost);
            h.switches = inc(c->c_sw, &c->h_sw);
        }
        h.restarts = c->restarts > c->h_restarts ? c->restarts - c->h_restarts : 0;
        c->h_restarts = c->restarts;
        h.health = h.state == 2 ? 2
                 : h.state == 1 && (c->h_no_input || c->h_active || h.cc || h.overflow || h.dropped || h.late ||
                                    h.lost) ? 1 : 0;
        history_chan(t, &h);
        c->h_in_sum = c->h_out_sum = 0;
        c->h_n = 0;
    }
    sysstat_metrics(metric_cb, &t);
}

/* ---- the evidence tools --------------------------------------------------------- */

/*
 * What can be looked at or kept about a problem, by the watchdog when an
 * incident opens or by a person (or, later, the technician) through the
 * API: a recording of the source (or, when the source cannot be read a
 * second time without harm, of the channel's output), a probe of the
 * source, a ping, the picture. Each runs on its own (a child process, or
 * an output of the channel for a while) and leaves a row in the history's
 * evidence table, with its file in the evidence folder (kept to its cap).
 */

/* A source that can be read again beside the channel without taking
 * anything from it: multicast (every listener gets every datagram), HTTP
 * (a connection of its own). Unicast UDP would have its datagrams shared
 * out between the two readers. */
static bool source_readable(const char *url)
{
    unsigned a = 0;

    if (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8))
        return true;
    if (strncmp(url, "udp://", 6) && strncmp(url, "rtp://", 6))
        return false;
    {
        const char *h = url + 6;

        if (*h == '@')
            h++;
        if (sscanf(h, "%u.", &a) != 1)
            return false;
    }
    return a >= 224 && a <= 239;
}

/* The channel's source as it really is (a shared input: the reader's URL). */
static void chan_source(const struct chan *c, char *url, size_t len, int *program)
{
    url[0] = 0;
    *program = 0;
    if (c->sh_url[0]) {
        snprintf(url, len, "%s", c->sh_url);
        *program = c->sh_program;
    } else if (c->wd_group[0]) {
        snprintf(url, len, "%s", c->wd_group);
    }
    if (!*program && c->cfg) {
        static struct json_doc doc;
        char err[100];
        int root = json_parse(&doc, c->cfg, err, sizeof(err));

        if (root >= 0) {
            *program = (int)json_get_num(&doc, root, "program", 0);
            if (!url[0])
                snprintf(url, len, "%s", json_get_str(&doc, root, "input", ""));
        }
    }
}

static int job_slot(struct super *s)
{
    for (int i = 0; i < JOBS_MAX; i++)
        if (!s->jobs[i].used)
            return i;
    return -1;
}

static bool evidence_on(const struct super *s)
{
    return s->evidence_dir[0] && strcmp(s->evidence_dir, "none") && history_enabled();
}

/* evidence/20261019-140501_bet_record.ts */
static void evidence_path(const struct super *s, const char *chan, const char *tool, const char *ext,
                          char *out, size_t len)
{
    time_t now = time(NULL);
    struct tm tm;
    char when[32], safe[64];
    size_t k = 0;

    localtime_r(&now, &tm);
    strftime(when, sizeof(when), "%Y%m%d-%H%M%S", &tm);
    for (const char *p = chan ? chan : "server"; *p && k < sizeof(safe) - 1; p++)
        safe[k++] = isalnum((unsigned char)*p) || *p == '-' || *p == '_' ? *p : '_';
    safe[k] = 0;
    mkdir(s->evidence_dir, 0755);
    snprintf(out, len, "%.400s/%s_%s_%s.%s", s->evidence_dir, when, safe, tool, ext);
    for (int n = 2; access(out, F_OK) == 0 && n < 100; n++)
        snprintf(out, len, "%.400s/%s_%s_%s-%d.%s", s->evidence_dir, when, safe, tool, n, ext);
}

/* A child whose standard output is kept in a file, its JSON the result. */
static bool job_spawn(struct super *s, const char *tool, const char *chan, int64_t incident, char *const argv[],
                      const char *file, const char *args, int timeout_s, char *err, size_t errlen)
{
    int k = job_slot(s), fd;
    struct job *j;
    pid_t pid;

    if (k < 0) {
        snprintf(err, errlen, "%d tools are running already", JOBS_MAX);
        return false;
    }
    j = &s->jobs[k];
    memset(j, 0, sizeof(*j));
    snprintf(j->out, sizeof(j->out), "%.500s/tool-%d.out", s->run_dir, k);
    fd = open(j->out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        snprintf(err, errlen, "%s: %s", j->out, strerror(errno));
        return false;
    }
    pid = fork();
    if (pid < 0) {
        close(fd);
        snprintf(err, errlen, "fork: %s", strerror(errno));
        return false;
    }
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);

        dup2(fd, 1);
        if (nul >= 0)
            dup2(nul, 2), dup2(nul, 0);
        for (int f = 3; f < 1024; f++)
            close(f);
        setpgid(0, 0);
        if (nice(5) < 0) {
            /* as it is */
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fd);
    j->used = true;
    j->pid = pid;
    snprintf(j->tool, sizeof(j->tool), "%s", tool);
    snprintf(j->chan, sizeof(j->chan), "%s", chan ? chan : "");
    snprintf(j->file, sizeof(j->file), "%s", file ? file : "");
    snprintf(j->args, sizeof(j->args), "%s", args ? args : "{}");
    j->incident = incident;
    j->started_ns = clock_now_ns();
    j->deadline_ns = j->started_ns + (int64_t)timeout_s * 1000000000LL;
    return true;
}

/* Reads `url` for `seconds` (a probe), and with `keep` records it too. */
static bool tool_probe(struct super *s, const char *chan, const char *url, int program, int seconds, bool keep,
                       int64_t incident, char *err, size_t errlen)
{
    char secs[16], prog[16], file[600] = "", args[700], q[600];
    char *argv[12];
    int n = 0;

    if (!source_readable(url)) {
        snprintf(err, errlen, "%s cannot be read beside the channel without taking from it "
                 "(only multicast and HTTP sources can)", url);
        return false;
    }
    if (keep && !evidence_on(s)) {
        snprintf(err, errlen, "no evidence is kept (see \"evidence\" in the config)");
        return false;
    }
    snprintf(secs, sizeof(secs), "%d", seconds);
    snprintf(prog, sizeof(prog), "%d", program);
    argv[n++] = s->binary;
    argv[n++] = "probe";
    argv[n++] = (char *)url;
    argv[n++] = "--seconds";
    argv[n++] = secs;
    if (program > 0) {
        argv[n++] = "--program";
        argv[n++] = prog;
    }
    if (keep) {
        evidence_path(s, chan, "source", "ts", file, sizeof(file));
        argv[n++] = "--save";
        argv[n++] = file;
    }
    argv[n] = NULL;
    snprintf(args, sizeof(args), "{\"url\": %s, \"seconds\": %d, \"program\": %d, \"from\": \"source\"}",
             json_quote(url, q, sizeof(q)), seconds, program);
    return job_spawn(s, keep ? "record" : "probe", chan, incident, argv, file, args, seconds + 25, err, errlen);
}

/* The channel's output for `seconds`, through a file output of its own. */
static bool tool_record_output(struct super *s, struct chan *c, int seconds, int64_t incident, char *err,
                               size_t errlen)
{
    int k = job_slot(s);
    char req[900], *reply, q[700], file[600];
    struct job *j;

    if (k < 0) {
        snprintf(err, errlen, "%d tools are running already", JOBS_MAX);
        return false;
    }
    if (!evidence_on(s)) {
        snprintf(err, errlen, "no evidence is kept (see \"evidence\" in the config)");
        return false;
    }
    if (c->state != CH_RUNNING) {
        snprintf(err, errlen, "the channel is not running");
        return false;
    }
    j = &s->jobs[k];
    memset(j, 0, sizeof(*j));
    evidence_path(s, c->name, "output", "ts", file, sizeof(file));
    snprintf(j->out, sizeof(j->out), "evrec%d", k);       /* the output's id */
    {
        char u[700];

        snprintf(u, sizeof(u), "file://%s", file);
        snprintf(req, sizeof(req), "{\"cmd\": \"add-output\", \"url\": %s, \"id\": \"%s\", \"save\": false}",
                 json_quote(u, q, sizeof(q)), j->out);
    }
    reply = chan_ctl(s, c, req);
    if (!reply || !strstr(reply, "\"ok\": true")) {
        snprintf(err, errlen, "the channel did not take the recording%s%.100s", reply ? ": " : "", reply ? reply : "");
        free(reply);
        return false;
    }
    free(reply);
    j->used = true;
    j->pid = 0;
    snprintf(j->tool, sizeof(j->tool), "record");
    snprintf(j->chan, sizeof(j->chan), "%s", c->name);
    snprintf(j->file, sizeof(j->file), "%s", file);
    snprintf(j->args, sizeof(j->args), "{\"seconds\": %d, \"from\": \"output\"}", seconds);
    j->incident = incident;
    j->started_ns = clock_now_ns();
    j->deadline_ns = j->started_ns + (int64_t)seconds * 1000000000LL;
    return true;
}

/* What the channel received from `before` seconds ago to `after` seconds on,
 * from what it keeps of its input (keep.h): the start of a fault is in it,
 * though the incident opened only once the fault had lasted. */
static bool tool_record_input(struct super *s, struct chan *c, int before, int after, int64_t incident, char *err,
                              size_t errlen)
{
    int k = job_slot(s);
    char req[900], *reply, q[700], file[600];
    struct job *j;

    if (k < 0) {
        snprintf(err, errlen, "%d tools are running already", JOBS_MAX);
        return false;
    }
    if (!evidence_on(s)) {
        snprintf(err, errlen, "no evidence is kept (see \"evidence\" in the config)");
        return false;
    }
    if (c->state != CH_RUNNING) {
        snprintf(err, errlen, "the channel is not running");
        return false;
    }
    j = &s->jobs[k];
    memset(j, 0, sizeof(*j));
    evidence_path(s, c->name, "source", "ts", file, sizeof(file));
    snprintf(req, sizeof(req), "{\"cmd\": \"keep-input\", \"path\": %s, \"before\": %d, \"after\": %d}",
             json_quote(file, q, sizeof(q)), before, after);
    reply = chan_ctl(s, c, req);
    if (!reply || !strstr(reply, "\"ok\": true")) {
        snprintf(err, errlen, "the channel did not save its input%s%.150s", reply ? ": " : "", reply ? reply : "");
        free(reply);
        return false;
    }
    free(reply);
    j->used = true;
    j->pid = 0;
    j->kept = true;
    snprintf(j->tool, sizeof(j->tool), "record");
    snprintf(j->chan, sizeof(j->chan), "%s", c->name);
    snprintf(j->file, sizeof(j->file), "%s", file);
    {
        time_t t0 = time(NULL) - before;
        struct tm tm;
        char when[32];

        localtime_r(&t0, &tm);
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
        snprintf(j->args, sizeof(j->args), "{\"from\": \"source\", \"kept\": true, \"before_s\": %d, "
                 "\"after_s\": %d, \"starts\": \"%s\", \"seconds\": %d}", before, after, when, before + after);
    }
    j->incident = incident;
    j->started_ns = clock_now_ns();
    j->deadline_ns = j->started_ns + (int64_t)(after + 4) * 1000000000LL;
    return true;
}

static bool host_ok(const char *h)
{
    if (!*h || strlen(h) > 253 || *h == '-')
        return false;
    for (; *h; h++)
        if (!isalnum((unsigned char)*h) && *h != '.' && *h != '-' && *h != ':')
            return false;
    return true;
}

static bool tool_ping(struct super *s, const char *host, int count, int64_t incident, char *err, size_t errlen)
{
    char cnt[8], args[400], q[300];
    char *argv[] = { "ping", "-n", "-c", cnt, "-W", "1", "-i", "0.3", (char *)host, NULL };

    if (!host_ok(host)) {
        snprintf(err, errlen, "not a host name or address: %.80s", host);
        return false;
    }
    snprintf(cnt, sizeof(cnt), "%d", count);
    snprintf(args, sizeof(args), "{\"host\": %s, \"count\": %d}", json_quote(host, q, sizeof(q)), count);
    return job_spawn(s, "ping", NULL, incident, argv, NULL, args, count + 10, err, errlen);
}

/* The channel's picture as it is now (its thumbnail), kept. */
static bool tool_snapshot(struct super *s, struct chan *c, int64_t incident, char *err, size_t errlen)
{
    char from[600], to[600];
    size_t len = 0;
    char *jpg;

    if (!evidence_on(s)) {
        snprintf(err, errlen, "no evidence is kept (see \"evidence\" in the config)");
        return false;
    }
    snprintf(from, sizeof(from), "%.500s/%s.jpg", s->run_dir, c->name);
    jpg = read_whole(from, &len, 16 << 20);
    if (!jpg || !len) {
        free(jpg);
        snprintf(err, errlen, "the channel has no picture yet");
        return false;
    }
    evidence_path(s, c->name, "picture", "jpg", to, sizeof(to));
    {
        FILE *f = fopen(to, "wb");

        if (!f || fwrite(jpg, 1, len, f) != len) {
            if (f)
                fclose(f);
            free(jpg);
            snprintf(err, errlen, "%.200s: %s", to, strerror(errno));
            return false;
        }
        fclose(f);
    }
    free(jpg);
    history_evidence_add(time(NULL), incident, c->name, "picture", "{}", "{}", to);
    return true;
}

/* ping's summary as JSON */
static void ping_result(const char *out, char *res, size_t len)
{
    const char *loss = strstr(out, "% packet loss");
    const char *rtt = strstr(out, "min/avg/max");
    double pl = -1, mn = -1, av = -1, mx = -1;

    if (loss) {
        const char *b = loss;

        while (b > out && (isdigit((unsigned char)b[-1]) || b[-1] == '.'))
            b--;
        pl = atof(b);
    }
    if (rtt && (rtt = strchr(rtt, '=')))
        sscanf(rtt + 1, " %lf/%lf/%lf", &mn, &av, &mx);
    snprintf(res, len, "{\"loss_pct\": %.0f, \"rtt_min_ms\": %.2f, \"rtt_avg_ms\": %.2f, \"rtt_max_ms\": %.2f}",
             pl, mn, av, mx);
}

/* Each tick: tools that are done leave their evidence. */
/* ---- the language check: Whisper on each channel's output (caricoder3 listen) --------- */

static const char *lang_name(const char *iso)
{
    static const struct { const char *iso, *name; } N[] = {
        { "eng", "English" }, { "spa", "Spanish" }, { "fra", "French" }, { "por", "Portuguese" }, { "deu", "German" },
        { "ita", "Italian" }, { "nld", "Dutch" }, { "hat", "Haitian Creole" }, { NULL, NULL }
    };

    for (int i = 0; N[i].iso; i++)
        if (!strcmp(N[i].iso, iso))
            return N[i].name;
    return iso;
}

/* What channel c should speak: its own "language", else the site's; ""
 * or "-": not checked. */
static void lang_expected(struct super *s, struct chan *c, char *out, size_t len)
{
    static struct json_doc doc;             /* main thread only */
    char err[100];
    int root = c->cfg ? json_parse(&doc, c->cfg, err, sizeof(err)) : -1;

    snprintf(out, len, "%s", root >= 0 && json_get(&doc, root, "language") >= 0
             ? json_get_str(&doc, root, "language", "") : s->lang_expect);
    if (!strcmp(out, "-"))
        out[0] = 0;
}

static bool lang_listen(struct super *s, struct chan *c, int64_t incident, bool keep, char *err, size_t errlen)
{
    char secs[16], file[600] = "", args[700], q[400];
    char *argv[10];
    int n = 0;

    if (!c->lang_url[0]) {
        snprintf(err, errlen, "no output of it can be read beside it (a multicast UDP output)");
        return false;
    }
    if (keep && !evidence_on(s))
        keep = false;
    snprintf(secs, sizeof(secs), "%d", s->lang_clip_s);
    argv[n++] = s->binary;
    argv[n++] = "listen";
    argv[n++] = c->lang_url;
    argv[n++] = "--seconds";
    argv[n++] = secs;
    if (keep) {
        evidence_path(s, c->name, "listen", "wav", file, sizeof(file));
        argv[n++] = "--save";
        argv[n++] = file;
    }
    argv[n] = NULL;
    snprintf(args, sizeof(args), "{\"url\": %s, \"seconds\": %d, \"from\": \"output\"}", json_quote(c->lang_url, q, sizeof(q)),
             s->lang_clip_s);
    if (!job_spawn(s, "listen", c->name, incident, argv, file, args, s->lang_clip_s + 120, err, errlen))
        return false;
    c->lang_busy = incident == 0;
    return true;
}

/* A check's answer (caricoder3 listen's JSON). */
static void lang_result(struct super *s, struct chan *c, const char *result)
{
    static struct json_doc doc;             /* main thread only */
    char err[100], want[8];
    int root = result ? json_parse(&doc, result, err, sizeof(err)) : -1;
    int64_t t = time(NULL);
    const char *heard;
    double p, vol;
    int every = s->lang_every_s > 0 ? s->lang_every_s : 6 * 3600;

    c->lang_busy = false;
    c->lang_t = t;
    lang_expected(s, c, want, sizeof(want));
    if (root < 0 || !json_get_bool(&doc, root, "ok", false)) {
        snprintf(c->lang_note, sizeof(c->lang_note), "could not listen: %.120s",
                 root >= 0 ? json_get_str(&doc, root, "error", "?") : "no answer");
        c->lang_due = t + (every < 1800 ? every : 1800);
        return;
    }
    heard = json_get_str(&doc, root, "language", "");
    p = json_get_num(&doc, root, "probability", 0);
    vol = json_get_num(&doc, root, "mean_volume_db", -91);
    c->lang_p = p;
    c->lang_vol = vol;
    if (p < s->lang_min_p || vol < -60) {
        /* music, silence, a few words: says little either way */
        snprintf(c->lang_note, sizeof(c->lang_note), "not sure (%s %.0f%%, sound at %.0f dB): heard again later",
                 lang_name(heard), p * 100, vol);
        c->lang_due = t + (every < 1800 ? every : 1800);
        return;
    }
    snprintf(c->lang_heard, sizeof(c->lang_heard), "%s", heard);
    if (!strcmp(heard, want)) {
        c->lang_wrong_n = 0;
        c->lang_wrong_since = 0;
        c->lang_wrong = false;
        snprintf(c->lang_note, sizeof(c->lang_note), "%s, as it should be (%.0f%% sure)", lang_name(heard), p * 100);
        c->lang_due = t + every;
        return;
    }
    if (!c->lang_wrong_n++)
        c->lang_wrong_since = t;
    /* Once is not enough, nor twice in a few minutes: an advertising break
     * can be in another language (seen on 23 Oct: Spanish for about 10
     * minutes on English channels). Wrong when it still is lang_confirm_s
     * after it was first heard; until then heard again in a minute, then
     * at that time. */
    if (c->lang_wrong_n >= 2 && t - c->lang_wrong_since >= s->lang_confirm_s)
        c->lang_wrong = true;
    snprintf(c->lang_note, sizeof(c->lang_note), "heard %s (%.0f%% sure), should be %s%s", lang_name(heard), p * 100,
             lang_name(want), c->lang_wrong ? "" : c->lang_wrong_n < 2 ? ": listening again in a minute"
             : ": listening again later (an advertising break can be in another language)");
    if (c->lang_wrong) {
        c->lang_due = t + (every < 600 ? every : 600);      /* while it is wrong, every 10 minutes */
    } else if (c->lang_wrong_n < 2) {
        c->lang_due = t + (every < 60 ? every : 60);
    } else {
        int64_t at = c->lang_wrong_since + s->lang_confirm_s, soon = t + (every < 60 ? every : 60);

        c->lang_due = at > soon ? at : soon;
    }
    log_info("%s: language check: %s", c->name, c->lang_note);
}

static void lang_tick(struct super *s)
{
    int64_t t = time(NULL), now = clock_now_ns();

    if (s->lang_every_s <= 0)
        return;
    if (t - s->lang_have_t >= 60) {
        const char *cli = getenv("CC3_WHISPER_CLI"), *model = getenv("CC3_WHISPER_MODEL");

        s->lang_have_t = t;
        s->lang_have = !access(cli ? cli : "/usr/local/bin/caricoder3-whisper-cli", X_OK) &&
                       !access(model ? model : "/var/lib/caricoder3/models/whisper/ggml-base.bin", R_OK);
    }
    if (!s->lang_have)
        return;
    for (int i = 0; i < JOBS_MAX; i++)
        if (s->jobs[i].used && !strcmp(s->jobs[i].tool, "listen"))
            return;                         /* one at a time */
    for (int i = 0; i < s->n; i++) {
        struct chan *c = &s->ch[i];
        char want[8], err[200];

        /* not in its first two minutes (the tests check more often than that) */
        int64_t warm = s->lang_every_s < 120 ? s->lang_every_s : 120;

        if (c->state != CH_RUNNING || now - c->started_ns < warm * 1000000000LL || t < c->lang_due || !c->lang_url[0])
            continue;
        lang_expected(s, c, want, sizeof(want));
        if (!want[0])
            continue;
        if (!lang_listen(s, c, 0, c->lang_wrong_n > 0, err, sizeof(err))) {
            snprintf(c->lang_note, sizeof(c->lang_note), "could not listen: %.120s", err);
            c->lang_due = t + 1800;
        }
        return;
    }
}

static void jobs_tick(struct super *s)
{
    int64_t now = clock_now_ns();

    for (int i = 0; i < JOBS_MAX; i++) {
        struct job *j = &s->jobs[i];
        int status;
        bool done = false, timed_out = false;

        if (!j->used)
            continue;
        if (j->pid > 0) {
            if (waitpid(j->pid, &status, WNOHANG) == j->pid)
                done = true;
            else if (now > j->deadline_ns) {
                kill(-j->pid, SIGKILL);
                kill(j->pid, SIGKILL);
                waitpid(j->pid, &status, 0);
                done = timed_out = true;
            }
            if (!done)
                continue;
            {
                size_t len = 0;
                char *out = read_whole(j->out, &len, 8 << 20);
                char res[700];
                const char *result = out && out[0] == '{' ? out : NULL;

                if (!timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 127) {
                    snprintf(res, sizeof(res), "{\"ok\": false, \"error\": \"%s is not installed on this server\"}",
                             !strcmp(j->tool, "ping") ? "ping" : "the tool");
                    result = res;
                } else if (!strcmp(j->tool, "ping")) {
                    ping_result(out ? out : "", res, sizeof(res));
                    result = res;
                } else if (!result) {
                    snprintf(res, sizeof(res), "{\"ok\": false, \"error\": \"%s\"}",
                             timed_out ? "took too long" : "no answer");
                    result = res;
                }
                if (j->file[0] && access(j->file, F_OK) != 0)
                    j->file[0] = 0;
                if (!strcmp(j->tool, "listen") && !j->incident) {
                    /* the language check: its answer goes to the channel; a clip
                     * kept (a second hearing) goes to the evidence */
                    struct chan *c = find_chan(s, j->chan);

                    if (c)
                        lang_result(s, c, result);
                    if (j->file[0])
                        history_evidence_add(time(NULL), 0, j->chan, j->tool, j->args, result, j->file);
                } else {
                    history_evidence_add(time(NULL), j->incident, j->chan[0] ? j->chan : NULL, j->tool, j->args,
                                         result, j->file);
                }
                free(out);
                unlink(j->out);
            }
            j->used = false;
        } else if (j->kept && now > j->deadline_ns) {
            /* the channel's kept input: written by the channel, with a note beside it */
            char side[700], res[400];
            char *note;

            snprintf(side, sizeof(side), "%s.json", j->file);
            note = read_whole(side, NULL, 4096);
            if (note && note[0] == '{') {
                char *e = note + strlen(note);

                while (e > note && (e[-1] == '\n' || e[-1] == ' '))
                    *--e = 0;
                snprintf(res, sizeof(res), "%s", note);
            } else {
                snprintf(res, sizeof(res), "{\"ok\": false, \"error\": \"the channel wrote nothing\"}");
            }
            free(note);
            unlink(side);
            history_evidence_add(time(NULL), j->incident, j->chan, "record", j->args, res,
                                 access(j->file, F_OK) == 0 ? j->file : "");
            j->used = false;
        } else if (now > j->deadline_ns) {
            /* an output recording: the output goes, the file stays */
            struct chan *c = find_chan(s, j->chan);
            char req[200], res[200];
            struct stat sb;

            snprintf(req, sizeof(req), "{\"cmd\": \"remove-output\", \"id\": \"%s\"}", j->out);
            if (c && c->state == CH_RUNNING)
                free(chan_ctl(s, c, req));
            snprintf(res, sizeof(res), "{\"ok\": %s, \"bytes\": %lld}", stat(j->file, &sb) == 0 ? "true" : "false",
                     stat(j->file, &sb) == 0 ? (long long)sb.st_size : 0LL);
            history_evidence_add(time(NULL), j->incident, j->chan, "record", j->args, res,
                                 access(j->file, F_OK) == 0 ? j->file : "");
            j->used = false;
        }
    }
}

static void jobs_json(const struct super *s, FILE *f)
{
    int n = 0;
    char q[200];

    fprintf(f, "{\"running\": [");
    for (int i = 0; i < JOBS_MAX; i++) {
        const struct job *j = &s->jobs[i];

        if (!j->used)
            continue;
        fprintf(f, "%s{\"tool\": \"%s\", \"channel\": %s, \"incident\": %lld, \"args\": %s, \"for_s\": %.0f}",
                n++ ? ", " : "", j->tool, json_quote(j->chan, q, sizeof(q)), (long long)j->incident, j->args,
                (double)(clock_now_ns() - j->started_ns) / 1e9);
    }
    fprintf(f, "]}");
}

/*
 * What is gathered when an incident opens, without being asked:
 *   picture and sound problems, a stream without data, CC errors, PID
 *   changes, the clock, a backup source: the picture, and 20 s of the
 *   source (of the output when the source cannot be read twice);
 *   no input: a probe of the source from here (does anything arrive at
 *   all?), once per source a minute.
 * A channel gets at most one recording in ten minutes.
 */
static void on_incident(int64_t id, const char *chan, const char *group, enum wd_rule r, void *op)
{
    struct super *s = op;
    struct chan *c = chan ? find_chan(s, chan) : NULL;
    char err[300], url[512];
    int program = 0;
    int64_t t = time(NULL);

    /* the technician looks at it once the evidence is in */
    tech_queue(id, false);
    if (!evidence_on(s) || !s->evidence_auto)
        return;
    switch (r) {
    case WD_BLACK: case WD_FROZEN: case WD_SILENT: case WD_NO_DATA: case WD_CC: case WD_PIDS:
    case WD_CLOCK: case WD_BACKUP: case WD_AV_SHIFT:
        if (!c)
            return;
        if (r == WD_BLACK || r == WD_FROZEN || r == WD_AV_SHIFT)
            tool_snapshot(s, c, id, err, sizeof(err));
        if (t - c->ev_last < 600)
            return;
        c->ev_last = t;
        chan_source(c, url, sizeof(url), &program);
        /* what the channel kept: from a minute back, so that the start is in it
         * (a picture or sound fault opens an incident only after content_s) */
        if (!tool_record_input(s, c, 60, 20, id, err, sizeof(err)) &&
            !(source_readable(url) && tool_probe(s, c->name, url, program, 20, true, id, err, sizeof(err))) &&
            !tool_record_output(s, c, 20, id, err, sizeof(err)))
            log_info("evidence for %s: %s", c->name, err);
        break;
    case WD_LANGUAGE:
        /* a clip of what it says now, kept for a person to listen to */
        if (c && !lang_listen(s, c, id, true, err, sizeof(err)))
            log_info("evidence for %s: %s", c->name, err);
        break;
    case WD_NO_INPUT:
        if (c)
            chan_source(c, url, sizeof(url), &program);
        else
            snprintf(url, sizeof(url), "%s", group ? group : "");
        if (!url[0] || !source_readable(url))
            return;
        for (int k = 0; k < 8; k++)
            if (!strcmp(s->probe_url[k], url) && t - s->probe_t[k] < 60)
                return;
        {
            int k = 0;

            for (int i = 1; i < 8; i++)
                if (s->probe_t[i] < s->probe_t[k])
                    k = i;
            snprintf(s->probe_url[k], sizeof(s->probe_url[k]), "%s", url);
            s->probe_t[k] = t;
        }
        if (!tool_probe(s, chan, url, 0, 3, false, id, err, sizeof(err)))
            log_info("evidence for %s: %s", chan ? chan : url, err);
        {
            /* and from another place: the peers on the same network */
            char why[200];

            if (chan)
                snprintf(why, sizeof(why), "My channel %.60s lost its input", chan);
            else
                snprintf(why, sizeof(why), "Several of my channels lost their input at once");
            peer_ask_check(url, program, 3, id, why);
        }
        break;
    default:
        break;
    }
}

static bool history_incident_exists(int64_t id)
{
    char *d, *v, *n, *a;
    bool ok = history_incident_answer(id, &d, &v, &n, &a);

    free(d), free(v), free(n), free(a);
    return ok;
}

/* A person's word on the technician's answer for incident `id`. */
static bool apply_verdict(const struct json_doc *doc, int root, int64_t id, char *err, size_t errlen)
{
    static const char *const cats[] = { "", "source_provider", "source_ird", "satellite_signal", "network",
                                        "transcoder", "normal_content", "unknown" };
    char actual[1200], q1[700], q2[700], text[900];
    const char *v = json_get_str(doc, root, "verdict", ""), *cat = json_get_str(doc, root, "category", "");
    const char *cause = json_get_str(doc, root, "cause", ""), *note = json_get_str(doc, root, "note", "");
    bool cat_ok = false;

    for (size_t k = 0; k < sizeof(cats) / sizeof(cats[0]); k++)
        cat_ok |= !strcmp(cat, cats[k]);
    if ((strcmp(v, "right") && strcmp(v, "partly") && strcmp(v, "wrong") && strcmp(v, "exclude") && strcmp(v, "none"))
        || !cat_ok) {
        snprintf(err, errlen, "verdict: right, partly, wrong, exclude (not a real case) or none (taken back);"
                 " category: one of the technician's");
        return false;
    }
    snprintf(actual, sizeof(actual), "{\"category\": %s, \"cause\": %s}", json_quote(cat, q1, sizeof(q1)),
             json_quote(cause, q2, sizeof(q2)));
    history_incident_verdict(id, time(NULL), v, note, *cat || *cause ? actual : "{}");
    snprintf(text, sizeof(text), "%s%s%.300s%s%.300s",
             !strcmp(v, "right") ? "Right." : !strcmp(v, "partly") ? "Partly." : !strcmp(v, "exclude")
             ? "Not a real case: not to learn from." : !strcmp(v, "none") ? "Verdict taken back." : "Wrong.",
             *cause ? " It was: " : "", cause, *note ? " " : "", note);
    history_message(time(NULL), "person", "in", text, "{}", id);
    return true;
}

/* ---- the peers: answering them, and telling ours about them ------------------------- */

static void api_error(struct http_resp *resp, int status, const char *msg);

/* A probe run here and waited for (a peer's question): its JSON, malloc'd. */
static char *probe_sync(struct super *s, const char *url, int program, int seconds)
{
    int pfd[2];
    pid_t pid;
    char secs[16], prog[16];
    char *argv[] = { s->binary, "probe", (char *)url, "--seconds", secs, program > 0 ? "--program" : NULL, prog, NULL };
    char *buf = NULL;
    size_t len = 0, cap = 0;
    int64_t deadline = clock_now_ns() + (int64_t)(seconds + 12) * 1000000000LL;

    snprintf(secs, sizeof(secs), "%d", seconds);
    snprintf(prog, sizeof(prog), "%d", program);
    if (pipe2(pfd, O_CLOEXEC) < 0)
        return NULL;
    pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return NULL;
    }
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);

        dup2(pfd[1], 1);
        if (nul >= 0)
            dup2(nul, 2), dup2(nul, 0);
        execv(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    for (;;) {
        struct pollfd p = { .fd = pfd[0], .events = POLLIN };
        int64_t left = (deadline - clock_now_ns()) / 1000000;
        ssize_t r;

        if (left <= 0 || poll(&p, 1, (int)left) <= 0)
            break;
        if (len + 4096 + 1 > cap) {
            char *nb = realloc(buf, cap ? cap * 2 : 16384);

            if (!nb)
                break;
            buf = nb;
            cap = cap ? cap * 2 : 16384;
        }
        r = read(pfd[0], buf + len, cap - len - 1);
        if (r <= 0)
            break;
        len += (size_t)r;
    }
    close(pfd[0]);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    if (buf)
        buf[len] = 0;
    return buf;
}

static bool peer_authorized(struct super *s, const struct http_req *req)
{
    size_t n = strlen(s->peer_key);
    const char *k = req->auth + 9;
    unsigned diff = 0;

    if (!n || strncmp(req->auth, "CC3-Peer ", 9) || strlen(k) != n)
        return false;
    for (size_t i = 0; i < n; i++)
        diff |= (unsigned)(k[i] ^ s->peer_key[i]);
    return diff == 0;
}

/* /api/peer/...: what another transcoder may ask (with the shared key) */
static void handle_peer(struct super *s, const struct http_req *req, struct http_resp *resp, const char *p)
{
    char q[300], qq[700];

    if (!strcmp(p, "/api/peer/hello")) {
        int running = 0, open_n = 0;
        struct wd_limits L;
        char *w = NULL;
        size_t wl = 0;
        FILE *wf = open_memstream(&w, &wl);
        double cpu = sysstat_cpu_percent();

        if (wf) {
            wd_state_json(wf, time(NULL));
            fclose(wf);
        }
        for (const char *x = w; x && (x = strstr(x, "\"incident\": ")); x += 12)
            open_n += atoi(x + 12) > 0;
        free(w);
        wd_get_limits(&L);
        pthread_mutex_lock(&s->lock);
        for (int i = 0; i < s->n; i++)
            running += s->ch[i].state == CH_RUNNING;
        http_resp_printf(resp, 200, "application/json",
                         "{\"ok\": true, \"name\": %s, \"version\": \"%s\", \"uptime_s\": %lld, \"channels\": %d, "
                         "\"running\": %d, \"open_incidents\": %d, \"cpu\": %.0f, \"busy\": %s}\n",
                         json_quote(s->name, q, sizeof(q)), CC3_VERSION, (long long)(time(NULL) - s->t_start), s->n,
                         running, open_n, cpu, cpu > L.cpu_pct ? "true" : "false");
        pthread_mutex_unlock(&s->lock);
        return;
    }
    if (!strcmp(p, "/api/peer/check") && !strcmp(req->method, "POST")) {
        struct json_doc *doc = malloc(sizeof(*doc));
        char err[200], url[512], from[64], why[200], say[400], *out = NULL;
        int root = doc ? json_parse(doc, req->body ? req->body : "{}", err, sizeof(err)) : -1;
        int program, seconds;
        bool seen = false;
        double bps = 0;
        int services = 0;
        char perr[200] = "";

        if (root < 0) {
            free(doc);
            api_error(resp, 400, "bad request");
            return;
        }
        snprintf(url, sizeof(url), "%s", json_get_str(doc, root, "url", ""));
        snprintf(from, sizeof(from), "%s", json_get_str(doc, root, "from", "a peer"));
        snprintf(why, sizeof(why), "%s", json_get_str(doc, root, "why", ""));
        program = (int)json_get_num(doc, root, "program", 0);
        seconds = (int)json_get_num(doc, root, "seconds", 3);
        free(doc);
        if (seconds < 1 || seconds > 10)
            seconds = 3;
        {
            char text[800];

            snprintf(text, sizeof(text), "%.60s asks: %.200s. Can I check %.400s from here?", from,
                     why[0] ? why : "a problem", url);
            history_message(time(NULL), from, "in", text, "{}", 0);
        }
        if (!source_readable(url)) {
            snprintf(say, sizeof(say), "I cannot look at %.250s without taking it from whoever reads it "
                     "(only multicast and HTTP sources can be read twice).", url);
        } else if ((out = probe_sync(s, url, program, seconds))) {
            static struct json_doc pd;          /* web threads: under the lock below */
            int pr;

            pthread_mutex_lock(&s->lock);
            pr = json_parse(&pd, out, err, sizeof(err));
            if (pr >= 0 && json_get_bool(&pd, pr, "ok", false)) {
                int progs = json_get(&pd, pr, "programs");

                bps = json_get_num(&pd, pr, "bitrate", 0);
                for (int j = progs >= 0 ? pd.node[progs].first_child : -1; j >= 0; j = pd.node[j].next)
                    services++;
                seen = bps > 0;
            } else if (pr >= 0) {
                snprintf(perr, sizeof(perr), "%.190s", json_get_str(&pd, pr, "error", "nothing received"));
            } else {
                snprintf(perr, sizeof(perr), "the probe said something strange");
            }
            pthread_mutex_unlock(&s->lock);
            if (seen)
                snprintf(say, sizeof(say), "I see it here: %.1f Mb/s, %d service%s, in %d s.", bps / 1e6, services,
                         services == 1 ? "" : "s", seconds);
            else
                snprintf(say, sizeof(say), "Nothing from it here either: %s.", perr[0] ? perr : "nothing received");
        } else {
            snprintf(say, sizeof(say), "I could not run the probe here.");
        }
        free(out);
        history_message(time(NULL), from, "out", say, "{}", 0);
        http_resp_printf(resp, 200, "application/json",
                         "{\"ok\": true, \"seen\": %s, \"bitrate\": %.0f, \"services\": %d, \"error\": %s, \"say\": %s}\n",
                         seen ? "true" : "false", bps, services, json_quote(perr, q, sizeof(q)),
                         json_quote(say, qq, sizeof(qq)));
        return;
    }
    if (!strcmp(p, "/api/peer/incidents")) {
        char *out = NULL;
        size_t len = 0;
        FILE *f = open_memstream(&out, &len);
        int64_t now = time(NULL);

        if (!f)
            return;
        if (!history_query_incidents(f, NULL, now - 86400, now + 1, true, 100))
            fprintf(f, "{\"incidents\": []}");
        fclose(f);
        resp->status = 200;
        resp->type = "application/json";
        resp->body = out;
        resp->body_len = len;
        return;
    }
    api_error(resp, 404, "no such peer request");
}

static const char *state_name(enum chan_state st);
static void disks_json(const struct super *s, FILE *f);

/* How things are now, for a question to the technician (tech.h). */
/* The incidents in plain lines for the chat, ahead of the JSON: what is
 * open now, apart from what is over, with ages in minutes (a small model
 * neither subtracts Unix times nor keeps the two apart reliably). */
static void chat_incident_lines(FILE *f, int64_t now, bool *first)
{
    for (int pass = 0; pass < 2; pass++) {
        char *buf = NULL;
        size_t len = 0;
        FILE *m = open_memstream(&buf, &len);
        struct json_doc *d = malloc(sizeof(*d));
        char err[100], line[700], q[800];
        int root = -1, shown = 0, total = 0;

        if (!m || !d) {
            if (m)
                fclose(m);
            free(buf);
            free(d);
            return;
        }
        if (!history_query_incidents_brief(m, pass ? now - 86400 : 0, now + 1, pass == 0, pass ? 60 : 30))
            fprintf(m, "{}");
        fclose(m);
        root = buf && strlen(buf) < JSON_TEXT_MAX ? json_parse(d, buf, err, sizeof(err)) : -1;
        free(buf);
        if (root >= 0) {
            int arr = json_get(d, root, "incidents");

            for (int j = arr >= 0 ? d->node[arr].first_child : -1; j >= 0; j = d->node[j].next) {
                bool open = json_get(d, j, "closed") < 0 || d->node[json_get(d, j, "closed")].type == JSON_NULL;
                double opened = json_get_num(d, j, "opened", 0), closed = json_get_num(d, j, "closed", 0);
                const char *ch = json_get_str(d, j, "channel", "");
                int parent = json_get(d, j, "parent");

                if (pass == 1 && open)
                    continue;
                if (parent >= 0 && d->node[parent].type != JSON_NULL)
                    continue;               /* part of a source's: that one is listed */
                total++;
                if (shown >= (pass ? 12 : 30))
                    continue;
                shown++;
                if (pass == 0)
                    snprintf(line, sizeof(line), "OPEN NOW, for %.0f min: %s (%s; incident %.0f)", (now - opened) / 60,
                             json_get_str(d, j, "summary", "?"), ch[0] ? ch : "the server or a source",
                             json_get_num(d, j, "id", 0));
                else
                    snprintf(line, sizeof(line), "over, ended %.0f min ago after %.0f s: %s (%s)", (now - closed) / 60,
                             closed - opened, json_get_str(d, j, "summary", "?"), ch[0] ? ch : "the server or a source");
                fprintf(f, "%s%s", *first ? "" : ", ", json_quote(line, q, sizeof(q)));
                *first = false;
            }
        }
        if (pass == 0 && !total) {
            fprintf(f, "%s\"Nothing is open now: no incident is going on.\"", *first ? "" : ", ");
            *first = false;
        }
        if (pass == 1) {
            snprintf(line, sizeof(line), "%d incident%s ended in the last 24 hours%s", total, total == 1 ? "" : "s",
                     total > shown ? " (the latest shown above)" : "");
            fprintf(f, "%s%s", *first ? "" : ", ", json_quote(line, q, sizeof(q)));
            *first = false;
        }
        free(d);
    }
}

static void tech_context(FILE *f, void *op)
{
    struct super *s = op;
    char q[700];
    int64_t now = time(NULL);
    struct peer_state ps[PEERS_MAX];
    int np = peer_get(ps, PEERS_MAX);
    bool first = true;

    fprintf(f, "{\"incidents_in_plain_words\": [");
    chat_incident_lines(f, now, &first);
    fprintf(f, "], \"now\": %lld, \"channels\": [", (long long)now);
    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < s->n; i++) {
        const struct chan *c = &s->ch[i];
        int last = c->hist_n ? (c->hist_head + c->hist_n - 1) % HIST_N : -1;

        if (!c->cfg)
            continue;
        fprintf(f, "%s{\"name\": %s, \"state\": \"%s\"", i ? ", " : "", json_quote(c->name, q, sizeof(q)),
                state_name(c->state));
        fprintf(f, ", \"source\": %s", json_quote(c->wd_group, q, sizeof(q)));
        if (c->state == CH_RUNNING && last >= 0)
            fprintf(f, ", \"in_kbps\": %.0f, \"out_kbps\": %.0f, \"up_s\": %lld", c->hist_in[last] / 1000,
                    c->hist_out[last] / 1000, (long long)((clock_now_ns() - c->started_ns) / 1000000000LL));
        if (c->state != CH_RUNNING && c->why[0])
            fprintf(f, ", \"why\": %s", json_quote(c->why, q, sizeof(q)));
        if (c->restarts)
            fprintf(f, ", \"restarts\": %d", c->restarts);
        if (c->mem && c->state == CH_RUNNING) {
            double r, cm;

            mw_last(c->mem, &r, &cm);
            if (r >= 0)
                fprintf(f, ", \"memory_mb\": %.0f", r);
            if (cm > 0)
                fprintf(f, ", \"card_memory_mb\": %.0f", cm);
        }
        fputc('}', f);
    }
    fprintf(f, "], \"disks\": ");
    disks_json(s, f);
    pthread_mutex_unlock(&s->lock);
    fprintf(f, ", \"peers\": [");
    for (int i = 0; i < np; i++) {
        fprintf(f, "%s{\"name\": %s, \"answering\": %s, \"open_incidents\": %d}", i ? ", " : "",
                json_quote(ps[i].name, q, sizeof(q)), ps[i].reachable ? "true" : "false", ps[i].open_incidents);
    }
    fprintf(f, "], \"recent_events\": ");
    if (!history_query_events(f, NULL, now - 1800, now + 1, 40))
        fprintf(f, "{}");
    fputc('}', f);
}

/* Our peers as configured, to the peer thread. */
static void peers_apply(struct super *s)
{
    peer_set(s->peers, s->n_peers, s->name, s->peer_key);
}

static void peers_json(struct super *s, FILE *f)
{
    struct peer_state st[PEERS_MAX];
    int n = peer_get(st, PEERS_MAX);
    char q[600];

    fprintf(f, "{\"ok\": true, \"name\": %s, ", json_quote(s->name, q, sizeof(q)));
    fprintf(f, "\"timezone\": %s, \"key_set\": %s, \"peers\": [", json_quote(s->timezone, q, sizeof(q)),
            s->peer_key[0] ? "true" : "false");
    for (int i = 0; i < n; i++) {
        fprintf(f, "%s{\"name\": %s", i ? ", " : "", json_quote(st[i].name, q, sizeof(q)));
        fprintf(f, ", \"url\": %s", json_quote(st[i].url, q, sizeof(q)));
        fprintf(f, ", \"reachable\": %s, \"last_ok\": %lld, \"version\": %s", st[i].reachable ? "true" : "false",
                (long long)st[i].last_ok, json_quote(st[i].version, q, sizeof(q)));
        fprintf(f, ", \"host\": %s, \"channels\": %d, \"running\": %d, \"open_incidents\": %d, \"cpu\": %.0f",
                json_quote(st[i].host, q, sizeof(q)), st[i].channels, st[i].running, st[i].open_incidents, st[i].cpu);
        fprintf(f, ", \"error\": %s}", json_quote(st[i].error, q, sizeof(q)));
    }
    fprintf(f, "], \"messages\": ");
    history_query_messages(f, 0, 60);
    fprintf(f, "}\n");
}

/* Every 10 s: how full the disks are, the rules about it, and room made
 * in what is ours when one is nearly full. */
static void disk_tick(struct super *s)
{
    int64_t t = time(NULL);
    const char *paths[40];
    char hist_dir[512] = "";
    int np = 0;
    struct wd_limits L;
    bool hist_critical = false, hist_ok = true;

    if (t - s->disk_last < 10)
        return;
    s->disk_last = t;
    wd_get_limits(&L);
    /* what to look at: ours, and where the outputs write */
    static struct disk_path old[32];
    int n_old = s->n_dpath;

    memcpy(old, s->dpath, sizeof(old));
    s->n_dpath = 0;
#define DPATH(p_, w_) do { if (s->n_dpath < 32 && (p_)[0]) { snprintf(s->dpath[s->n_dpath].path, sizeof(s->dpath[0].path), "%.255s", p_); \
        snprintf(s->dpath[s->n_dpath].what, sizeof(s->dpath[0].what), "%s", w_); s->dpath[s->n_dpath].bytes = -1; s->n_dpath++; } } while (0)
    if (history_enabled()) {
        char *sl;

        snprintf(hist_dir, sizeof(hist_dir), "%s", s->history_path);
        if ((sl = strrchr(hist_dir, '/')) && sl != hist_dir)
            *sl = 0;
        DPATH(s->history_path, "history");
    }
    DPATH(s->run_dir, "run folder");
    if (evidence_on(s))
        DPATH(s->evidence_dir, "evidence");
    if (access("/var/log/journal", F_OK) == 0)
        DPATH("/var/log/journal", "system log (journald)");
    DPATH(s->channels_dir, "channel settings");
    for (int i = 0; i < s->n; i++)
        for (int k = 0; k < s->ch[i].n_out_dirs; k++) {
            bool dup = false;

            for (int j = 0; j < s->n_dpath; j++)
                dup |= !strcmp(s->dpath[j].path, s->ch[i].out_dirs[k]);
            if (!dup)
                DPATH(s->ch[i].out_dirs[k], "output folder");
        }
#undef DPATH
    for (int j = 0; j < s->n_dpath; j++) {
        paths[np++] = s->dpath[j].path;
        for (int k = 0; k < n_old; k++)
            if (!strcmp(old[k].path, s->dpath[j].path))
                s->dpath[j].bytes = old[k].bytes;   /* measured once a minute: kept between */
    }
    s->n_disks = disk_list(paths, np, s->disks, DISK_MAX);
    /* sizes of the folders, now and then (a walk) */
    if (t - s->du_last >= 60) {
        s->du_last = t;
        for (int j = 0; j < s->n_dpath; j++)
            s->dpath[j].bytes = !strcmp(s->dpath[j].what, "history") ? history_bytes()
                                                                      : disk_du(s->dpath[j].path, 20000);
    }
    for (int k = 0; k < s->n_disks; k++) {
        struct disk_fs *fs = &s->disks[k];
        struct disk_track *tr = NULL;
        double free_pct = fs->total ? 100.0 * (double)fs->avail / (double)fs->total : 100;
        uint64_t used = fs->total - fs->avail;
        char subj[160], what[220], name[96];
        double growth = 0, full_h = -1;

        for (int i = 0; i < DISK_MAX && !tr; i++)
            if (!strcmp(s->dtr[i].mount, fs->mount))
                tr = &s->dtr[i];
        for (int i = 0; i < DISK_MAX && !tr; i++)
            if (!s->dtr[i].mount[0]) {
                tr = &s->dtr[i];
                snprintf(tr->mount, sizeof(tr->mount), "%s", fs->mount);
            }
        if (tr) {
            int last = (tr->head + tr->n - 1 + 61) % 61;

            if (!tr->n || t - tr->t[last] >= 60) {
                int idx = (tr->head + tr->n) % 61;

                if (tr->n == 61)
                    tr->head = (tr->head + 1) % 61;
                else
                    tr->n++;
                tr->t[idx] = t;
                tr->used[idx] = used;
            }
            if (tr->n > 1 && t - tr->t[tr->head] >= 600) {
                growth = ((double)used - (double)tr->used[tr->head]) / ((double)(t - tr->t[tr->head]) / 3600.0);
                if (growth > 0)
                    full_h = (double)fs->avail / growth;
            }
        }
        snprintf(subj, sizeof(subj), "server/disk:%s", fs->mount);
        snprintf(name, sizeof(name), "%s", fs->mount);
        snprintf(what, sizeof(what), "disk %s: %.1f%% free (%.1f GB of %.1f GB)", name, free_pct,
                 (double)fs->avail / 1e9, (double)fs->total / 1e9);
        wd_observe(t, subj, NULL, "", WD_DISK_CRITICAL, free_pct < L.disk_crit_pct, what, 0);
        wd_observe(t, subj, NULL, "", WD_DISK_LOW, free_pct < L.disk_warn_pct && free_pct >= L.disk_crit_pct, what, 0);
        snprintf(what, sizeof(what), "disk %s filling: %.1f GB an hour, full in about %.0f h", name, growth / 1e9,
                 full_h);
        wd_observe(t, subj, NULL, "", WD_DISK_FILLING, full_h >= 0 && full_h < L.disk_full_h, what, 0);
        if (history_enabled()) {
            char m[200];

            snprintf(m, sizeof(m), "disk.%s.free_pct", fs->mount);
            history_metric(t / 10 * 10, m, free_pct);
        }
        for (int i = 0; i < fs->n_paths; i++)
            if (!strcmp(fs->paths[i], s->history_path)) {
                hist_critical = free_pct < L.disk_crit_pct;
                hist_ok = free_pct >= L.disk_warn_pct;
            }
    }
    /* the evidence folder within its cap; to half of it while its disk is nearly full */
    if (evidence_on(s) && t - s->ev_trim_last >= 60) {
        int64_t cap = (int64_t)s->evidence_max_mb * 1048576;

        s->ev_trim_last = t;
        for (int k = 0; k < s->n_disks; k++)
            for (int i = 0; i < s->disks[k].n_paths; i++)
                if (!strcmp(s->disks[k].paths[i], s->evidence_dir) && s->disks[k].total &&
                    100.0 * (double)s->disks[k].avail / (double)s->disks[k].total < L.disk_crit_pct)
                    cap /= 2;
        disk_trim(s->evidence_dir, cap);
    }
    /* the history's disk nearly full: it gives up its oldest records, a
     * quarter at a time, every ten minutes while it lasts */
    if (hist_critical && t - s->squeeze_last >= 600) {
        int64_t b = history_bytes();

        s->squeeze_last = t;
        s->squeezed = true;
        history_squeeze(b * 3 / 4);
        ev(NULL, "warn", "disk", "the history's disk is nearly full: the oldest records go (%.0f MB to %.0f MB)",
           (double)b / 1048576, (double)b * 3 / 4 / 1048576);
    } else if (s->squeezed && hist_ok) {
        s->squeezed = false;
        history_squeeze(0);
        ev(NULL, "info", "disk", "the history's disk has room again: its cap is as set");
    }
}

static void disks_json(const struct super *s, FILE *f)
{
    fputc('[', f);
    for (int k = 0; k < s->n_disks; k++) {
        const struct disk_fs *fs = &s->disks[k];
        const struct disk_track *tr = NULL;
        char q[300];
        double growth = 0;
        int n = 0;

        for (int i = 0; i < DISK_MAX && !tr; i++)
            if (!strcmp(s->dtr[i].mount, fs->mount))
                tr = &s->dtr[i];
        if (tr && tr->n > 1) {
            int last = (tr->head + tr->n - 1) % 61;
            int64_t span = tr->t[last] - tr->t[tr->head];

            if (span >= 600)
                growth = ((double)tr->used[last] - (double)tr->used[tr->head]) / ((double)span / 3600.0);
        }
        fprintf(f, "%s{\"mount\": %s", k ? ", " : "", json_quote(fs->mount, q, sizeof(q)));
        fprintf(f, ", \"device\": %s", json_quote(fs->dev, q, sizeof(q)));
        fprintf(f, ", \"type\": %s, \"total\": %llu, \"avail\": %llu, \"used_pct\": %.1f, \"growth_per_h\": %.0f",
                json_quote(fs->fstype, q, sizeof(q)), (unsigned long long)fs->total, (unsigned long long)fs->avail,
                fs->used_pct, growth);
        fprintf(f, ", \"ours\": [");
        for (int i = 0; i < fs->n_paths; i++)
            for (int j = 0; j < s->n_dpath; j++)
                if (!strcmp(s->dpath[j].path, fs->paths[i])) {
                    fprintf(f, "%s{\"path\": %s", n++ ? ", " : "", json_quote(s->dpath[j].path, q, sizeof(q)));
                    fprintf(f, ", \"what\": %s, \"bytes\": %lld}", json_quote(s->dpath[j].what, q, sizeof(q)),
                            (long long)s->dpath[j].bytes);
                    break;
                }
        fprintf(f, "]}");
    }
    fprintf(f, "], \"history_squeezed\": %s", s->squeezed ? "true" : "false");
}

/* ---- the leak watch (memwatch.h) ------------------------------------------- */

static void mem_one(struct memtrack **mp, pid_t pid, int64_t t, const char *subject, const char *chan,
                    const char *group, const char *who)
{
    struct wd_limits L;
    double rss, from = 0, to = 0, cf = 0, ct = 0;
    bool grow, cgrow;
    char what[300];

    if (!*mp && !(*mp = calloc(1, sizeof(**mp))))
        return;
    rss = pid > 0 ? mw_rss_mb(pid) : -1;
    if (rss < 0)
        return;
    mw_add(*mp, pid, t, rss, pid > 0 ? sysstat_card_mb(pid) : -1);
    wd_get_limits(&L);
    grow = mw_growing(*mp, t, L.mem_window_s, L.mem_growth_mb, L.mem_growth_pct, false, &from, &to);
    cgrow = mw_growing(*mp, t, L.mem_window_s, L.mem_growth_mb, L.mem_growth_pct, true, &cf, &ct);
    {
        char span[24];

        if (L.mem_window_s >= 7200)
            snprintf(span, sizeof(span), "%d h", L.mem_window_s / 3600);
        else if (L.mem_window_s >= 120)
            snprintf(span, sizeof(span), "%d min", L.mem_window_s / 60);
        else
            snprintf(span, sizeof(span), "%d s", L.mem_window_s);
        if (grow && cgrow)
            snprintf(what, sizeof(what), "%s: memory rose steadily over %s, from %.0f to %.0f MB (and on the card "
                     "from %.0f to %.0f MB): a leak in this transcoder", who, span, from, to, cf, ct);
        else if (cgrow)
            snprintf(what, sizeof(what), "%s: card memory rose steadily over %s, from %.0f to %.0f MB: a leak in "
                     "this transcoder", who, span, cf, ct);
        else
            snprintf(what, sizeof(what), "%s: memory rose steadily over %s, from %.0f to %.0f MB: a leak in this "
                     "transcoder", who, span, from, to);
    }
    wd_observe(t, subject, chan, group, WD_MEM_GROWING, grow || cgrow, what, 0);
}

/* A sample a minute of every process of ours: the channels, the card
 * processes, the supervisor. */
static void mem_tick(struct super *s)
{
    int64_t t = time(NULL);
    char subj[64], who[96];

    if (t - s->mem_last < (s->mem_test_s ? s->mem_test_s : 60))
        return;
    s->mem_last = t;
    for (int i = 0; i < s->n; i++) {
        struct chan *c = &s->ch[i];

        if (!c->cfg || c->state != CH_RUNNING || c->pid <= 0)
            continue;
        snprintf(who, sizeof(who), "channel %s", c->name);
        mem_one(&c->mem, c->pid, t, c->name, c->name, c->wd_group, who);
    }
    for (int k = 0; k < s->n_cards; k++) {
        if (s->cards[k].pid <= 0)
            continue;
        snprintf(subj, sizeof(subj), "server/card%d.process", s->cards[k].index);
        snprintf(who, sizeof(who), "the card process of card %d", s->cards[k].index);
        mem_one(&s->cards[k].mem, s->cards[k].pid, t, subj, NULL, "", who);
    }
    mem_one(&s->mem_self, getpid(), t, "server/supervisor", NULL, "", "the supervisor");
}

static void tick(struct super *s)
{
    int64_t now = clock_now_ns();

    for (int i = 0; i < s->n; i++)
        chan_share_prepare(s, &s->ch[i]);
    shares_update(s, now);
    cards_update(s, now, s->stopping_all);
    mem_tick(s);
    hist_tick(s);
    disk_tick(s);
    jobs_tick(s);
    lang_tick(s);
    {
        static int64_t wd_last;
        int64_t t = time(NULL);

        if (t - wd_last >= 2) {
            wd_last = t;
            for (int i = 0; i < s->n; i++)
                wd_channel_state(&s->ch[i], t);
            {
                struct wd_limits L;

                wd_get_limits(&L);
                ird_watch(t, L.ird_cn_drop_db);
            }
            wd_tick(t);
        }
    }

    for (int i = 0; i < s->n; i++) {
        struct chan *c = &s->ch[i];
        int status;

        chan_read_log(c);
        if (c->pid > 0 && waitpid(c->pid, &status, WNOHANG) == c->pid)
            chan_reap(s, c, status);
        chan_sample(s, c, now);
        preview_reap(s, c);
        if (c->pv_pid <= 0 && c->pv_restart) {
            char e[200];

            c->pv_restart = false;
            if (c->state == CH_RUNNING && !preview_start(s, c, c->pv_next, e, sizeof(e)))
                snprintf(c->pv_why, sizeof(c->pv_why), "%.119s", e);
        }
        if (c->pv_pid > 0 && (c->state != CH_RUNNING ||
                              now - c->pv_last_hit_ns > (int64_t)PREVIEW_IDLE_S * 1000000000LL))
            preview_stop(c);
        if (c->pv_log_fd >= 0 && c->pv_pid > 0) {
            char buf[4096];                 /* the preview's log is not kept, only drained */

            while (read(c->pv_log_fd, buf, sizeof(buf)) > 0)
                ;
        }
        if (c->state == CH_STOPPING && now - c->stop_asked_ns > STOP_GRACE_MS * 1000000LL) {
            log_warn("channel %s: did not stop in %d s, killing it", c->name, STOP_GRACE_MS / 1000);
            ev(c->name, "warn", "stop", "did not stop in %d s, killed", STOP_GRACE_MS / 1000);
            kill(c->pid, SIGKILL);
            c->stop_asked_ns = now;
        }
        if (c->want && (c->state == CH_STOPPED || (c->state == CH_FAILED && now >= c->next_start_ns)))
            chan_start(s, c);
    }
}

/* ---- the channels directory ----------------------------------------------------- */

static void scan_channels(struct super *s)
{
    DIR *d = opendir(s->channels_dir);
    struct dirent *e;
    bool seen[CHANNELS_MAX] = { false };

    if (!d) {
        log_warn("channels: %s: %s", s->channels_dir, strerror(errno));
        return;
    }
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        char name[64], path[600];
        char *text;
        struct chan *c;
        static struct json_doc doc;
        char err[200];
        int root;

        if (n < 6 || strcmp(e->d_name + n - 5, ".json") || strstr(e->d_name, ".outputs.json"))
            continue;
        if (n - 5 >= sizeof(name))
            continue;
        memcpy(name, e->d_name, n - 5);
        name[n - 5] = 0;
        if (!name_ok(name))
            continue;
        snprintf(path, sizeof(path), "%.400s/%.100s", s->channels_dir, e->d_name);
        text = read_file(path, NULL);
        if (!text)
            continue;
        root = json_parse(&doc, text, err, sizeof(err));
        if (root < 0 || doc.node[root].type != JSON_OBJECT) {
            log_err("channel %s: %s: %s", name, path, root < 0 ? err : "not an object");
            free(text);
            continue;
        }
        c = find_chan(s, name);
        if (!c) {
            for (int i = 0; i < s->n && !c; i++)    /* a slot whose channel was deleted */
                if (!s->ch[i].cfg && s->ch[i].pid == 0)
                    c = &s->ch[i];
            if (!c && s->n == CHANNELS_MAX) {
                log_err("channel %s: no room (%d channels at most)", name, CHANNELS_MAX);
                free(text);
                continue;
            }
            if (!c)
                c = &s->ch[s->n++];
            free(c->wd_stats);
            memset(c, 0, sizeof(*c));
            snprintf(c->name, sizeof(c->name), "%s", name);
            c->log_fd = -1;
            c->pv_log_fd = -1;
            c->state = CH_STOPPED;
            ev(name, "info", "create", "channel added");
        }
        if (!c->cfg || strcmp(c->cfg, text)) {
            bool changed = c->cfg != NULL;

            free(c->cfg);
            c->cfg = text;
            c->enabled = json_get_bool(&doc, root, "enabled", true);
            c->want = c->enabled;
            if (changed && c->state == CH_RUNNING) {
                log_info("channel %s: config changed, restarting", c->name);
                ev(c->name, "info", "config", "settings changed, restarting");
                chan_stop(s, c);
            }
        } else {
            free(text);
        }
        seen[c - s->ch] = true;
    }
    closedir(d);
    /* files that went away: stop and forget */
    for (int i = 0; i < s->n; i++)
        if (!seen[i] && s->ch[i].cfg) {
            struct chan *c = &s->ch[i];

            log_info("channel %s: its file is gone, stopping", c->name);
            ev(c->name, "info", "delete", "channel removed");
            c->want = false;
            chan_stop(s, c);
            free(c->cfg);
            c->cfg = NULL;
        }
}

/* ---- the API -------------------------------------------------------------------- */

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static bool authorized(struct super *s, const struct http_req *req)
{
    char want[160], got[200];
    const char *b;
    int o = 0, bits = 0;
    unsigned acc = 0;

    if (!s->password[0])
        return true;
    if (strncasecmp(req->auth, "Basic ", 6))
        return false;
    for (b = req->auth + 6; *b && *b != '=' && o < (int)sizeof(got) - 1; b++) {
        int v = b64val(*b);

        if (v < 0)
            continue;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            got[o++] = (char)((acc >> bits) & 0xFF);
        }
    }
    got[o] = 0;
    snprintf(want, sizeof(want), "admin:%s", s->password);
    return strlen(got) == strlen(want) && memcmp(got, want, strlen(want)) == 0;
}

static const char *state_name(enum chan_state st)
{
    switch (st) {
    case CH_STOPPED:  return "stopped";
    case CH_STARTING: return "starting";
    case CH_RUNNING:  return "running";
    case CH_STOPPING: return "stopping";
    case CH_FAILED:   return "failed";
    }
    return "?";
}

/* Asks the channel's control socket; the reply (one JSON object) or NULL. */
static char *chan_ctl(struct super *s, struct chan *c, const char *request)
{
    char path[600], *out = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&out, &len);
    int rc;

    if (!f)
        return NULL;
    snprintf(path, sizeof(path), "%.400s/%s.sock", s->run_dir, c->name);
    rc = control_client(path, request, f);
    fclose(f);
    if (rc == 2) {
        free(out);
        return NULL;
    }
    return out;
}

/* The channel's statistics file, or NULL. */
static char *chan_stats(struct super *s, struct chan *c)
{
    char path[600];

    snprintf(path, sizeof(path), "%.400s/%s.json", s->run_dir, c->name);
    return read_file(path, NULL);
}

/* What the API says about a channel, taken under the lock so the I/O
 * (control socket, files) can happen without it. */
struct chan_view {
    char name[64];
    enum chan_state state;
    bool enabled;
    pid_t pid;
    double uptime;
    int restarts;
    double rss_mb, card_mb;                 /* -1: not known */
    char why[160];
    char *cfg;
    char *log;                              /* JSON array text, or NULL */
    float hist_in[HIST_N], hist_out[HIST_N];
    int hist_n;
    bool pv_running;
    char pv_drop[128];
    char pv_why[120];
    double pv_uptime;
    int64_t lang_t;
    char lang_note[160], lang_heard[8];
    bool lang_wrong, lang_busy, lang_can;
};

static void chan_view_take(struct chan *c, struct chan_view *v, bool with_log)
{
    int64_t now = clock_now_ns();

    memset(v, 0, sizeof(*v));
    snprintf(v->name, sizeof(v->name), "%s", c->name);
    v->state = c->state;
    v->enabled = c->enabled;
    v->pid = c->pid;
    v->uptime = c->state == CH_RUNNING || c->state == CH_STOPPING ? (double)(now - c->started_ns) / 1e9 : 0.0;
    v->restarts = c->restarts;
    v->rss_mb = v->card_mb = -1;
    if (c->mem && c->state == CH_RUNNING)
        mw_last(c->mem, &v->rss_mb, &v->card_mb);
    snprintf(v->why, sizeof(v->why), "%s", c->why);
    v->cfg = c->cfg ? strdup(c->cfg) : NULL;
    for (int i = 0; i < c->hist_n; i++) {
        v->hist_in[i] = c->hist_in[(c->hist_head + i) % HIST_N];
        v->hist_out[i] = c->hist_out[(c->hist_head + i) % HIST_N];
    }
    v->hist_n = c->hist_n;
    v->pv_running = c->pv_pid > 0;
    v->pv_uptime = c->pv_pid > 0 ? (double)(now - c->pv_started_ns) / 1e9 : 0;
    snprintf(v->pv_drop, sizeof(v->pv_drop), "%s", c->pv_drop);
    snprintf(v->pv_why, sizeof(v->pv_why), "%s", c->pv_why);
    v->lang_t = c->lang_t;
    snprintf(v->lang_note, sizeof(v->lang_note), "%s", c->lang_note);
    snprintf(v->lang_heard, sizeof(v->lang_heard), "%s", c->lang_heard);
    v->lang_wrong = c->lang_wrong;
    v->lang_busy = c->lang_busy;
    v->lang_can = c->lang_url[0] != 0;
    if (with_log) {
        char *out = NULL;
        size_t len = 0;
        FILE *f = open_memstream(&out, &len);
        char q[LOG_LINE_MAX * 2 + 4];

        if (f) {
            fprintf(f, "[");
            for (int i = 0; i < c->log_n; i++)
                fprintf(f, "%s%s", i ? ", " : "", json_quote(c->log[(c->log_head + i) % LOG_LINES], q, sizeof(q)));
            fprintf(f, "]");
            fclose(f);
            v->log = out;
        }
    }
}

static void chan_view_drop(struct chan_view *v)
{
    free(v->cfg);
    free(v->log);
}

/* JSON node `i` of `d` written out again (for the brief channel list). */
static void jnode_write(FILE *f, const struct json_doc *d, int i)
{
    char q[1100];
    const struct json_node *n = &d->node[i];

    switch (n->type) {
    case JSON_NULL: fputs("null", f); break;
    case JSON_BOOL: fputs(n->boolean ? "true" : "false", f); break;
    case JSON_NUMBER:
        if (n->num == (double)(long long)n->num && n->num < 9e15 && n->num > -9e15)
            fprintf(f, "%lld", (long long)n->num);
        else
            fprintf(f, "%.6g", n->num);
        break;
    case JSON_STRING: fputs(json_quote(n->str ? n->str : "", q, sizeof(q)), f); break;
    case JSON_ARRAY: case JSON_OBJECT: {
        int k = 0;

        fputc(n->type == JSON_ARRAY ? '[' : '{', f);
        for (int c = n->first_child; c >= 0; c = d->node[c].next, k++) {
            if (k)
                fputs(", ", f);
            if (n->type == JSON_OBJECT)
                fprintf(f, "%s: ", json_quote(d->node[c].key ? d->node[c].key : "", q, sizeof(q)));
            jnode_write(f, d, c);
        }
        fputc(n->type == JSON_ARRAY ? ']' : '}', f);
        break;
    }
    }
}

/* Only these members of object `obj` (those present), the braces left
 * open (jpick_open) or closed (jpick). Returns how many were written. */
static int jpick_open(FILE *f, const struct json_doc *d, int obj, const char *const *keys)
{
    char q[200];
    int k = 0;

    fputc('{', f);
    for (int i = 0; obj >= 0 && keys[i]; i++) {
        int c = json_get(d, obj, keys[i]);

        if (c < 0)
            continue;
        fprintf(f, "%s%s: ", k++ ? ", " : "", json_quote(keys[i], q, sizeof(q)));
        jnode_write(f, d, c);
    }
    return k;
}

static void jpick(FILE *f, const struct json_doc *d, int obj, const char *const *keys)
{
    jpick_open(f, d, obj, keys);
    fputc('}', f);
}

/* A running channel's statistics, cut to what a list of channels needs
 * (its health, rates, what it does): a tenth of the size, and no question
 * to the channel itself. */
static void stats_brief(FILE *f, struct json_doc *d, const char *st)
{
    static const char *const in_k[] = { "url", "shared", "bitrate", "last_packet_age_ms", "cc_errors", "socket_overflow",
                                        "rejoins", "joined_on", "join_note",
                                        "active", "switches", NULL };
    static const char *const link_k[] = { "transport", "state", "connected", "lost", NULL };
    static const char *const out_k[] = { "bitrate", "mode", "muxrate", "dropped_units", "late_units", NULL };
    static const char *const clk_k[] = { "locked", "drift_ppm", NULL };
    static const char *const s_k[] = { "kind", "language", "in_pid", "pid", "from_pid", "src_pid", "idle_ms", "failed",
                                       "input_dropped", "codec", "from", "size", "mode", "encoder", "decoder", NULL };
    char err[100];
    int root = st && strlen(st) < JSON_TEXT_MAX ? json_parse(d, st, err, sizeof(err)) : -1;
    int in, svc, ss, ct;

    if (root < 0) {
        fputs("null", f);
        return;
    }
    in = json_get(d, root, "input");
    fputs("{\"input\": ", f);
    {
        int k = jpick_open(f, d, in, in_k);

        if (in >= 0 && json_get(d, in, "link") >= 0) {
            fprintf(f, "%s\"link\": ", k ? ", " : "");
            jpick(f, d, json_get(d, in, "link"), link_k);
        }
        fputc('}', f);
    }
    fputs(", \"output\": ", f);
    jpick(f, d, json_get(d, root, "output"), out_k);
    fputs(", \"clock\": ", f);
    jpick(f, d, json_get(d, root, "clock"), clk_k);
    fprintf(f, ", \"uptime_s\": %.0f", json_get_num(d, root, "uptime_s", 0));
    if ((ct = json_get(d, root, "content")) >= 0) {
        fputs(", \"content\": ", f);
        jnode_write(f, d, ct);
    }
    svc = json_get(d, root, "service");
    ss = svc >= 0 ? json_get(d, svc, "streams") : -1;
    fprintf(f, ", \"service\": {\"number\": %.0f, \"streams\": [", svc >= 0 ? json_get_num(d, svc, "number", 0) : 0);
    for (int j = ss >= 0 ? d->node[ss].first_child : -1, k = 0; j >= 0; j = d->node[j].next, k++) {
        if (k)
            fputs(", ", f);
        jpick(f, d, j, s_k);
    }
    fputs("]}}", f);
}

static void json_chan_view(struct super *s, const struct chan_view *v, FILE *f, bool full);
static void json_chan_view(struct super *s, const struct chan_view *v, FILE *f, bool full)
{
    char q[200];
    struct chan tmp;

    memset(&tmp, 0, sizeof(tmp));
    snprintf(tmp.name, sizeof(tmp.name), "%s", v->name);
    fprintf(f, "{\"name\": %s, \"state\": \"%s\", \"enabled\": %s, \"pid\": %d, \"uptime\": %.0f, "
               "\"restarts\": %d, \"why\": ",
            json_quote(v->name, q, sizeof(q)), state_name(v->state), v->enabled ? "true" : "false",
            (int)v->pid, v->uptime, v->restarts);
    fprintf(f, "%s", json_quote(v->why, q, sizeof(q)));
    if (v->rss_mb >= 0)
        fprintf(f, ", \"memory_mb\": %.0f", v->rss_mb);
    if (v->card_mb >= 0)
        fprintf(f, ", \"card_memory_mb\": %.0f", v->card_mb);
    fprintf(f, ", \"language\": {\"checked\": %lld, \"heard\": %s, ", (long long)v->lang_t,
            json_quote(v->lang_heard, q, sizeof(q)));
    fprintf(f, "\"note\": %s, \"wrong\": %s, \"listening\": %s, \"can\": %s}",
            json_quote(v->lang_note, q, sizeof(q)), v->lang_wrong ? "true" : "false", v->lang_busy ? "true" : "false",
            v->lang_can ? "true" : "false");
    fprintf(f, ", \"config\": %s", v->cfg ? v->cfg : "null");
    if (v->state == CH_RUNNING || full) {
        char *st = chan_stats(s, &tmp);
        char *outs = v->state == CH_RUNNING ? chan_ctl(s, &tmp, "{\"cmd\": \"list-outputs\"}") : NULL;

        fprintf(f, ", \"stats\": %s", st && v->state == CH_RUNNING ? st : "null");
        fprintf(f, ", \"outputs\": %s", outs ? outs : "null");
        free(st);
        free(outs);
        if (!outs) {
            /* not running: what the outputs file says */
            char path[600], *text;

            snprintf(path, sizeof(path), "%.400s/%s.outputs.json", s->channels_dir, v->name);
            text = read_file(path, NULL);
            fprintf(f, ", \"saved_outputs\": %s", text ? text : "null");
            free(text);
        }
    }
    if (full) {
        fprintf(f, ", \"history\": {\"step_s\": %d, \"in\": [", HIST_STEP_S);
        for (int i = 0; i < v->hist_n; i++)
            fprintf(f, "%s%.0f", i ? "," : "", (double)v->hist_in[i]);
        fprintf(f, "], \"out\": [");
        for (int i = 0; i < v->hist_n; i++)
            fprintf(f, "%s%.0f", i ? "," : "", (double)v->hist_out[i]);
        fprintf(f, "]}, \"preview\": {\"running\": %s, \"uptime\": %.0f, \"drop\": ",
                v->pv_running ? "true" : "false", v->pv_uptime);
        fprintf(f, "%s, \"why\": ", json_quote(v->pv_drop, q, sizeof(q)));
        fprintf(f, "%s}", json_quote(v->pv_why, q, sizeof(q)));
        fprintf(f, ", \"log\": %s", v->log ? v->log : "[]");
    }
    fprintf(f, "}");
}

static void api_error(struct http_resp *resp, int status, const char *msg)
{
    char q[400];

    http_resp_printf(resp, status, "application/json", "{\"ok\": false, \"error\": %s}\n",
                     json_quote(msg, q, sizeof(q)));
}

/* /hls/NAME/... straight through to the channel's HTTP socket. */
static void proxy_hls(struct super *s, const struct http_req *req, struct http_resp *resp)
{
    char name[64], path[600], line[2048];
    const char *p = req->path + 5, *slash = strchr(p, '/');
    struct sockaddr_un sa;
    int fd;
    size_t nl = slash ? (size_t)(slash - p) : strlen(p);

    if (nl == 0 || nl >= sizeof(name)) {
        resp->status = 404;
        return;
    }
    memcpy(name, p, nl);
    name[nl] = 0;
    {
        /* NAME.preview: the preview process's own HLS; the request keeps it alive */
        size_t l = strlen(name);
        bool preview = l > 8 && !strcmp(name + l - 8, ".preview");

        if (preview)
            name[l - 8] = 0;
        if (!name_ok(name)) {
            resp->status = 404;
            return;
        }
        if (preview) {
            struct chan *c;

            pthread_mutex_lock(&s->lock);
            c = find_chan(s, name);
            if (c)
                c->pv_last_hit_ns = clock_now_ns();
            pthread_mutex_unlock(&s->lock);
            snprintf(path, sizeof(path), "%.400s/%s.preview.http", s->run_dir, name);
        } else {
            snprintf(path, sizeof(path), "%.400s/%s.http", s->run_dir, name);
        }
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%.107s", path);
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        if (fd >= 0)
            close(fd);
        api_error(resp, 503, "that channel has no HLS output running");
        return;
    }
    /* one request per connection: without "close" the channel keeps the
     * connection for the next request, and this copy loop would wait for
     * its idle timeout before the player got the end of the response */
    snprintf(line, sizeof(line), "GET %.900s%s%.900s HTTP/1.1\r\nHost: caricoder3\r\nConnection: close\r\n\r\n",
             req->path, req->query[0] ? "?" : "", req->query);
    if (http_send(fd, line, strlen(line))) {
        char buf[65536];
        ssize_t r;

        resp->handled = true;
        for (;;) {
            struct pollfd pf = { fd, POLLIN, 0 };

            if (poll(&pf, 1, 15000) <= 0)
                break;
            r = recv(fd, buf, sizeof(buf), 0);
            if (r <= 0)
                break;
            if (!http_send(req->fd, buf, (size_t)r))
                break;
        }
    } else {
        api_error(resp, 502, "the channel did not answer");
    }
    close(fd);
}

/* name=value from a query string, %-decoded; false when absent. */
static bool query_param(const char *query, const char *name, char *out, size_t len)
{
    size_t nl = strlen(name);

    for (const char *p = query; p && *p; ) {
        const char *amp = strchr(p, '&'), *end = amp ? amp : p + strlen(p);

        if ((size_t)(end - p) > nl && !strncmp(p, name, nl) && p[nl] == '=') {
            size_t o = 0;

            for (const char *q = p + nl + 1; q < end && o + 1 < len; q++) {
                if (*q == '%' && q + 2 < end + 1 && isxdigit((unsigned char)q[1]) && isxdigit((unsigned char)q[2])) {
                    char hex[3] = { q[1], q[2], 0 };

                    out[o++] = (char)strtol(hex, NULL, 16);
                    q += 2;
                } else {
                    out[o++] = *q == '+' ? ' ' : *q;
                }
            }
            out[o] = 0;
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    return false;
}

static void handle(void *opaque, const struct http_req *req, struct http_resp *resp)
{
    struct super *s = opaque;
    const char *p = req->path;
    char q[600];

    if (!strncmp(p, "/hls/", 5)) {
        proxy_hls(s, req, resp);
        return;
    }
    if (!strncmp(p, "/api/peer/", 10)) {
        /* another transcoder, with the shared key (or a person signed in) */
        if (!peer_authorized(s, req) && !(s->password[0] && authorized(s, req))) {
            http_resp_text(resp, 401, "application/json", "{\"ok\": false, \"error\": \"not our key\"}\n");
            return;
        }
        handle_peer(s, req, resp, p);
        return;
    }
    if (!authorized(s, req)) {
        snprintf(resp->extra, sizeof(resp->extra), "WWW-Authenticate: Basic realm=\"caricoder3\"\r\n");
        http_resp_text(resp, 401, "text/plain", "sign in as admin with the password from the config\n");
        return;
    }
    if (!strcmp(p, "/") || !strcmp(p, "/index.html")) {
        resp->status = 200;
        resp->type = "text/html; charset=utf-8";
        resp->body = malloc(gui_html_len);
        if (resp->body) {
            memcpy(resp->body, gui_html, gui_html_len);
            resp->body_len = gui_html_len;
        }
        return;
    }
    if (!strcmp(p, "/hls.min.js")) {
        resp->status = 200;
        resp->type = "application/javascript";
        resp->body = malloc(gui_hls_js_len);
        if (resp->body) {
            memcpy(resp->body, gui_hls_js, gui_hls_js_len);
            resp->body_len = gui_hls_js_len;
        }
        snprintf(resp->extra, sizeof(resp->extra), "Cache-Control: max-age=86400\r\n");
        return;
    }
    if (!strcmp(p, "/api/events") || !strcmp(p, "/api/history/server") || !strcmp(p, "/api/records") ||
        !strcmp(p, "/api/summary") ||
        (!strncmp(p, "/api/channels/", 14) && strlen(p) > 22 && !strcmp(p + strlen(p) - 8, "/history"))) {
        /* ?from=&to= (unix seconds; default the last 24 h), &channel= / &metric= / &limit= */
        char v[128], chan[64] = "";
        int64_t now = time(NULL), from = now - 86400, to = now + 1;
        int limit = 500;
        char *out = NULL;
        size_t len = 0;
        FILE *f;
        bool ok;

        if (query_param(req->query, "from", v, sizeof(v)))
            from = strtoll(v, NULL, 10);
        if (query_param(req->query, "to", v, sizeof(v)))
            to = strtoll(v, NULL, 10);
        if (query_param(req->query, "limit", v, sizeof(v)))
            limit = atoi(v) > 0 && atoi(v) <= 5000 ? atoi(v) : 500;
        if (to <= from)
            to = from + 1;
        if (!history_enabled()) {
            api_error(resp, 503, "no history is kept (see the Server page)");
            return;
        }
        f = open_memstream(&out, &len);
        if (!f)
            return;
        if (!strcmp(p, "/api/events")) {
            bool has = query_param(req->query, "channel", chan, sizeof(chan));

            ok = history_query_events(f, has ? chan : NULL, from, to, limit);
        } else if (!strcmp(p, "/api/records")) {
            /* what came and went (CC errors, short drops the source caused): ?channel= */
            bool has = query_param(req->query, "channel", chan, sizeof(chan));

            ok = history_query_records(f, has ? chan : NULL, from, to, limit);
        } else if (!strcmp(p, "/api/summary")) {
            /* by source: ?days=N (default 7) up to now, or ?from=&to= */
            if (query_param(req->query, "days", v, sizeof(v)) && atoi(v) > 0 && atoi(v) <= 366)
                from = now - (int64_t)atoi(v) * 86400;
            else if (!query_param(req->query, "from", v, sizeof(v)))
                from = now - 7 * 86400;
            ok = history_summary(f, from, to);
        } else if (!strcmp(p, "/api/history/server")) {
            ok = history_query_metrics(f, query_param(req->query, "metric", v, sizeof(v)) ? v : "*", from, to);
        } else {
            snprintf(chan, sizeof(chan), "%.*s", (int)(strlen(p) - 22), p + 14);
            ok = history_query_chan(f, chan, from, to);
        }
        fclose(f);
        if (!ok) {
            free(out);
            api_error(resp, 500, "the history could not be read");
            return;
        }
        resp->status = 200;
        resp->type = "application/json";
        resp->body = out;
        resp->body_len = len;
        return;
    }
    if (!strncmp(p, "/api/incidents/", 15) && strstr(p + 15, "/verdict")) {
        /* POST {"verdict": "right"|"partly"|"wrong", "category", "cause", "note"}: a person's word on the technician's */
        struct json_doc *doc = malloc(sizeof(*doc));
        char err[200];
        int root = doc ? json_parse(doc, req->body ? req->body : "{}", err, sizeof(err)) : -1;
        int64_t id = strtoll(p + 15, NULL, 10);

        if (root < 0 || strcmp(req->method, "POST") || id <= 0) {
            free(doc);
            api_error(resp, 400, "POST {\"verdict\": \"right\", \"partly\" or \"wrong\", ...}");
            return;
        }
        if (!apply_verdict(doc, root, id, err, sizeof(err))) {
            free(doc);
            api_error(resp, 400, err);
            return;
        }
        free(doc);
        http_resp_text(resp, 200, "application/json", "{\"ok\": true}\n");
        return;
    }
    if (!strcmp(p, "/api/verdicts/import")) {
        /* POST JSON lines, one verdict to a line: {"id", "verdict", "category", "cause", "note"} */
        struct json_doc *doc = malloc(sizeof(*doc));
        char *body = req->body ? strdup(req->body) : NULL, *save = NULL, out[1500], err[200];
        int done = 0, bad = 0, lineno = 0;
        size_t n = 0;

        if (strcmp(req->method, "POST") || !doc || !body) {
            free(doc);
            free(body);
            api_error(resp, 400, "POST the verdicts, one JSON object to a line");
            return;
        }
        out[0] = 0;
        for (char *line = strtok_r(body, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            int root;
            int64_t id;

            lineno++;
            while (*line == ' ' || *line == '\t' || *line == '\r')
                line++;
            if (!*line)
                continue;
            root = json_parse(doc, line, err, sizeof(err));
            id = root >= 0 ? (int64_t)json_get_num(doc, root, "id", 0) : 0;
            if (root < 0 || id <= 0 || !history_incident_exists(id) || !apply_verdict(doc, root, id, err, sizeof(err))) {
                bad++;
                if (n + 200 < sizeof(out))
                    n += (size_t)snprintf(out + n, sizeof(out) - n, "%sline %d: %.120s", n ? "; " : "", lineno,
                                          root < 0 ? err : id <= 0 ? "no id" : !history_incident_exists(id) ? "no such incident" : err);
                continue;
            }
            done++;
        }
        free(body);
        free(doc);
        {
            char qe[1600], text[1800];

            snprintf(text, sizeof(text), "{\"ok\": true, \"applied\": %d, \"failed\": %d, \"errors\": %s}\n", done, bad,
                     json_quote(out, qe, sizeof(qe)));
            http_resp_text(resp, 200, "application/json", text);
        }
        if (done)
            ev(NULL, "info", "verdicts", "%d verdicts imported%s", done, bad ? " (some lines could not be)" : "");
        return;
    }
    if (!strcmp(p, "/api/training/export")) {
        /* the verdicts so far as training data: JSON lines */
        char *out = NULL;
        size_t len = 0;
        FILE *f = open_memstream(&out, &len);
        int n;

        if (!f)
            return;
        n = tech_export(f);
        fclose(f);
        resp->status = 200;
        resp->type = "application/x-ndjson";
        resp->body = out;
        resp->body_len = len;
        snprintf(resp->extra, sizeof(resp->extra),
                 "Content-Disposition: attachment; filename=\"caricoder3-%s-training-%d.jsonl\"\r\n", s->name, n);
        return;
    }
    if (!strcmp(p, "/api/incidents") || !strncmp(p, "/api/incidents/", 15)) {
        /* ?channel=&open=1&from=&to=&limit= ; /api/incidents/ID: one, with its events */
        char v[128], chan[64] = "";
        int64_t now = time(NULL), from = now - 7 * 86400, to = now + 1;
        int limit = 200;
        char *out = NULL;
        size_t len = 0;
        FILE *f;
        bool ok;

        if (strcmp(req->method, "GET")) {
            api_error(resp, 405, "GET only");
            return;
        }
        if (!history_enabled()) {
            api_error(resp, 503, "no history is kept (see the Server page)");
            return;
        }
        if (query_param(req->query, "from", v, sizeof(v)))
            from = strtoll(v, NULL, 10);
        if (query_param(req->query, "to", v, sizeof(v)))
            to = strtoll(v, NULL, 10);
        if (query_param(req->query, "limit", v, sizeof(v)))
            limit = atoi(v) > 0 && atoi(v) <= 2000 ? atoi(v) : 200;
        f = open_memstream(&out, &len);
        if (!f)
            return;
        if (p[14] == '/') {
            ok = history_query_incident(f, strtoll(p + 15, NULL, 10), now);
        } else {
            bool has = query_param(req->query, "channel", chan, sizeof(chan));
            bool open_only = query_param(req->query, "open", v, sizeof(v)) && !strcmp(v, "1");

            /* ?brief=1: without each one's evidence snapshot (the list needs none; it is
             * most of the size: 690 kB a day on transcoder2, fetched every few seconds) */
            if (query_param(req->query, "brief", v, sizeof(v)) && !strcmp(v, "1") && !has)
                ok = history_query_incidents_brief(f, from, to, open_only, limit);
            else
                ok = history_query_incidents(f, has ? chan : NULL, from, to, open_only, limit);
        }
        fclose(f);
        if (!ok) {
            free(out);
            api_error(resp, p[14] == '/' ? 404 : 500, p[14] == '/' ? "no such incident" : "the history could not be read");
            return;
        }
        resp->status = 200;
        resp->type = "application/json";
        resp->body = out;
        resp->body_len = len;
        if (query_param(req->query, "download", v, sizeof(v)) && !strcmp(v, "1"))
            snprintf(resp->extra, sizeof(resp->extra),
                     "Content-Disposition: attachment; filename=\"caricoder3-%s-incidents.json\"\r\n", s->name);
        return;
    }
    if (!strncmp(p, "/api/tools", 10) && (p[10] == 0 || p[10] == '/')) {
        /* POST /api/tools/record|probe|ping|snapshot {"channel", "url", "host", "seconds", "incident"};
         * GET /api/tools: what is running */
        const char *tool = p[10] == '/' ? p + 11 : "";
        char err[400] = "", url[512] = "", tq[64];
        bool ok = false;

        if (!strcmp(req->method, "GET") && !tool[0]) {
            char *out = NULL;
            size_t len = 0;
            FILE *f = open_memstream(&out, &len);

            if (!f)
                return;
            pthread_mutex_lock(&s->lock);
            jobs_json(s, f);
            pthread_mutex_unlock(&s->lock);
            fclose(f);
            resp->status = 200;
            resp->type = "application/json";
            resp->body = out;
            resp->body_len = len;
            return;
        }
        if (strcmp(req->method, "POST")) {
            api_error(resp, 405, "POST /api/tools/record, probe, ping or snapshot");
            return;
        }
        {
            struct json_doc *doc = malloc(sizeof(*doc));
            int root = doc ? json_parse(doc, req->body && req->body_len ? req->body : "{}", err, sizeof(err)) : -1;
            const char *chan, *host;
            int seconds, program = 0;
            int64_t incident;
            struct chan *c = NULL;

            if (root < 0) {
                free(doc);
                api_error(resp, 400, doc ? err : "out of memory");
                return;
            }
            chan = json_get_str(doc, root, "channel", NULL);
            host = json_get_str(doc, root, "host", "");
            seconds = (int)json_get_num(doc, root, "seconds", !strcmp(tool, "record") ? 20 : 3);
            incident = (int64_t)json_get_num(doc, root, "incident", 0);
            snprintf(url, sizeof(url), "%s", json_get_str(doc, root, "url", ""));
            if (seconds < 1)
                seconds = 1;
            if (seconds > 60)
                seconds = 60;
            pthread_mutex_lock(&s->lock);
            if (chan && !(c = find_chan(s, chan)))
                snprintf(err, sizeof(err), "no channel %.60s", chan);
            else if (c && !url[0])
                chan_source(c, url, sizeof(url), &program);
            if (err[0]) {
                /* said above */
            } else if (!strcmp(tool, "record")) {
                const char *from = json_get_str(doc, root, "from", "");

                if (!c)
                    snprintf(err, sizeof(err), "record needs \"channel\"");
                else if (strcmp(from, "output") &&
                         tool_record_input(s, c, 60, seconds > 30 ? 30 : seconds, incident, err, sizeof(err)))
                    ok = true;              /* with the minute before the button was pressed */
                else if (strcmp(from, "output") && source_readable(url))
                    ok = tool_probe(s, c->name, url, program, seconds, true, incident, err, sizeof(err));
                else
                    ok = tool_record_output(s, c, seconds, incident, err, sizeof(err));
            } else if (!strcmp(tool, "probe")) {
                if (!url[0])
                    snprintf(err, sizeof(err), "probe needs \"channel\" or \"url\"");
                else
                    ok = tool_probe(s, c ? c->name : NULL, url, program, seconds, false, incident, err, sizeof(err));
            } else if (!strcmp(tool, "ask_peers")) {
                char why[200];

                snprintf(why, sizeof(why), "%s%.60s", c ? "Checking the source of my channel " : "Checking a source",
                         c ? c->name : "");
                if (!url[0])
                    snprintf(err, sizeof(err), "ask_peers needs \"channel\" or \"url\"");
                else if (!source_readable(url))
                    snprintf(err, sizeof(err), "%.300s cannot be read twice (only multicast and HTTP can), so the peers cannot look at it", url);
                else if (!(ok = peer_ask_check(url, program, seconds < 10 ? seconds : 10, incident, why) > 0))
                    snprintf(err, sizeof(err), "no peer answers just now (see Peers on the Server page)");
            } else if (!strcmp(tool, "ping")) {
                ok = tool_ping(s, host, seconds < 2 ? 3 : seconds > 10 ? 10 : seconds, incident, err, sizeof(err));
            } else if (!strcmp(tool, "snapshot")) {
                if (!c)
                    snprintf(err, sizeof(err), "snapshot needs \"channel\"");
                else
                    ok = tool_snapshot(s, c, incident, err, sizeof(err));
            } else {
                snprintf(err, sizeof(err), "no tool %.40s (record, probe, ping, snapshot)", tool);
            }
            pthread_mutex_unlock(&s->lock);
            free(doc);
            if (!ok) {
                api_error(resp, 400, err);
                return;
            }
            ev(c ? c->name : NULL, "info", "tool", "%s started by hand%s%s", tool, url[0] && strcmp(tool, "ping") ? " on " : "",
               strcmp(tool, "ping") ? url : "");
            http_resp_printf(resp, 200, "application/json", "{\"ok\": true, \"tool\": %s}\n", json_quote(tool, tq, sizeof(tq)));
            return;
        }
    }
    if (!strncmp(p, "/api/evidence", 13) && (p[13] == 0 || p[13] == '/')) {
        /* GET /api/evidence?channel=&incident=&limit= ; /api/evidence/ID/file: the file kept */
        char v[64], chan[64];

        if (!history_enabled()) {
            api_error(resp, 503, "no history is kept");
            return;
        }
        if (p[13] == '/') {
            char path[600];
            int64_t id = strtoll(p + 14, NULL, 10);
            size_t len = 0;
            char *data;
            const char *ext;

            if (!strstr(p + 14, "/file") || !history_evidence_file(id, path, sizeof(path))) {
                api_error(resp, 404, "no such evidence file");
                return;
            }
            /* only from the evidence folder */
            if (strncmp(path, s->evidence_dir, strlen(s->evidence_dir)) || strstr(path, "..") ||
                !(data = read_whole(path, &len, 512u << 20))) {
                api_error(resp, 404, "the file is gone (the evidence folder is kept to its size)");
                return;
            }
            ext = strrchr(path, '.');
            resp->status = 200;
            resp->type = ext && !strcmp(ext, ".jpg") ? "image/jpeg" : "video/mp2t";
            resp->body = data;
            resp->body_len = len;
            if (!(ext && !strcmp(ext, ".jpg")))
                snprintf(resp->extra, sizeof(resp->extra), "Content-Disposition: attachment; filename=\"%.200s\"\r\n",
                         strrchr(path, '/') + 1);
            return;
        }
        {
            char *out = NULL;
            size_t len = 0;
            FILE *f = open_memstream(&out, &len);
            bool has = query_param(req->query, "channel", chan, sizeof(chan));
            int64_t inc = query_param(req->query, "incident", v, sizeof(v)) ? strtoll(v, NULL, 10) : 0;
            int limit = query_param(req->query, "limit", v, sizeof(v)) ? atoi(v) : 100;

            if (!f)
                return;
            if (!history_query_evidence(f, has ? chan : NULL, inc, limit > 0 && limit <= 1000 ? limit : 100)) {
                fclose(f);
                free(out);
                api_error(resp, 500, "the history could not be read");
                return;
            }
            fclose(f);
            resp->status = 200;
            resp->type = "application/json";
            resp->body = out;
            resp->body_len = len;
            return;
        }
    }
    if (!strcmp(p, "/api/technician/ask")) {
        /* POST {"question"}: {"id"}; GET ?id=: how it is, the answer when there is one */
        char v[32], *out = NULL, q2[64];
        size_t len = 0;
        FILE *f;

        if (!strcmp(req->method, "POST")) {
            struct json_doc *doc = malloc(sizeof(*doc));
            char err[200];
            int root = doc ? json_parse(doc, req->body ? req->body : "{}", err, sizeof(err)) : -1;
            const char *question = root >= 0 ? json_get_str(doc, root, "question", "") : "";
            int64_t id = *question ? tech_ask(question) : 0;

            free(doc);
            if (!id) {
                api_error(resp, *question ? 503 : 400, *question ? "the technician is not running on this server" : "no question");
                return;
            }
            snprintf(q2, sizeof(q2), "%lld", (long long)id);
            http_resp_printf(resp, 200, "application/json", "{\"ok\": true, \"id\": %s}\n", q2);
            return;
        }
        f = open_memstream(&out, &len);
        if (!f)
            return;
        if (!query_param(req->query, "id", v, sizeof(v)) || !tech_answer_json(f, strtoll(v, NULL, 10))) {
            fclose(f);
            free(out);
            api_error(resp, 404, "no such question (the last 16 are kept)");
            return;
        }
        fclose(f);
        resp->status = 200;
        resp->type = "application/json";
        resp->body = out;
        resp->body_len = len;
        return;
    }
    if (!strcmp(p, "/api/technician/prompt")) {
        /* ?incident=ID: what the model is given about it; ?chat=1: the chat's state */
        char v[32], *out = NULL;
        size_t len = 0;
        FILE *f = open_memstream(&out, &len);
        bool ok;

        if (!f)
            return;
        if (query_param(req->query, "chat", v, sizeof(v))) {
            /* ?chat=1: how things are now, as a question in the chat is given it */
            tech_context(f, s);
            ok = true;
        } else {
            ok = query_param(req->query, "incident", v, sizeof(v)) && tech_prompt(f, strtoll(v, NULL, 10));
        }
        fclose(f);
        if (!ok) {
            free(out);
            api_error(resp, 404, "no such incident");
            return;
        }
        resp->status = 200;
        resp->type = "application/json";
        resp->body = out;
        resp->body_len = len;
        return;
    }
    if (!strcmp(p, "/api/technician") || !strcmp(p, "/api/technician/diagnose")) {
        /* GET: how it is; POST /diagnose {"incident": ID}: look at that one now */
        char *out = NULL;
        size_t len = 0;
        FILE *f;

        if (!strcmp(p, "/api/technician/diagnose")) {
            struct json_doc *doc = malloc(sizeof(*doc));
            char err[200];
            int root = doc ? json_parse(doc, req->body ? req->body : "{}", err, sizeof(err)) : -1;
            int64_t id = root >= 0 ? (int64_t)json_get_num(doc, root, "incident", 0) : 0;

            free(doc);
            if (strcmp(req->method, "POST") || id <= 0) {
                api_error(resp, 400, "POST {\"incident\": ID}");
                return;
            }
            tech_queue(id, true);
        } else if (!strcmp(req->method, "PUT")) {
            /* {"model": a .gguf on this server, "gpu": "auto"|"cpu"|"all"|N, "keep_loaded": bool} */
            struct json_doc *doc = malloc(sizeof(*doc));
            char err[200], model[512], gpu[16];
            int root = doc ? json_parse(doc, req->body ? req->body : "{}", err, sizeof(err)) : -1;
            size_t ml;
            bool gpu_ok, keep;

            if (root < 0) {
                free(doc);
                api_error(resp, 400, "PUT {\"model\", \"gpu\", \"keep_loaded\"}");
                return;
            }
            pthread_mutex_lock(&s->lock);
            snprintf(model, sizeof(model), "%s", json_get_str(doc, root, "model", s->tech.model));
            snprintf(gpu, sizeof(gpu), "%s", json_get_str(doc, root, "gpu", s->tech.gpu));
            keep = json_get_bool(doc, root, "keep_loaded", s->tech.idle_s == 0);
            pthread_mutex_unlock(&s->lock);
            free(doc);
            gpu_ok = !strcmp(gpu, "auto") || !strcmp(gpu, "cpu") || !strcmp(gpu, "all") ||
                     (gpu[0] >= '0' && gpu[0] <= '9' && !gpu[1]);
            {
                /* under the models folder, or beside the model set now */
                char cur[512], *sl;

                pthread_mutex_lock(&s->lock);
                snprintf(cur, sizeof(cur), "%s", s->tech.model);
                pthread_mutex_unlock(&s->lock);
                if ((sl = strrchr(cur, '/')))
                    sl[1] = 0;
                if (!strncmp(model, "/var/lib/caricoder3/models/", 27) || (sl && !strncmp(model, cur, strlen(cur))))
                    ml = strlen(model);
                else
                    ml = 0;
            }
            if (!ml || strstr(model, "..") || ml < 6 || strcmp(model + ml - 5, ".gguf") || access(model, R_OK) != 0) {
                api_error(resp, 400, "model: a .gguf file under /var/lib/caricoder3/models");
                return;
            }
            if (!gpu_ok) {
                api_error(resp, 400, "gpu: auto, cpu, all or a card's number");
                return;
            }
            pthread_mutex_lock(&s->lock);
            snprintf(s->tech.model, sizeof(s->tech.model), "%s", model);
            snprintf(s->tech.gpu, sizeof(s->tech.gpu), "%s", gpu);
            s->tech.idle_s = keep ? 0 : 600;
            {
                struct tech_conf tc = s->tech;

                save_system_config(s);
                pthread_mutex_unlock(&s->lock);
                tech_set(&tc);
            }
            ev(NULL, "info", "technician", "model set to %s on %s%s", strrchr(model, '/') + 1, gpu,
               keep ? ", kept loaded" : "");
        }
        f = open_memstream(&out, &len);
        if (!f)
            return;
        tech_state_json(f);
        fflush(f);
        if (len && out[len - 1] == '}') {
            /* and the verdicts so far */
            fseek(f, -1, SEEK_END);
            fprintf(f, ", \"verdicts\": ");
            history_verdict_counts(f);
            fputc('}', f);
        }
        fclose(f);
        resp->status = 200;
        resp->type = "application/json";
        resp->body = out;
        resp->body_len = len;
        return;
    }
    if (!strcmp(p, "/api/peers")) {
        /* GET: this server's name, the peers and how they are, the conversation; PUT {"name", "key", "peers"} */
        char *out = NULL;
        size_t len = 0;
        FILE *f;

        if (!strcmp(req->method, "PUT")) {
            struct json_doc *doc = malloc(sizeof(*doc));
            char err[200];
            int root = doc ? json_parse(doc, req->body ? req->body : "", err, sizeof(err)) : -1, a;

            if (root < 0) {
                free(doc);
                api_error(resp, 400, doc ? err : "out of memory");
                return;
            }
            pthread_mutex_lock(&s->lock);
            if (json_get(doc, root, "name") >= 0 && *json_get_str(doc, root, "name", ""))
                snprintf(s->name, sizeof(s->name), "%s", json_get_str(doc, root, "name", ""));
            if (json_get(doc, root, "key") >= 0)
                snprintf(s->peer_key, sizeof(s->peer_key), "%s", json_get_str(doc, root, "key", ""));
            if (json_get(doc, root, "timezone") >= 0 && !set_timezone(s, json_get_str(doc, root, "timezone", ""))) {
                pthread_mutex_unlock(&s->lock);
                free(doc);
                api_error(resp, 400, "timezone: a name such as America/Barbados, or empty for the server's own");
                return;
            }
            if ((a = json_get(doc, root, "peers")) >= 0) {
                s->n_peers = 0;
                for (int j = doc->node[a].first_child; j >= 0 && s->n_peers < PEERS_MAX; j = doc->node[j].next) {
                    const char *u = json_get_str(doc, j, "url", "");

                    if (strncmp(u, "http://", 7))
                        continue;
                    snprintf(s->peers[s->n_peers].name, sizeof(s->peers[0].name), "%s", json_get_str(doc, j, "name", u + 7));
                    snprintf(s->peers[s->n_peers].url, sizeof(s->peers[0].url), "%s", u);
                    s->n_peers++;
                }
            }
            save_system_config(s);
            peers_apply(s);
            pthread_mutex_unlock(&s->lock);
            free(doc);
            ev(NULL, "info", "user", "peers changed");
        } else if (strcmp(req->method, "GET")) {
            api_error(resp, 405, "GET or PUT");
            return;
        }
        f = open_memstream(&out, &len);
        if (!f)
            return;
        pthread_mutex_lock(&s->lock);
        peers_json(s, f);
        pthread_mutex_unlock(&s->lock);
        fclose(f);
        resp->status = 200;
        resp->type = "application/json";
        resp->body = out;
        resp->body_len = len;
        return;
    }
    if (!strcmp(p, "/api/irds") || !strcmp(p, "/api/irds/check")) {
        /* GET: the IRDs and their last readings; PUT {"irds": [{"name", "address", "kind",
         * "raw", "outputs"}]}: the list; POST .../check: ask them all now */
        char *out = NULL;
        size_t len = 0;
        FILE *f;

        if (!strcmp(p, "/api/irds/check")) {
            if (strcmp(req->method, "POST")) {
                api_error(resp, 405, "POST");
                return;
            }
            ird_check_now();
        } else if (!strcmp(req->method, "PUT")) {
            struct json_doc *doc = malloc(sizeof(*doc));
            char err[200];
            int root = doc ? json_parse(doc, req->body ? req->body : "", err, sizeof(err)) : -1, a;
            static struct ird_conf list[IRDS_MAX];
            int n = 0;

            a = root >= 0 ? json_get(doc, root, "irds") : -1;
            if (root < 0 || a < 0) {
                free(doc);
                api_error(resp, 400, root < 0 ? (doc ? err : "out of memory") : "PUT {\"irds\": [...]}");
                return;
            }
            for (int j = doc->node[a].first_child; j >= 0; j = doc->node[j].next) {
                if (n == IRDS_MAX) {
                    free(doc);
                    api_error(resp, 400, "too many IRDs");
                    return;
                }
                if (!ird_conf_read(doc, j, &list[n], err, sizeof(err))) {
                    free(doc);
                    api_error(resp, 400, err);
                    return;
                }
                for (int k = 0; k < n; k++)
                    if (!strcmp(list[k].name, list[n].name)) {
                        free(doc);
                        api_error(resp, 400, "two IRDs with one name");
                        return;
                    }
                n++;
            }
            free(doc);
            pthread_mutex_lock(&s->lock);
            memcpy(s->irds, list, sizeof(list[0]) * (size_t)n);
            s->n_irds = n;
            save_system_config(s);
            irds_apply(s);
            pthread_mutex_unlock(&s->lock);
            ev(NULL, "info", "user", "IRDs changed: %d", n);
            ird_check_now();
        } else if (strcmp(req->method, "GET")) {
            api_error(resp, 405, "GET, PUT or POST .../check");
            return;
        }
        f = open_memstream(&out, &len);
        if (!f)
            return;
        fprintf(f, "{\"ok\": true, \"irds\": ");
        ird_json(f);
        fprintf(f, "}\n");
        fclose(f);
        resp->status = 200;
        resp->type = "application/json";
        resp->body = out;
        resp->body_len = len;
        return;
    }
    if (!strcmp(p, "/api/watchdog")) {
        /* GET: the limits, the rules, what is being watched now; PUT: new limits */
        struct wd_limits L;
        char *out = NULL;
        size_t len = 0;
        FILE *f;

        wd_get_limits(&L);
        if (!strcmp(req->method, "PUT")) {
            struct json_doc *doc = malloc(sizeof(*doc));
            char err[200];
            int root = doc ? json_parse(doc, req->body ? req->body : "", err, sizeof(err)) : -1;

            if (root < 0) {
                free(doc);
                api_error(resp, 400, doc ? err : "out of memory");
                return;
            }
            wd_limits_read(doc, root, &L);
            free(doc);
            wd_set_limits(&L);
            pthread_mutex_lock(&s->lock);
            save_system_config(s);
            pthread_mutex_unlock(&s->lock);
            ev(NULL, "info", "user", "watchdog limits changed");
        } else if (strcmp(req->method, "GET")) {
            api_error(resp, 405, "GET or PUT");
            return;
        }
        f = open_memstream(&out, &len);
        if (!f)
            return;
        fprintf(f, "{\"ok\": true, \"limits\": ");
        wd_limits_write(f, &L);
        fprintf(f, ", \"rules\": [");
        for (int r = 0; r < WD_RULES; r++)
            fprintf(f, "%s{\"id\": \"%s\", \"name\": \"%s\", \"on\": %s}", r ? ", " : "", wd_rule_id(r),
                    wd_rule_name(r), L.off[r] ? "false" : "true");
        fprintf(f, "], \"watching\": ");
        wd_state_json(f, time(NULL));
        fprintf(f, "}\n");
        fclose(f);
        resp->status = 200;
        resp->type = "application/json";
        resp->body = out;
        resp->body_len = len;
        return;
    }
    if (!strcmp(p, "/api/system")) {
        char *out = NULL;
        size_t len = 0;
        FILE *f = open_memstream(&out, &len);
        char host[128] = "?";
        int running = 0, failed = 0, nch = 0;

        if (!f)
            return;
        gethostname(host, sizeof(host));
        pthread_mutex_lock(&s->lock);
        for (int i = 0; i < s->n; i++) {
            if (!s->ch[i].cfg)
                continue;               /* a deleted channel's slot */
            nch++;
            running += s->ch[i].state == CH_RUNNING;
            failed += s->ch[i].state == CH_FAILED;
        }
        fprintf(f, "{\"ok\": true, \"version\": \"%s\", \"host\": %s, \"uptime\": %.0f, \"channels\": %d, "
                   "\"running\": %d, \"failed\": %d, \"open_incidents\": %d, \"port\": %d, \"channels_dir\": ",
                CC3_VERSION, json_quote(host, q, sizeof(q)), (double)(clock_now_ns() - s->t_start) / 1e9,
                nch, running, failed, wd_open_count(), s->port);
        fprintf(f, "%s, \"shares\": [", json_quote(s->channels_dir, q, sizeof(q)));
        {
            /* the shared inputs: who uses them, and the reader's own stats */
            int n_sh = 0;

            for (int k = 0; k < SHARES_MAX; k++) {
                const struct share *sh = &s->sh[k];
                char path[600], *st;
                int users = 0;

                if (!sh->used)
                    continue;
                for (int i = 0; i < s->n; i++)
                    users += s->ch[i].share_port && (s->ch[i].want || s->ch[i].pid > 0) &&
                             !strcmp(s->ch[i].sh_url, sh->url);
                share_stats_path(s, k, path, sizeof(path));
                st = read_file(path, NULL);
                fprintf(f, "%s{\"url\": %s, \"running\": %s, \"channels\": %d, \"restarts\": %d, \"reader\": %s}",
                        n_sh++ ? ", " : "", json_quote(sh->url, q, sizeof(q)), sh->pid > 0 ? "true" : "false",
                        users, sh->restarts, st && st[0] == '{' ? st : "null");
                free(st);
            }
        }
        fprintf(f, "], \"irds\": ");
        ird_json(f);
        fprintf(f, ", \"card_mode\": \"%s\", \"card_processes\": [", s->card_mode[0] ? s->card_mode : "auto");
        for (int k = 0; k < s->n_cards; k++) {
            /* the card processes: running, restarts, and their own list of sessions */
            char sock[600], path[600], *st;

            card_paths(s, s->cards[k].index, sock, sizeof(sock), path, sizeof(path));
            st = s->cards[k].pid > 0 ? read_file(path, NULL) : NULL;
            fprintf(f, "%s{\"card\": %d, \"running\": %s, \"restarts\": %d, \"up_s\": %.0f, \"process\": %s}",
                    k ? ", " : "", s->cards[k].index, s->cards[k].pid > 0 ? "true" : "false", s->cards[k].restarts,
                    s->cards[k].pid > 0 ? (double)(clock_now_ns() - s->cards[k].started_ns) / 1e9 : 0.0,
                    st && st[0] == '{' ? st : "null");
            free(st);
        }
        fprintf(f, "], \"history\": ");
        history_info(f);
        fprintf(f, ", \"disks\": ");
        disks_json(s, f);
        fprintf(f, ", \"system\": ");
        pthread_mutex_unlock(&s->lock);
        sysstat_json(f);
        fprintf(f, ", \"backends\": ");
        {
            char *b = NULL, *qb = malloc(16384);
            size_t bl = 0;
            FILE *bf = open_memstream(&b, &bl);

            if (bf && qb) {
                codec_list(bf);
                sink_list(bf);
                fclose(bf);
                fprintf(f, "%s", json_quote(b ? b : "", qb, 16384));
            } else {
                if (bf)
                    fclose(bf);
                fprintf(f, "\"\"");
            }
            free(b);
            free(qb);
        }
        fprintf(f, "}\n");
        fclose(f);
        resp->status = 200;
        resp->type = "application/json";
        resp->body = out;
        resp->body_len = len;
        return;
    }
    if (!strcmp(p, "/api/probe") && !strcmp(req->method, "POST")) {
        struct json_doc *doc = malloc(sizeof(*doc));
        char err[200];
        int root = doc ? json_parse(doc, req->body ? req->body : "", err, sizeof(err)) : -1;
        const char *url = root >= 0 ? json_get_str(doc, root, "url", NULL) : NULL;
        double seconds = root >= 0 ? json_get_num(doc, root, "seconds", 3) : 3;
        int program = root >= 0 ? (int)json_get_num(doc, root, "program", 0) : 0;
        char secs[16], prog[16];
        char *argv[8];
        int n = 0, pfd[2];
        pid_t pid;

        if (!url || !url[0] || !strstr(url, "://")) {
            api_error(resp, 400, root < 0 ? err : "needs \"url\" with a scheme");
            free(doc);
            return;
        }
        if (seconds < 1)
            seconds = 1;
        if (seconds > 30)
            seconds = 30;
        snprintf(secs, sizeof(secs), "%.0f", seconds);
        snprintf(prog, sizeof(prog), "%d", program);
        argv[n++] = s->binary;
        argv[n++] = "probe";
        argv[n++] = (char *)url;
        argv[n++] = "--seconds";
        argv[n++] = secs;
        if (program > 0) {
            argv[n++] = "--program";
            argv[n++] = prog;
        }
        argv[n] = NULL;
        if (pipe2(pfd, O_CLOEXEC) < 0) {
            api_error(resp, 500, "pipe");
            free(doc);
            return;
        }
        pid = fork();
        if (pid == 0) {
            dup2(pfd[1], 1);
            close(pfd[0]);
            close(pfd[1]);
            {
                int devnull = open("/dev/null", O_RDWR);

                if (devnull >= 0) {
                    dup2(devnull, 0);
                    dup2(devnull, 2);
                }
            }
            execv(argv[0], argv);
            _exit(127);
        }
        close(pfd[1]);
        free(doc);
        if (pid < 0) {
            close(pfd[0]);
            api_error(resp, 500, "fork");
            return;
        }
        {
            char *out = NULL;
            size_t len = 0, cap = 0;
            int64_t deadline = clock_now_ns() + (int64_t)((seconds + 15) * 1e9);
            bool timed_out = false;

            for (;;) {
                struct pollfd pf = { pfd[0], POLLIN, 0 };
                int64_t left = deadline - clock_now_ns();
                int rc;

                if (left <= 0) {
                    timed_out = true;
                    break;
                }
                rc = poll(&pf, 1, (int)(left / 1000000));
                if (rc <= 0) {
                    if (rc < 0 && errno == EINTR)
                        continue;
                    timed_out = rc == 0;
                    break;
                }
                if (len + 65536 > cap) {
                    char *nb = realloc(out, cap + 262144);

                    if (!nb)
                        break;
                    out = nb;
                    cap += 262144;
                }
                ssize_t r = read(pfd[0], out + len, cap - len - 1);

                if (r <= 0)
                    break;
                len += (size_t)r;
            }
            close(pfd[0]);
            if (timed_out)
                kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            if (timed_out || !out || len == 0) {
                free(out);
                api_error(resp, 504, timed_out ? "the probe did not finish" : "the probe gave nothing");
                return;
            }
            out[len] = 0;
            resp->status = strstr(out, "\"ok\": true") ? 200 : 400;
            resp->type = "application/json";
            resp->body = out;
            resp->body_len = len;
        }
        return;
    }
    if (!strcmp(p, "/api/channels") && !strcmp(req->method, "GET")) {
        char *out = NULL;
        size_t len = 0;
        FILE *f = open_memstream(&out, &len);

        if (!f)
            return;
        struct chan_view views[CHANNELS_MAX];
        int nv = 0;

        char bv[8];
        bool brief = query_param(req->query, "brief", bv, sizeof(bv)) && !strcmp(bv, "1");
        struct json_doc *bd = brief ? malloc(sizeof(*bd)) : NULL;

        fprintf(f, "{\"ok\": true, \"open_incidents\": %d, \"channels\": [", wd_open_count());
        pthread_mutex_lock(&s->lock);
        for (int i = 0; i < s->n; i++)
            if (s->ch[i].cfg)
                chan_view_take(&s->ch[i], &views[nv++], false);
        pthread_mutex_unlock(&s->lock);
        for (int i = 0; i < nv; i++) {
            fprintf(f, "%s\n", i ? "," : "");
            if (bd) {
                /* ?brief=1: what a list needs; no question to each channel for its outputs */
                char bq[200], *st = NULL;
                struct chan tmp;
                int cr;
                char err[100];

                memset(&tmp, 0, sizeof(tmp));
                snprintf(tmp.name, sizeof(tmp.name), "%.63s", views[i].name);
                fprintf(f, "{\"name\": %s, \"state\": \"%s\", \"enabled\": %s, \"uptime\": %.0f, \"restarts\": %d, \"why\": ",
                        json_quote(views[i].name, bq, sizeof(bq)), state_name(views[i].state),
                        views[i].enabled ? "true" : "false", views[i].uptime, views[i].restarts);
                fputs(json_quote(views[i].why, bq, sizeof(bq)), f);
                cr = views[i].cfg ? json_parse(bd, views[i].cfg, err, sizeof(err)) : -1;
                fprintf(f, ", \"config\": {\"input\": %s}", json_quote(cr >= 0 ? json_get_str(bd, cr, "input", "") : "", bq, sizeof(bq)));
                if (views[i].state == CH_RUNNING) {
                    st = chan_stats(s, &tmp);
                    fputs(", \"stats\": ", f);
                    stats_brief(f, bd, st);
                    free(st);
                }
                fputc('}', f);
            } else {
                json_chan_view(s, &views[i], f, false);
            }
            chan_view_drop(&views[i]);
        }
        free(bd);
        fprintf(f, "]}\n");
        fclose(f);
        resp->status = 200;
        resp->type = "application/json";
        resp->body = out;
        resp->body_len = len;
        return;
    }
    if (!strncmp(p, "/api/channels/", 14)) {
        char name[64], rest[256] = "";
        const char *n = p + 14, *slash = strchr(n, '/');
        size_t nl = slash ? (size_t)(slash - n) : strlen(n);
        struct chan *c;
        char path[600], err[300];

        if (nl == 0 || nl >= sizeof(name)) {
            api_error(resp, 404, "no such channel");
            return;
        }
        memcpy(name, n, nl);
        name[nl] = 0;
        if (slash)
            snprintf(rest, sizeof(rest), "%s", slash + 1);
        if (!name_ok(name)) {
            api_error(resp, 400, "a channel name is letters, digits, - and _ (up to 48)");
            return;
        }
        snprintf(path, sizeof(path), "%s/%s.json", s->channels_dir, name);

        if (!rest[0] && !strcmp(req->method, "PUT")) {
            struct json_doc *doc = malloc(sizeof(*doc));
            int root = doc ? json_parse(doc, req->body ? req->body : "", err, sizeof(err)) : -1;

            if (root < 0 || doc->node[root].type != JSON_OBJECT) {
                api_error(resp, 400, root < 0 ? err : "the config must be a JSON object");
                free(doc);
                return;
            }
            if (!json_get_str(doc, root, "input", NULL)) {
                api_error(resp, 400, "the config needs \"input\"");
                free(doc);
                return;
            }
            free(doc);
            if (!write_file(path, req->body, err, sizeof(err))) {
                api_error(resp, 500, err);
                return;
            }
            pthread_mutex_lock(&s->lock);
            scan_channels(s);
            c = find_chan(s, name);
            pthread_mutex_unlock(&s->lock);
            http_resp_printf(resp, c ? 200 : 500, "application/json", "{\"ok\": %s}\n", c ? "true" : "false");
            return;
        }
        pthread_mutex_lock(&s->lock);
        c = find_chan(s, name);
        if (!c || !c->cfg) {
            pthread_mutex_unlock(&s->lock);
            api_error(resp, 404, "no such channel");
            return;
        }
        if (!rest[0] && !strcmp(req->method, "GET")) {
            char *out = NULL;
            size_t len = 0;
            FILE *f = open_memstream(&out, &len);
            struct chan_view v;

            chan_view_take(c, &v, true);
            pthread_mutex_unlock(&s->lock);
            if (f) {
                fprintf(f, "{\"ok\": true, \"channel\": ");
                json_chan_view(s, &v, f, true);
                fprintf(f, "}\n");
                fclose(f);
                resp->status = 200;
                resp->type = "application/json";
                resp->body = out;
                resp->body_len = len;
            }
            chan_view_drop(&v);
            return;
        } else if (!rest[0] && !strcmp(req->method, "DELETE")) {
            char opath[600];

            c->want = false;
            chan_stop(s, c);
            unlink(path);
            snprintf(opath, sizeof(opath), "%s/%s.outputs.json", s->channels_dir, name);
            unlink(opath);
            snprintf(opath, sizeof(opath), "%s/%s.streams", s->channels_dir, name);
            unlink(opath);
            free(c->cfg);
            c->cfg = NULL;
            http_resp_text(resp, 200, "application/json", "{\"ok\": true}\n");
        } else if (!strcmp(rest, "start") && !strcmp(req->method, "POST")) {
            ev(c->name, "info", "user", "start asked for");
            c->want = true;
            c->restarts = 0;
            c->next_start_ns = 0;
            if (c->state == CH_STOPPED || c->state == CH_FAILED)
                chan_start(s, c);
            http_resp_text(resp, 200, "application/json", "{\"ok\": true}\n");
        } else if (!strcmp(rest, "stop") && !strcmp(req->method, "POST")) {
            ev(c->name, "info", "user", "stop asked for");
            c->want = false;
            chan_stop(s, c);
            if (c->state == CH_FAILED)
                c->state = CH_STOPPED;
            http_resp_text(resp, 200, "application/json", "{\"ok\": true}\n");
        } else if (!strcmp(rest, "listen") && !strcmp(req->method, "POST")) {
            /* the language check, now */
            char lerr[200];

            if (c->state != CH_RUNNING) {
                api_error(resp, 409, "the channel is not running");
            } else if (c->lang_busy) {
                http_resp_text(resp, 200, "application/json", "{\"ok\": true, \"note\": \"listening already\"}\n");
            } else if (!lang_listen(s, c, 0, true, lerr, sizeof(lerr))) {
                api_error(resp, 409, lerr);
            } else {
                http_resp_text(resp, 200, "application/json", "{\"ok\": true}\n");
            }
        } else if (!strcmp(rest, "restart") && !strcmp(req->method, "POST")) {
            ev(c->name, "info", "user", "restart asked for");
            c->want = true;
            c->restarts = 0;
            c->next_start_ns = 0;
            if (c->state == CH_RUNNING)
                chan_stop(s, c);
            else if (c->state != CH_STOPPING)
                chan_start(s, c);
            http_resp_text(resp, 200, "application/json", "{\"ok\": true}\n");
        } else if (!strcmp(rest, "preview") && !strcmp(req->method, "POST")) {
            /* {"drop": [pid, ...]}: start (or restart with another audio) */
            struct json_doc *doc = malloc(sizeof(*doc));
            int root = doc ? json_parse(doc, req->body && req->body[0] ? req->body : "{}", err, sizeof(err)) : -1;
            char drop[128] = "";
            size_t o = 0;
            int a = root >= 0 ? json_get(doc, root, "drop") : -1;
            bool ok = true;

            if (a >= 0 && doc->node[a].type == JSON_ARRAY)
                for (int j = doc->node[a].first_child; j >= 0; j = doc->node[j].next)
                    if (doc->node[j].type == JSON_NUMBER && o + 8 < sizeof(drop))
                        o += (size_t)snprintf(drop + o, sizeof(drop) - o, "%s%d", o ? "," : "", (int)doc->node[j].num);
            free(doc);
            if (c->pv_pid > 0 && strcmp(drop, c->pv_drop)) {
                /* another audio: stopped, and started again once it is gone */
                preview_stop(c);
                c->pv_restart = true;
                snprintf(c->pv_next, sizeof(c->pv_next), "%s", drop);
                c->pv_last_hit_ns = clock_now_ns();
            } else if (c->pv_pid <= 0) {
                ok = preview_start(s, c, drop, err, sizeof(err));
            } else {
                c->pv_last_hit_ns = clock_now_ns();
            }
            pthread_mutex_unlock(&s->lock);
            if (!ok)
                api_error(resp, 409, err);
            else
                http_resp_printf(resp, 200, "application/json",
                                 "{\"ok\": true, \"url\": \"/hls/%s.preview/index.m3u8\"}\n", c->name);
            return;
        } else if (!strcmp(rest, "preview") && !strcmp(req->method, "DELETE")) {
            c->pv_restart = false;
            preview_stop(c);
            pthread_mutex_unlock(&s->lock);
            http_resp_text(resp, 200, "application/json", "{\"ok\": true}\n");
            return;
        } else if (!strcmp(rest, "thumb.jpg")) {
            char tpath[600];
            size_t len = 0;
            char *data;

            snprintf(tpath, sizeof(tpath), "%.400s/%s.jpg", s->run_dir, c->name);
            pthread_mutex_unlock(&s->lock);
            data = read_file(tpath, &len);
            if (!data || len == 0) {
                free(data);
                api_error(resp, 404, "no thumbnail yet");
                return;
            }
            resp->status = 200;
            resp->type = "image/jpeg";
            resp->body = data;
            resp->body_len = len;
            /* the GUI asks with a 10 s cache key; a redraw in between must not refetch */
            snprintf(resp->extra, sizeof(resp->extra), "Cache-Control: max-age=10\r\n");
            return;
        } else if (!strcmp(rest, "log")) {
            char *out = NULL;
            size_t len = 0;
            FILE *f = open_memstream(&out, &len);

            if (f) {
                fprintf(f, "{\"ok\": true, \"log\": [");
                for (int i = 0; i < c->log_n; i++)
                    fprintf(f, "%s%s", i ? ", " : "", json_quote(c->log[(c->log_head + i) % LOG_LINES], q, sizeof(q)));
                fprintf(f, "]}\n");
                fclose(f);
                resp->status = 200;
                resp->type = "application/json";
                resp->body = out;
                resp->body_len = len;
            }
        } else if (!strcmp(rest, "outputs") && !strcmp(req->method, "POST")) {
            struct json_doc *doc = malloc(sizeof(*doc));
            int root = doc ? json_parse(doc, req->body ? req->body : "", err, sizeof(err)) : -1;
            const char *url, *id;
            char reqline[1400], q2[600], q3[200], *reply, evname[64], evurl[512];
            bool quiet;

            if (root < 0 || !(url = json_get_str(doc, root, "url", NULL))) {
                pthread_mutex_unlock(&s->lock);
                api_error(resp, 400, root < 0 ? err : "needs \"url\"");
                free(doc);
                return;
            }
            id = json_get_str(doc, root, "id", "");
            quiet = !strcmp(id, "preview");         /* the player's own, not worth an event */
            snprintf(evname, sizeof(evname), "%s", c->name);
            snprintf(evurl, sizeof(evurl), "%s", url);
            if (c->state != CH_RUNNING) {
                pthread_mutex_unlock(&s->lock);
                api_error(resp, 409, "the channel is not running; start it, then add outputs");
                free(doc);
                return;
            }
            snprintf(reqline, sizeof(reqline), "{\"cmd\": \"add-output\", \"url\": %s, \"id\": %s, \"save\": %s}",
                     json_quote(url, q2, sizeof(q2)), json_quote(id, q3, sizeof(q3)),
                     json_get_bool(doc, root, "save", true) ? "true" : "false");
            free(doc);
            {
                struct chan tmp;

                memset(&tmp, 0, sizeof(tmp));
                snprintf(tmp.name, sizeof(tmp.name), "%s", c->name);
                pthread_mutex_unlock(&s->lock);
                reply = chan_ctl(s, &tmp, reqline);
            }
            if (reply) {
                http_resp_text(resp, strstr(reply, "\"ok\": true") ? 200 : 400, "application/json", reply);
                if (!quiet && strstr(reply, "\"ok\": true"))
                    ev(evname, "info", "output", "output added: %s", evurl);
                free(reply);
            } else {
                api_error(resp, 502, "the channel did not answer");
            }
            return;
        } else if (!strncmp(rest, "outputs/", 8) && !strcmp(req->method, "PUT")) {
            /* {"url": "..."}: the output with this id, changed in place */
            struct json_doc *doc = malloc(sizeof(*doc));
            int root = doc ? json_parse(doc, req->body ? req->body : "", err, sizeof(err)) : -1;
            const char *url;
            char reqline[1400], q2[600], q3[200], *reply;
            struct chan tmp;

            if (root < 0 || !(url = json_get_str(doc, root, "url", NULL))) {
                pthread_mutex_unlock(&s->lock);
                api_error(resp, 400, root < 0 ? err : "needs \"url\"");
                free(doc);
                return;
            }
            if (c->state != CH_RUNNING) {
                pthread_mutex_unlock(&s->lock);
                api_error(resp, 409, "the channel is not running");
                free(doc);
                return;
            }
            snprintf(reqline, sizeof(reqline), "{\"cmd\": \"replace-output\", \"id\": %s, \"url\": %s}",
                     json_quote(rest + 8, q3, sizeof(q3)), json_quote(url, q2, sizeof(q2)));
            free(doc);
            memset(&tmp, 0, sizeof(tmp));
            snprintf(tmp.name, sizeof(tmp.name), "%s", c->name);
            pthread_mutex_unlock(&s->lock);
            reply = chan_ctl(s, &tmp, reqline);
            if (reply) {
                http_resp_text(resp, strstr(reply, "\"ok\": true") ? 200 : 400, "application/json", reply);
                if (strstr(reply, "\"ok\": true")) ev(tmp.name, "info", "output", "output %s changed", rest + 8);
                free(reply);
            } else {
                api_error(resp, 502, "the channel did not answer");
            }
            return;
        } else if (!strncmp(rest, "outputs/", 8) && !strcmp(req->method, "DELETE")) {
            char reqline[400], q3[200], *reply;
            struct chan tmp;

            if (c->state != CH_RUNNING) {
                pthread_mutex_unlock(&s->lock);
                api_error(resp, 409, "the channel is not running");
                return;
            }
            snprintf(reqline, sizeof(reqline), "{\"cmd\": \"remove-output\", \"id\": %s}",
                     json_quote(rest + 8, q3, sizeof(q3)));
            memset(&tmp, 0, sizeof(tmp));
            snprintf(tmp.name, sizeof(tmp.name), "%s", c->name);
            pthread_mutex_unlock(&s->lock);
            reply = chan_ctl(s, &tmp, reqline);
            if (reply) {
                http_resp_text(resp, strstr(reply, "\"ok\": true") ? 200 : 400, "application/json", reply);
                if (strstr(reply, "\"ok\": true") && strcmp(rest + 8, "preview"))
                    ev(tmp.name, "info", "output", "output %s removed", rest + 8);
                free(reply);
            } else {
                api_error(resp, 502, "the channel did not answer");
            }
            return;
        } else {
            api_error(resp, 404, "unknown request");
        }
        pthread_mutex_unlock(&s->lock);
        return;
    }
    resp->status = 404;
}

/* ---- main ---------------------------------------------------------------------- */

static void usage(FILE *f)
{
    fprintf(f,
"Usage: caricoder3 serve [options]\n"
"  --config PATH      system config (default " DEFAULT_CONFIG ")\n"
"  --port N           listen on this port (default %d; saved to the config)\n"
"  --listen ADDR      listen on this address (default 0.0.0.0; saved)\n"
"  --password TEXT    set the GUI password (saved) and exit\n"
"  --show-password    print the GUI password and exit\n"
"  --show-port        print the port and exit\n"
"  -v                 more logging\n"
"Channels are the *.json files in the channels directory (default\n"
"/etc/caricoder3/channels); the GUI makes them, or write them by hand.\n", DEFAULT_PORT);
}

int super_main(int argc, char **argv);
int super_tech_eval(int argc, char **argv);

/* caricoder3 tech-eval: models tried against the verdicts (tech.h) */
int super_tech_eval(int argc, char **argv)
{
    static struct super s;
    struct tech_eval_opts o;
    char err[300], outp[512];
    static char found[TECH_EVAL_MODELS][600];
    int n_found = 0;

    memset(&o, 0, sizeof(o));
    o.cases = 12;
    o.gpu = "auto";
    o.port = 8561;
    snprintf(s.config_path, sizeof(s.config_path), DEFAULT_CONFIG);
    outp[0] = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc)
            snprintf(s.config_path, sizeof(s.config_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--model") && i + 1 < argc && o.n_models < TECH_EVAL_MODELS)
            o.models[o.n_models++] = argv[++i];
        else if (!strcmp(argv[i], "--cases") && i + 1 < argc)
            o.cases = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gpu") && i + 1 < argc)
            o.gpu = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc)
            o.port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--context") && i + 1 < argc)
            o.ctx = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc)
            o.threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)
            snprintf(outp, sizeof(outp), "%s", argv[++i]);
        else {
            fprintf(stderr,
                    "usage: caricoder3 tech-eval [--model FILE.gguf ...] [--cases 12] [--gpu auto|cpu|all|N]\n"
                    "                            [--threads N] [--context TOKENS] [--port 8561] [--out FILE] [--config FILE]\n"
                    "  Tries each model on incidents you gave a verdict on (one or two of each kind), with\n"
                    "  exactly what the technician is sent, and scores the answers against your verdicts.\n"
                    "  Without --model: the technician's own model and every .gguf in\n"
                    "  /var/lib/caricoder3/models/eval. The running technician is not disturbed.\n");
            return 2;
        }
    }
    if (!load_system_config(&s, err, sizeof(err))) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    if (!history_open_readonly(s.history_path, err, sizeof(err))) {
        fprintf(stderr, "history: %s\n", err);
        return 1;
    }
    snprintf(s.tech.my_name, sizeof(s.tech.my_name), "%s", s.name);
    if (!o.n_models) {
        DIR *d = opendir("/var/lib/caricoder3/models/eval");
        struct dirent *e;

        snprintf(found[n_found++], sizeof(found[0]), "%s", s.tech.model);
        while (d && (e = readdir(d)) && n_found < TECH_EVAL_MODELS) {
            size_t n = strlen(e->d_name);

            if (n > 5 && !strcmp(e->d_name + n - 5, ".gguf"))
                snprintf(found[n_found++], sizeof(found[0]), "/var/lib/caricoder3/models/eval/%.500s", e->d_name);
        }
        if (d)
            closedir(d);
        for (int i = 0; i < n_found; i++)
            o.models[o.n_models++] = found[i];
    }
    if (!outp[0]) {
        time_t t = time(NULL);
        struct tm tm;

        localtime_r(&t, &tm);
        strftime(outp, sizeof(outp), "/var/lib/caricoder3/tech-eval-%Y%m%d-%H%M.jsonl", &tm);
    }
    o.out = outp;
    if (o.threads > 0)
        s.tech.threads = o.threads;     /* CPU threads for the test's model server */
    sysstat_start();
    sleep(6);                       /* the cards' free memory, from the first samples */
    return tech_eval(&s.tech, &o);
}

/* The JSON string starting at `p` (just after its opening quote),
 * unescaped onto `f`; returns where it ended. */
static const char *put_unescaped(FILE *f, const char *p)
{
    for (; *p && *p != '"'; p++) {
        if (*p != '\\' || !p[1]) {
            fputc(*p, f);
            continue;
        }
        switch (*++p) {
        case 'n': fputc('\n', f); break;
        case 't': fputc('\t', f); break;
        case 'r': break;
        case 'u': {
            unsigned v = 0;

            if (sscanf(p + 1, "%4x", &v) == 1)
                p += 4;
            if (v < 0x80)
                fputc((int)v, f);
            else if (v < 0x800) {
                fputc(0xc0 | (v >> 6), f);
                fputc(0x80 | (v & 0x3f), f);
            } else {
                fputc(0xe0 | (v >> 12), f);
                fputc(0x80 | ((v >> 6) & 0x3f), f);
                fputc(0x80 | (v & 0x3f), f);
            }
            break;
        }
        default: fputc(*p, f); break;
        }
    }
    return p;
}

int super_tech_prompt(int argc, char **argv);

/* caricoder3 tech-prompt: exactly what the technician would be sent about
 * an incident, without a model: to read it, and check the past cases it
 * is shown. */
int super_tech_prompt(int argc, char **argv)
{
    static struct super s;
    char err[300], *out = NULL;
    size_t len = 0;
    int64_t id = 0;
    bool raw = false, ok;
    FILE *m;

    snprintf(s.config_path, sizeof(s.config_path), DEFAULT_CONFIG);
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc)
            snprintf(s.config_path, sizeof(s.config_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--id") && i + 1 < argc)
            id = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--json"))
            raw = true;
        else
            id = -1;
    }
    if (id <= 0) {
        fprintf(stderr,
                "usage: caricoder3 tech-prompt --id INCIDENT [--json] [--config FILE]\n"
                "  Prints what the technician would be sent about that incident: its notes, the\n"
                "  incident, and the past cases it is shown. --json: the request as sent.\n");
        return 2;
    }
    if (!load_system_config(&s, err, sizeof(err))) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    if (!history_open_readonly(s.history_path, err, sizeof(err))) {
        fprintf(stderr, "history: %s\n", err);
        return 1;
    }
    snprintf(s.tech.my_name, sizeof(s.tech.my_name), "%s", s.name);
    m = open_memstream(&out, &len);
    if (!m)
        return 1;
    ok = tech_prompt_with(&s.tech, m, id);
    fclose(m);
    if (!ok || !out || !len) {
        fprintf(stderr, "no incident %lld in the history\n", (long long)id);
        free(out);
        return 1;
    }
    if (raw || out[0] != '{' || strstr(out, "\"ok\": false")) {
        fputs(out, stdout);
        fputc('\n', stdout);
    } else {
        /* each message's role and content, as plain text */
        const char *p = out;

        while ((p = strstr(p, "\"role\"")) != NULL) {
            const char *r = strchr(p + 6, '"'), *c;

            if (!r)
                break;
            printf("===== ");
            r = put_unescaped(stdout, r + 1);
            printf(" =====\n");
            c = strstr(r, "\"content\"");
            if (!c || !(c = strchr(c + 9, '"')))
                break;
            p = put_unescaped(stdout, c + 1);
            printf("\n\n");
        }
    }
    free(out);
    return ok ? 0 : 1;
}

int super_main(int argc, char **argv)
{
    static struct super s;
    char err[300];
    const char *set_password = NULL;
    bool show_password = false, show_port = false, port_given = false, listen_given = false;
    int port = 0;
    const char *listen_addr = NULL;
    struct sigaction sa;

    memset(&s, 0, sizeof(s));
    snprintf(s.config_path, sizeof(s.config_path), DEFAULT_CONFIG);
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc)
            snprintf(s.config_path, sizeof(s.config_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            port = atoi(argv[++i]);
            port_given = true;
        } else if (!strcmp(argv[i], "--listen") && i + 1 < argc) {
            listen_addr = argv[++i];
            listen_given = true;
        } else if (!strcmp(argv[i], "--password") && i + 1 < argc)
            set_password = argv[++i];
        else if (!strcmp(argv[i], "--show-password"))
            show_password = true;
        else if (!strcmp(argv[i], "--show-port"))
            show_port = true;
        else if (!strcmp(argv[i], "-v"))
            log_level = LOG_DBG;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout);
            return 0;
        } else {
            usage(stderr);
            return 2;
        }
    }
    if (!load_system_config(&s, err, sizeof(err))) {
        log_err("config: %s", err);
        return 1;
    }
    {
        char *slash;

        if (readlink("/proc/self/exe", s.binary, sizeof(s.binary) - 1) < 0)
            snprintf(s.binary, sizeof(s.binary), "/usr/local/bin/caricoder3");
        slash = strrchr(s.config_path, '/');
        if (slash) {
            char dir[512];

            snprintf(dir, sizeof(dir), "%.*s", (int)(slash - s.config_path), s.config_path);
            mkdir(dir, 0755);
        }
    }
    if (port_given)
        s.port = port;
    if (listen_given)
        snprintf(s.listen, sizeof(s.listen), "%s", listen_addr);
    if (set_password) {
        if (strlen(set_password) < 6 || strlen(set_password) >= sizeof(s.password)) {
            fprintf(stderr, "the password must be 6 to %zu characters\n", sizeof(s.password) - 1);
            return 2;
        }
        snprintf(s.password, sizeof(s.password), "%s", set_password);
        save_system_config(&s);
        printf("password saved to %s\n", s.config_path);
        return 0;
    }
    if (!s.password[0]) {
        make_password(s.password, sizeof(s.password));
        save_system_config(&s);
        log_info("GUI password made up and saved to %s (caricoder3 serve --show-password)", s.config_path);
    } else if (port_given || listen_given) {
        save_system_config(&s);
    }
    if (show_password) {
        printf("admin / %s\n", s.password);
        return 0;
    }
    if (show_port) {
        printf("%d\n", s.port);
        return 0;
    }
    mkdir(s.channels_dir, 0755);
    mkdir(s.run_dir, 0775);
    cards_detect(&s);
    pthread_mutex_init(&s.lock, NULL);
    s.t_start = clock_now_ns();

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    sysstat_start();
    if (s.history_path[0] && strcmp(s.history_path, "none")) {
        char herr[300];

        if (!history_open(s.history_path, s.history_max_mb, herr, sizeof(herr)))
            log_warn("history: %s", herr);
        else
            ev(NULL, "info", "server", "supervisor started (caricoder3 %s)", CC3_VERSION);
    }
    wd_set_snapshot(wd_snapshot, &s);
    wd_set_on_open(on_incident, &s);
    peers_apply(&s);
    ird_set_uses(chan_uses, &s);
    irds_apply(&s);
    snprintf(s.tech.my_name, sizeof(s.tech.my_name), "%s", s.name);
    tech_set_context(tech_context, &s);
    tech_set(&s.tech);
    s.http = http_start(s.listen, s.port, handle, &s, err, sizeof(err));
    if (!s.http) {
        log_err("web: %s", err);
        return 1;
    }
    log_info("caricoder3 %s supervisor: http://%s:%d/ (user admin), channels in %s",
             CC3_VERSION, s.listen, s.port, s.channels_dir);
    pthread_mutex_lock(&s.lock);
    scan_channels(&s);
    tick(&s);
    pthread_mutex_unlock(&s.lock);

    while (!g_stop) {
        usleep(200000);
        pthread_mutex_lock(&s.lock);
        if (g_hup) {
            g_hup = 0;
            scan_channels(&s);
        }
        tick(&s);
        pthread_mutex_unlock(&s.lock);
    }

    log_info("stopping the channels");
    pthread_mutex_lock(&s.lock);
    for (int i = 0; i < s.n; i++) {
        s.ch[i].want = false;
        chan_stop(&s, &s.ch[i]);
    }
    for (int k = 0; k < STOP_GRACE_MS / 100 + 20; k++) {
        bool any = false;

        tick(&s);
        for (int i = 0; i < s.n; i++)
            any |= s.ch[i].pid > 0;
        /* the channels gone, their card processes follow */
        s.stopping_all = !any;
        for (int j = 0; j < SHARES_MAX; j++)
            any |= s.sh[j].pid > 0;
        for (int j = 0; j < s.n_cards; j++)
            any |= s.cards[j].pid > 0;
        if (!any)
            break;
        pthread_mutex_unlock(&s.lock);
        usleep(100000);
        pthread_mutex_lock(&s.lock);
    }
    pthread_mutex_unlock(&s.lock);
    http_stop(s.http);
    ev(NULL, "info", "server", "supervisor stopped");
    tech_stop();
    peer_stop();
    ird_stop();
    history_close();
    sysstat_stop();
    for (int i = 0; i < s.n; i++)
        free(s.ch[i].cfg);
    log_info("stopped");
    return 0;
}
