/*
 * caricoder3 - the technician
 * Copyright (c) 2026 CariTech Solutions
 */
#include "tech.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "history.h"
#include "json.h"
#include "log.h"
#include "peer.h"
#include "sysstat.h"

#define QUEUE_MAX 32
#define GPU_NEED_MB 7000        /* a 7B model at Q4 with its context, and some room */
#define START_WAIT_S 180        /* loading a model from disk */

struct job {
    int64_t id;
    int64_t due;                /* wall clock */
    int tries;                  /* not in the record yet (written every 2 s): looked for again */
};

static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t th;
    bool running, stop;
    struct tech_conf c;
    struct job q[QUEUE_MAX];
    int qn;
    pid_t server;               /* llama-server, 0: not running */
    char where[32];             /* "gpu1", "cpu" */
    char state[32];             /* "off", "idle", "starting", "thinking", "unavailable" */
    char last_error[300];
    int64_t last_used, started;
    int done, failed;
    int64_t last_id, last_t;
    double last_seconds;
} T = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

static void set_state(const char *st)
{
    snprintf(T.state, sizeof(T.state), "%s", st);
}

/* ---- the model server ---------------------------------------------------------- */

static void server_stop(void)
{
    if (T.server > 0) {
        kill(T.server, SIGTERM);
        for (int i = 0; i < 50 && waitpid(T.server, NULL, WNOHANG) == 0; i++)
            usleep(100000);
        kill(T.server, SIGKILL);
        waitpid(T.server, NULL, 0);
        log_info("technician: model stopped (idle)");
    }
    T.server = 0;
}

/* A card with room: the most memory free, at least GPU_NEED_MB, not flat out. -1: none. */
static int pick_gpu(const char *want)
{
    int idx[8], free_mb[8], busy[8], n = sysstat_gpu_free(idx, free_mb, busy, 8), best = -1;

    if (!strcmp(want, "cpu"))
        return -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(want, "auto") && atoi(want) != idx[i])
            continue;
        if (free_mb[i] < GPU_NEED_MB || busy[i] > 90)
            continue;
        if (best < 0 || free_mb[i] > free_mb[best])
            best = i;
    }
    return best >= 0 ? idx[best] : -1;
}

static bool server_up(void)
{
    char url[64], *out = NULL;
    int st;

    snprintf(url, sizeof(url), "http://127.0.0.1:%d", T.c.port);
    st = peer_http(url, "GET", "/health", NULL, NULL, 2000, &out);
    free(out);
    return st == 200;
}

/* Called with the lock released (it waits). */
static bool server_start(struct tech_conf *c, char *err, size_t errlen)
{
    int gpu = pick_gpu(c->gpu);
    char port[16], ctx[16], threads[16], ngl[8], logp[600];
    pid_t pid;

    if (access(c->server, X_OK) != 0) {
        snprintf(err, errlen, "no model server at %.240s (the installer builds it)", c->server);
        return false;
    }
    if (access(c->model, R_OK) != 0) {
        snprintf(err, errlen, "no model at %.240s (the installer downloads it)", c->model);
        return false;
    }
    snprintf(port, sizeof(port), "%d", c->port);
    snprintf(ctx, sizeof(ctx), "%d", c->ctx);
    snprintf(threads, sizeof(threads), "%d", c->threads);
    snprintf(ngl, sizeof(ngl), "%d", gpu >= 0 ? 99 : 0);
    snprintf(logp, sizeof(logp), "/tmp/caricoder3-technician.log");
    pid = fork();
    if (pid < 0) {
        snprintf(err, errlen, "fork: %s", strerror(errno));
        return false;
    }
    if (pid == 0) {
        char *argv[] = { c->server, "-m", c->model, "--host", "127.0.0.1", "--port", port, "-c", ctx, "-t", threads,
                         "-ngl", ngl, "-np", "1", "--no-webui", NULL };
        int fd = open(logp, O_WRONLY | O_CREAT | O_TRUNC, 0644), nul = open("/dev/null", O_RDONLY);
        char dev[16];

        if (fd >= 0)
            dup2(fd, 1), dup2(fd, 2);
        if (nul >= 0)
            dup2(nul, 0);
        for (int f = 3; f < 1024; f++)
            close(f);
        setpgid(0, 0);
        if (gpu >= 0) {
            snprintf(dev, sizeof(dev), "%d", gpu);
            setenv("CUDA_VISIBLE_DEVICES", dev, 1);
            setenv("GGML_VK_VISIBLE_DEVICES", dev, 1);
        } else {
            setenv("CUDA_VISIBLE_DEVICES", "", 1);
        }
        /* on the CPU it never competes with the channels */
        setpriority(PRIO_PROCESS, 0, 19);
        execv(c->server, argv);
        _exit(127);
    }
    pthread_mutex_lock(&T.lock);
    T.server = pid;
    T.started = time(NULL);
    if (gpu >= 0)
        snprintf(T.where, sizeof(T.where), "gpu%d", gpu);
    else
        snprintf(T.where, sizeof(T.where), "cpu");
    set_state("starting");
    pthread_mutex_unlock(&T.lock);
    log_info("technician: starting the model on %s", gpu >= 0 ? "a card" : "the CPU");
    for (int i = 0; i < START_WAIT_S * 2; i++) {
        int status;

        if (waitpid(pid, &status, WNOHANG) == pid) {
            pthread_mutex_lock(&T.lock);
            T.server = 0;
            pthread_mutex_unlock(&T.lock);
            snprintf(err, errlen, "the model server stopped while starting (see %.200s)", logp);
            return false;
        }
        if (server_up())
            return true;
        usleep(500000);
    }
    snprintf(err, errlen, "the model did not load in %d s", START_WAIT_S);
    pthread_mutex_lock(&T.lock);
    server_stop();
    pthread_mutex_unlock(&T.lock);
    return false;
}

