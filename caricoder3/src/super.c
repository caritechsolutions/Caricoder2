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
 *   /api/probe                 POST {url, seconds, program}: what is in a source
 *                              (caricoder3 probe, run as a child; up to 30 s)
 *   /hls/NAME/...              the channel's HLS output, proxied
 *
 * Everything but /hls needs HTTP basic authentication: user "admin",
 * the password in the system config (made up and written there on the
 * first run, printed in the log).
 *
 * A channel file (channels/NAME.json) holds what the command line would:
 * {"input": "udp://...", "program": 0, "muxrate": "5M", "vbr": false,
 *  "delay": 500, "video": ["h264:3M", "640x360:h264:1M"], "audio": ["aac:128k"], "drop": [258],
 *  "decoder": "auto", "encoder": "auto", "gpu": 0, "preset": "", "gop": 2000,
 *  "vbv": 1000, "service_name": "", "service_provider": "", "enabled": true}
 * Its outputs live in channels/NAME.outputs.json, which the channel
 * itself reads and writes (--config), so outputs added while it runs
 * survive a restart.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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

#define CHANNELS_MAX    64
#define LOG_LINES       200
#define LOG_LINE_MAX    240
#define STOP_GRACE_MS   6000
#define DEFAULT_PORT    8550
#define DEFAULT_CONFIG  "/etc/caricoder3/caricoder3.json"

extern const unsigned char gui_html[];
extern const unsigned int gui_html_len;

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
};

struct super {
    char config_path[512];
    char listen[64];
    int port;
    char channels_dir[512];
    char run_dir[512];
    char password[80];
    char binary[512];
    int64_t t_start;