/* ---- the prompt ------------------------------------------------------------------ */

static void append_file(FILE *f, const char *path)
{
    FILE *in = fopen(path, "r");
    char buf[4096];
    size_t n;

    if (!in)
        return;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        fwrite(buf, 1, n, f);
    fclose(in);
    fputs("\n\n", f);
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* technician.md first, then the other notes of each folder in name order */
static void knowledge(FILE *f, const struct tech_conf *c)
{
    for (int k = 0; k < 2; k++) {
        DIR *d = c->knowledge[k][0] ? opendir(c->knowledge[k]) : NULL;
        struct dirent *e;
        char *names[64];
        int n = 0;
        char path[1100];

        if (!d)
            continue;
        while ((e = readdir(d)) && n < 64) {
            size_t l = strlen(e->d_name);

            if (l > 3 && !strcmp(e->d_name + l - 3, ".md"))
                names[n++] = strdup(e->d_name);
        }
        closedir(d);
        qsort(names, (size_t)n, sizeof(names[0]), by_name);
        for (int i = 0; i < n; i++)
            if (!strcmp(names[i], "technician.md")) {
                snprintf(path, sizeof(path), "%s/%s", c->knowledge[k], names[i]);
                append_file(f, path);
            }
        for (int i = 0; i < n; i++) {
            if (strcmp(names[i], "technician.md")) {
                snprintf(path, sizeof(path), "%s/%s", c->knowledge[k], names[i]);
                append_file(f, path);
            }
            free(names[i]);
        }
    }
}

/* JSON without the spaces between its parts (fewer tokens), in place */
static void compact(char *s)
{
    char *o = s;
    bool in = false, esc = false;

    for (; *s; s++) {
        if (in) {
            *o++ = *s;
            if (esc)
                esc = false;
            else if (*s == '\\')
                esc = true;
            else if (*s == '"')
                in = false;
        } else if (*s == '"') {
            in = true;
            *o++ = *s;
        } else if (*s != ' ' && *s != '\n' && *s != '\t' && *s != '\r') {
            *o++ = *s;
        }
    }
    *o = 0;
}

/* a JSON string of `s` */
static void jesc(FILE *f, const char *s, size_t n)
{
    fputc('"', f);
    for (size_t i = 0; i < n && s[i]; i++) {
        unsigned char ch = (unsigned char)s[i];

        if (ch == '"' || ch == '\\')
            fprintf(f, "\\%c", ch);
        else if (ch == '\n')
            fputs("\\n", f);
        else if (ch == '\t')
            fputs("\\t", f);
        else if (ch < 0x20)
            fprintf(f, "\\u%04x", ch);
        else
            fputc(ch, f);
    }
    fputc('"', f);
}

static const char SCHEMA[] =
    "{\"type\": \"object\", \"properties\": {"
    "\"cause\": {\"type\": \"string\", \"maxLength\": 300},"
    "\"category\": {\"type\": \"string\", \"enum\": [\"source_provider\", \"source_ird\", \"satellite_signal\", \"network\","
    " \"transcoder\", \"normal_content\", \"unknown\"]},"
    "\"confidence\": {\"type\": \"number\", \"minimum\": 0, \"maximum\": 1},"
    "\"explanation\": {\"type\": \"string\", \"maxLength\": 1200},"
    "\"next_checks\": {\"type\": \"array\", \"items\": {\"type\": \"string\", \"maxLength\": 200}, \"maxItems\": 4},"
    "\"ticket\": {\"type\": \"string\", \"maxLength\": 800}},"
    " \"required\": [\"cause\", \"category\", \"confidence\", \"explanation\", \"next_checks\", \"ticket\"]}";

/* The request to the model, malloc'd; NULL when the incident is not there
 * (or belongs to a source's: that one is looked at instead, *parent set). */
static char *build_request(const struct tech_conf *c, int64_t id, int64_t *parent)
{
    char *sys = NULL, *rep = NULL, *sim = NULL, *req = NULL;
    size_t sl = 0, rl = 0, ml = 0, ql = 0;
    FILE *f;

    *parent = 0;
    if (!(f = open_memstream(&rep, &rl)))
        return NULL;
    if (!history_report_incident(f, id, time(NULL))) {
        fclose(f);
        free(rep);
        return NULL;
    }
    fclose(f);
    {
        const char *p = strstr(rep, "\"parent\": ");

        /* the incident's own "parent" comes first in the text */
        if (p && strncmp(p + 10, "null", 4))
            *parent = strtoll(p + 10, NULL, 10);
        if (*parent) {
            free(rep);
            return NULL;
        }
    }
    compact(rep);
    if ((f = open_memstream(&sim, &ml))) {
        history_similar(f, id, 6);
        fclose(f);
        if (sim)
            compact(sim);
    }
    if ((f = open_memstream(&sys, &sl))) {
        knowledge(f, c);
        fclose(f);
    }
    if ((f = open_memstream(&req, &ql))) {
        char user[256];

        fprintf(f, "{\"messages\": [{\"role\": \"system\", \"content\": ");
        jesc(f, sys ? sys : "", 120000);
        fprintf(f, "}, {\"role\": \"user\", \"content\": ");
        {
            char *u = NULL;
            size_t ul = 0;
            FILE *uf = open_memstream(&u, &ul);
            struct peer_state ps[PEERS_MAX];
            int np = peer_get(ps, PEERS_MAX);

            if (uf) {
                fprintf(uf, "This transcoder is %s.", c->my_name);
                if (np) {
                    fprintf(uf, " Its peers:");
                    for (int i = 0; i < np; i++)
                        fprintf(uf, " %s (%s)%s", ps[i].name, ps[i].reachable ? "answering" : "not answering",
                                i + 1 < np ? "," : ".");
                }
                fprintf(uf, "\n\nThe incident, with its evidence and the events around it (JSON; times are Unix "
                            "seconds):\n%.40000s\n\nEarlier incidents like it, with what you said then and what a "
                            "person said (JSON; null where nobody said):\n%.12000s\n\nWhat caused this incident? "
                            "Answer with the JSON object only.", rep, sim ? sim : "[]");
                fclose(uf);
                jesc(f, u ? u : "", ul);
            }
            free(u);
        }
        snprintf(user, sizeof(user), "}], \"temperature\": 0.2, \"max_tokens\": 1500, \"stream\": false, ");
        fputs(user, f);
        fprintf(f, "\"response_format\": {\"type\": \"json_schema\", \"json_schema\": {\"name\": \"diagnosis\", "
                   "\"schema\": %s}}}", SCHEMA);
        fclose(f);
    }
    free(sys);
    free(rep);
    free(sim);
    return req;
}

/* ---- the thread ---------------------------------------------------------------------- */

static void requeue(int64_t id, int tries);

static void diagnose(int64_t id, int tries)
{
    struct tech_conf c;
    char err[300] = "", url[64], *out = NULL, *req;
    int64_t parent = 0, t0;
    int st;
    bool up;

    pthread_mutex_lock(&T.lock);
    c = T.c;
    up = T.server > 0;
    pthread_mutex_unlock(&T.lock);
    req = build_request(&c, id, &parent);
    if (!req) {
        if (parent)
            tech_queue(parent, true);       /* the source's incident says more */
        else if (tries < 10)
            requeue(id, tries + 1);         /* not written yet */
        return;
    }
    if (!up || !server_up()) {
        pthread_mutex_lock(&T.lock);
        server_stop();
        pthread_mutex_unlock(&T.lock);
        if (!server_start(&c, err, sizeof(err))) {
            pthread_mutex_lock(&T.lock);
            set_state("unavailable");
            snprintf(T.last_error, sizeof(T.last_error), "%s", err);
            T.failed++;
            pthread_mutex_unlock(&T.lock);
            log_warn("technician: %s", err);
            free(req);
            return;
        }
    }
    pthread_mutex_lock(&T.lock);
    set_state("thinking");
    pthread_mutex_unlock(&T.lock);
    t0 = time(NULL);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d", c.port);
    st = peer_http(url, "POST", "/v1/chat/completions", NULL, req, 600000, &out);
    free(req);
    {
        static struct json_doc doc;         /* this thread only */
        char e2[200];
        const char *content = NULL;
        int root = st == 200 && out ? json_parse(&doc, out, e2, sizeof(e2)) : -1;

        if (root >= 0) {
            int ch = json_get(&doc, root, "choices");

            if (ch >= 0 && doc.node[ch].first_child >= 0) {
                int msg = json_get(&doc, doc.node[ch].first_child, "message");

                content = msg >= 0 ? json_get_str(&doc, msg, "content", NULL) : NULL;
            }
        }
        if (content && *content == '{') {
            static struct json_doc dd;
            char e3[200], text[700], q[400];
            int r = json_parse(&dd, content, e3, sizeof(e3));

            if (r >= 0) {
                double conf = json_get_num(&dd, r, "confidence", 0);

                history_incident_diagnosis(id, time(NULL), content);
                if (!(conf >= 0))
                    conf = 0;
                if (conf > 1)
                    conf = conf <= 100 ? conf / 100 : 1;
                snprintf(text, sizeof(text), "%.300s (%s, %.0f%% sure)", json_get_str(&dd, r, "cause", "?"),
                         json_get_str(&dd, r, "category", "?"), conf * 100);
                history_message(time(NULL), "technician", "out", text, "{}", id);
                (void)q;
                pthread_mutex_lock(&T.lock);
                T.done++;
                set_state("idle");
                T.last_id = id;
                T.last_t = time(NULL);
                T.last_seconds = (double)(time(NULL) - t0);
                T.last_error[0] = 0;
                pthread_mutex_unlock(&T.lock);
                log_info("technician: incident %lld: %s", (long long)id, text);
            } else {
                snprintf(err, sizeof(err), "the model's answer was not JSON: %.100s", content);
            }
        } else {
            snprintf(err, sizeof(err), "no answer from the model (%d): %.150s", st, out ? out : "");
        }
    }
    free(out);
    pthread_mutex_lock(&T.lock);
    if (err[0]) {
        snprintf(T.last_error, sizeof(T.last_error), "%s", err);
        T.failed++;
        log_warn("technician: incident %lld: %s", (long long)id, err);
    }
    T.last_used = time(NULL);
    set_state("idle");
    pthread_mutex_unlock(&T.lock);
}

static void *thread_main(void *arg)
{
    (void)arg;
    for (;;) {
        int64_t id = 0, now = time(NULL);
        int tries = 0;
        struct timespec ts;

        pthread_mutex_lock(&T.lock);
        if (T.stop) {
            server_stop();
            pthread_mutex_unlock(&T.lock);
            break;
        }
        for (int i = 0; i < T.qn; i++)
            if (T.q[i].due <= now) {
                id = T.q[i].id;
                tries = T.q[i].tries;
                memmove(T.q + i, T.q + i + 1, sizeof(T.q[0]) * (size_t)(T.qn - i - 1));
                T.qn--;
                break;
            }
        if (!id) {
            if (T.server > 0 && now - T.last_used > T.c.idle_s && now - T.started > T.c.idle_s) {
                server_stop();
                set_state("off");
            }
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&T.cond, &T.lock, &ts);
            pthread_mutex_unlock(&T.lock);
            continue;
        }
        pthread_mutex_unlock(&T.lock);
        diagnose(id, tries);
    }
    return NULL;
}