    pthread_mutex_t lock;                   /* channels, from the web threads */
    struct chan ch[CHANNELS_MAX];
    int n;
    struct http_server *http;
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

static void save_system_config(struct super *s)
{
    char text[2048], q[4][600], err[200];

    snprintf(text, sizeof(text),
             "{\n  \"listen\": %s,\n  \"port\": %d,\n  \"channels_dir\": %s,\n"
             "  \"run_dir\": %s,\n  \"password\": %s\n}\n",
             json_quote(s->listen, q[0], sizeof(q[0])), s->port,
             json_quote(s->channels_dir, q[1], sizeof(q[1])),
             json_quote(s->run_dir, q[2], sizeof(q[2])),
             json_quote(s->password, q[3], sizeof(q[3])));
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
    if (s->port < 1 || s->port > 65535) {
        snprintf(err, errlen, "port must be 1..65535");
        return false;
    }
    return true;
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

/* Builds the command line from the channel's JSON. Returns the number
 * of arguments, -1 with a reason. */
static int chan_argv(struct super *s, struct chan *c, char **argv, int max, char *err, size_t errlen)
{
    static struct json_doc doc;            /* under the supervisor's lock */
    static char store[64][512];
    int root, n = 0, k = 0;
    const char *input;
#define ARG(fmt, ...) do { if (n + 1 >= max || k >= 64) goto full; \
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
    ARG("%s", input);
    ARGS("--name");
    ARGS(c->name);
    ARGS("--run-dir");
    ARGS(s->run_dir);
    ARGS("--config");
    ARG("%.400s/%s.outputs.json", s->channels_dir, c->name);
    ARGS("--stats-file");
    ARG("%.400s/%s.json", s->run_dir, c->name);
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
        v = json_get_num(&doc, root, "pcr_interval", 0);
        if (v > 0)
            ARG("--pcr-interval=%d", (int)v);
        v = json_get_num(&doc, root, "psi_interval", 0);
        if (v > 0)
            ARG("--psi-interval=%d", (int)v);
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
    }
    {
        static const char *const strs[][2] = {
            { "decoder", "--decoder=%s" }, { "encoder", "--encoder=%s" }, { "preset", "--preset=%s" },
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
    char *argv[80];
    char err[200];
    int pfd[2];
    pid_t pid;

    if (chan_argv(s, c, argv, 80, err, sizeof(err)) < 0) {
        snprintf(c->why, sizeof(c->why), "config: %.140s", err);
        c->state = CH_FAILED;
        c->want = false;
        chan_log_line(c, c->why);
        log_err("channel %s: %s", c->name, err);
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
        log_warn("channel %s: %s; starting again in %lld s", c->name, c->why, (long long)(wait / 1000000000LL));
    }
    (void)s;
}

/* Once around: logs, exits, restarts, stop timeouts. Called with the lock. */
static void tick(struct super *s)
{
    int64_t now = clock_now_ns();

    for (int i = 0; i < s->n; i++) {
        struct chan *c = &s->ch[i];
        int status;

        chan_read_log(c);
        if (c->pid > 0 && waitpid(c->pid, &status, WNOHANG) == c->pid)
            chan_reap(s, c, status);
        if (c->state == CH_STOPPING && now - c->stop_asked_ns > STOP_GRACE_MS * 1000000LL) {
            log_warn("channel %s: did not stop in %d s, killing it", c->name, STOP_GRACE_MS / 1000);
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
            memset(c, 0, sizeof(*c));
            snprintf(c->name, sizeof(c->name), "%s", name);
            c->log_fd = -1;
            c->state = CH_STOPPED;
        }
        if (!c->cfg || strcmp(c->cfg, text)) {
            bool changed = c->cfg != NULL;

            free(c->cfg);
            c->cfg = text;
            c->enabled = json_get_bool(&doc, root, "enabled", true);
            c->want = c->enabled;
            if (changed && c->state == CH_RUNNING) {
                log_info("channel %s: config changed, restarting", c->name);
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
    char why[160];
    char *cfg;
    char *log;                              /* JSON array text, or NULL */
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
    snprintf(v->why, sizeof(v->why), "%s", c->why);
    v->cfg = c->cfg ? strdup(c->cfg) : NULL;
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
    if (full)
        fprintf(f, ", \"log\": %s", v->log ? v->log : "[]");
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
    if (!name_ok(name)) {
        resp->status = 404;
        return;
    }
    snprintf(path, sizeof(path), "%.400s/%s.http", s->run_dir, name);
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
    snprintf(line, sizeof(line), "GET %.900s%s%.900s HTTP/1.1\r\nHost: caricoder3\r\n\r\n",
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

static void handle(void *opaque, const struct http_req *req, struct http_resp *resp)
{
    struct super *s = opaque;
    const char *p = req->path;
    char q[600];

    if (!strncmp(p, "/hls/", 5)) {
        proxy_hls(s, req, resp);
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
    if (!strcmp(p, "/api/system")) {
        char *out = NULL;
        size_t len = 0;
        FILE *f = open_memstream(&out, &len);
        char host[128] = "?";
        int running = 0;

        if (!f)
            return;
        gethostname(host, sizeof(host));
        pthread_mutex_lock(&s->lock);
        for (int i = 0; i < s->n; i++)
            running += s->ch[i].state == CH_RUNNING;
        fprintf(f, "{\"ok\": true, \"version\": \"%s\", \"host\": %s, \"uptime\": %.0f, \"channels\": %d, "
                   "\"running\": %d, \"port\": %d, \"channels_dir\": ",
                CC3_VERSION, json_quote(host, q, sizeof(q)), (double)(clock_now_ns() - s->t_start) / 1e9,
                s->n, running, s->port);
        fprintf(f, "%s, \"system\": ", json_quote(s->channels_dir, q, sizeof(q)));
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

        fprintf(f, "{\"ok\": true, \"channels\": [");
        pthread_mutex_lock(&s->lock);
        for (int i = 0; i < s->n; i++)
            if (s->ch[i].cfg)
                chan_view_take(&s->ch[i], &views[nv++], false);
        pthread_mutex_unlock(&s->lock);
        for (int i = 0; i < nv; i++) {
            fprintf(f, "%s\n", i ? "," : "");
            json_chan_view(s, &views[i], f, false);
            chan_view_drop(&views[i]);
        }
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
            free(c->cfg);
            c->cfg = NULL;
            http_resp_text(resp, 200, "application/json", "{\"ok\": true}\n");
        } else if (!strcmp(rest, "start") && !strcmp(req->method, "POST")) {
            c->want = true;
            c->restarts = 0;
            c->next_start_ns = 0;
            if (c->state == CH_STOPPED || c->state == CH_FAILED)
                chan_start(s, c);
            http_resp_text(resp, 200, "application/json", "{\"ok\": true}\n");
        } else if (!strcmp(rest, "stop") && !strcmp(req->method, "POST")) {
            c->want = false;
            chan_stop(s, c);
            if (c->state == CH_FAILED)
                c->state = CH_STOPPED;
            http_resp_text(resp, 200, "application/json", "{\"ok\": true}\n");
        } else if (!strcmp(rest, "restart") && !strcmp(req->method, "POST")) {
            c->want = true;
            c->restarts = 0;
            c->next_start_ns = 0;
            if (c->state == CH_RUNNING)
                chan_stop(s, c);
            else if (c->state != CH_STOPPING)
                chan_start(s, c);
            http_resp_text(resp, 200, "application/json", "{\"ok\": true}\n");
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
            char reqline[1400], q2[600], q3[200], *reply;

            if (root < 0 || !(url = json_get_str(doc, root, "url", NULL))) {
                pthread_mutex_unlock(&s->lock);
                api_error(resp, 400, root < 0 ? err : "needs \"url\"");
                free(doc);
                return;
            }
            id = json_get_str(doc, root, "id", "");
            if (c->state != CH_RUNNING) {
                pthread_mutex_unlock(&s->lock);
                api_error(resp, 409, "the channel is not running; start it, then add outputs");
                free(doc);
                return;
            }
            snprintf(reqline, sizeof(reqline), "{\"cmd\": \"add-output\", \"url\": %s, \"id\": %s, \"save\": true}",
                     json_quote(url, q2, sizeof(q2)), json_quote(id, q3, sizeof(q3)));
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
    pthread_mutex_init(&s.lock, NULL);
    s.t_start = clock_now_ns();

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    sysstat_start();
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
        if (!any)
            break;
        pthread_mutex_unlock(&s.lock);
        usleep(100000);
        pthread_mutex_lock(&s.lock);
    }
    pthread_mutex_unlock(&s.lock);
    http_stop(s.http);
    sysstat_stop();
    for (int i = 0; i < s.n; i++)
        free(s.ch[i].cfg);
    log_info("stopped");
    return 0;
}