void tech_set(const struct tech_conf *c)
{
    pthread_mutex_lock(&T.lock);
    T.c = *c;
    if (T.c.port <= 0)
        T.c.port = 8551;
    if (T.c.ctx <= 0)
        T.c.ctx = 16384;
    if (T.c.threads <= 0)
        T.c.threads = 8;
    if (T.c.idle_s <= 0)
        T.c.idle_s = 600;
    if (!T.state[0])
        set_state(c->enabled ? "off" : "disabled");
    if (c->enabled && !T.running) {
        T.stop = false;
        T.running = pthread_create(&T.th, NULL, thread_main, NULL) == 0;
    }
    pthread_mutex_unlock(&T.lock);
}

void tech_stop(void)
{
    pthread_mutex_lock(&T.lock);
    if (!T.running) {
        pthread_mutex_unlock(&T.lock);
        return;
    }
    T.stop = true;
    pthread_cond_signal(&T.cond);
    pthread_mutex_unlock(&T.lock);
    pthread_join(T.th, NULL);
    T.running = false;
}

static void requeue(int64_t id, int tries)
{
    pthread_mutex_lock(&T.lock);
    if (T.qn < QUEUE_MAX) {
        T.q[T.qn].id = id;
        T.q[T.qn].due = time(NULL) + 3;
        T.q[T.qn].tries = tries;
        T.qn++;
    }
    pthread_mutex_unlock(&T.lock);
}

void tech_queue(int64_t id, bool now)
{
    pthread_mutex_lock(&T.lock);
    if (T.c.enabled && T.running && id > 0) {
        bool dup = false;

        for (int i = 0; i < T.qn; i++)
            dup |= T.q[i].id == id;
        if (!dup && T.qn < QUEUE_MAX) {
            T.q[T.qn].id = id;
            T.q[T.qn].due = time(NULL) + (now ? 0 : T.c.delay_s);
            T.q[T.qn].tries = 0;
            T.qn++;
            pthread_cond_signal(&T.cond);
        }
    }
    pthread_mutex_unlock(&T.lock);
}

bool tech_prompt(FILE *f, int64_t id)
{
    struct tech_conf c;
    int64_t parent = 0;
    char *req;

    pthread_mutex_lock(&T.lock);
    c = T.c;
    pthread_mutex_unlock(&T.lock);
    req = build_request(&c, id, &parent);
    if (!req) {
        if (parent)
            fprintf(f, "{\"ok\": false, \"error\": \"it belongs to incident %lld, which is the one looked at\"}", (long long)parent);
        return parent != 0;
    }
    fputs(req, f);
    free(req);
    return true;
}

void tech_state_json(FILE *f)
{
    pthread_mutex_lock(&T.lock);
    fprintf(f, "{\"enabled\": %s, \"state\": \"%s\", \"where\": \"%s\", \"model\": ", T.c.enabled ? "true" : "false",
            T.c.enabled ? (T.state[0] ? T.state : "off") : "disabled", T.server > 0 ? T.where : "");
    jesc(f, strrchr(T.c.model, '/') ? strrchr(T.c.model, '/') + 1 : T.c.model, 300);
    fprintf(f, ", \"model_present\": %s, \"server_present\": %s, \"setting_up\": %s, \"gpu\": ",
            access(T.c.model, R_OK) == 0 ? "true" : "false", access(T.c.server, X_OK) == 0 ? "true" : "false",
            access("/var/lib/caricoder3/technician-setup.running", F_OK) == 0 ? "true" : "false");
    jesc(f, T.c.gpu, 16);
    fprintf(f, ", \"queue\": %d, \"done\": %d, \"failed\": %d, \"last_incident\": %lld, \"last_t\": %lld, "
               "\"last_seconds\": %.0f, \"last_error\": ", T.qn, T.done, T.failed, (long long)T.last_id,
            (long long)T.last_t, T.last_seconds);
    jesc(f, T.last_error, sizeof(T.last_error));
    fputc('}', f);
    pthread_mutex_unlock(&T.lock);
}
