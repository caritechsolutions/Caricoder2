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

#define ASKS_MAX 16

/* A question in plain words, and its answer. */
struct ask {
    int64_t id, t;
    char question[1200];
    int state;                  /* 0 waiting, 1 thinking, 2 answered, 3 failed */
    char *answer;
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
    struct ask asks[ASKS_MAX];
    int64_t next_ask;
    void (*context)(FILE *f, void *op);
    void *context_op;
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

/* Whether the model server was built for the CPU only (no CUDA compiler
 * when it was set up): then the cards are of no use to it. */
static bool server_cpu_only(void)
{
    const char *libs = getenv("CARICODER3_LIBS");
    char path[512], line[128] = "";
    FILE *f;

    snprintf(path, sizeof(path), "%s/llama-server.version", libs && libs[0] ? libs : "/opt/caricoder3-libs");
    if (!(f = fopen(path, "r")))
        return false;
    if (!fgets(line, sizeof(line), f))
        line[0] = 0;
    fclose(f);
    line[strcspn(line, "\r\n")] = 0;
    return strlen(line) >= 4 && !strcmp(line + strlen(line) - 4, " cpu");
}

/* A card with room: the most memory free, at least GPU_NEED_MB, not flat out. -1: none. */
static int pick_gpu(const char *want)
{
    int idx[8], free_mb[8], busy[8], n, best = -1;

    if (!strcmp(want, "cpu") || server_cpu_only())
        return -1;
    n = sysstat_gpu_free(idx, free_mb, busy, 8);
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

/* Makes `s` valid UTF-8 in place: a model can send a broken sequence, and a
 * cut ("%.300s") can split one; each bad byte becomes '?'. */
static void utf8_fix(char *s)
{
    unsigned char *p = (unsigned char *)s;

    while (*p) {
        int n = *p < 0x80 ? 0 : (*p & 0xE0) == 0xC0 && *p >= 0xC2 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 && *p <= 0xF4 ? 3 : -1;
        int k = 1;

        for (; n > 0 && k <= n; k++)
            if ((p[k] & 0xC0) != 0x80)
                break;
        if (n < 0 || k <= n) {
            *p++ = '?';
            continue;
        }
        p += n + 1;
    }
}

/* The first `"key": value` in a report (the incident's own fields come
 * before its detail): a number, or a string copied unescaped enough to
 * read. false: not there, or null. */
static bool rep_num(const char *rep, const char *key, long long *v)
{
    char k[48];
    const char *p;

    snprintf(k, sizeof(k), "\"%s\":", key);
    if (!(p = strstr(rep, k)))
        return false;
    p += strlen(k);
    while (*p == ' ')
        p++;
    if (!strncmp(p, "null", 4))
        return false;
    *v = strtoll(p, NULL, 10);
    return true;
}

static void rep_str(const char *rep, const char *key, char *out, size_t len)
{
    char k[48];
    const char *p;
    size_t n = 0;

    out[0] = 0;
    snprintf(k, sizeof(k), "\"%s\":", key);
    if (!(p = strstr(rep, k)))
        return;
    p += strlen(k);
    while (*p == ' ')
        p++;
    if (*p != '"')
        return;
    for (p++; *p && *p != '"' && n + 1 < len; p++) {
        if (*p == '\\' && p[1])
            p++;
        out[n++] = *p;
    }
    out[n] = 0;
}

/* The few facts that matter most, in plain words ahead of the JSON: a
 * small model reads these better than it finds them in a long report. */
static void key_facts(FILE *f, const char *rep, int64_t now)
{
    long long opened = 0, closed = 0;
    char rule[48], what[300], chan[80];
    bool has_closed;

    rep_num(rep, "opened", &opened);
    has_closed = rep_num(rep, "closed", &closed);
    rep_str(rep, "rule", rule, sizeof(rule));
    rep_str(rep, "summary", what, sizeof(what));
    rep_str(rep, "channel", chan, sizeof(chan));
    fprintf(f, "Key facts (from the record; do not add to them):\n- rule: %s\n- what: %s\n- where: %s\n", rule, what,
            chan[0] ? chan : "a source or the server (no single channel)");
    if (opened) {
        char tb[40];
        time_t tt = (time_t)opened;
        struct tm tm;

        localtime_r(&tt, &tm);
        strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", &tm);
        fprintf(f, "- began: %s (this server's local time)\n", tb);
    }
    if (has_closed)
        fprintf(f, "- it lasted %lld s and is OVER: it ended by itself\n", closed - opened);
    else if (opened)
        fprintf(f, "- it began %lld s ago and is STILL GOING ON\n", (long long)now - opened);
}

/* The request to the model, malloc'd; NULL when the incident is not there
 * (or belongs to a source's: that one is looked at instead, *parent set).
 * With `answer` (training data): the messages only, and that answer as the
 * assistant's. */
static char *build_request_as(const struct tech_conf *c, int64_t id, int64_t *parent, const char *answer)
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
                fprintf(uf, "\n\n");
                key_facts(uf, rep, time(NULL));
                fprintf(uf, "\nThe incident, with its evidence and the events around it (JSON; times are Unix "
                            "seconds):\n%.40000s\n\nEarlier incidents like it, with what you said then and what a "
                            "person said (JSON; null where nobody said):\n%.12000s\n\nWhat caused this incident? "
                            "Answer with the JSON object only.", rep, sim ? sim : "[]");
                fclose(uf);
                jesc(f, u ? u : "", ul);
            }
            free(u);
        }
        if (answer) {
            fprintf(f, "}, {\"role\": \"assistant\", \"content\": ");
            jesc(f, answer, strlen(answer));
            fprintf(f, "}]}");
        } else {
            snprintf(user, sizeof(user), "}], \"temperature\": 0.2, \"max_tokens\": 1500, \"stream\": false, ");
            fputs(user, f);
            fprintf(f, "\"response_format\": {\"type\": \"json_schema\", \"json_schema\": {\"name\": \"diagnosis\", "
                       "\"schema\": %s}}}", SCHEMA);
        }
        fclose(f);
    }
    free(sys);
    free(rep);
    free(sim);
    return req;
}

static char *build_request(const struct tech_conf *c, int64_t id, int64_t *parent)
{
    return build_request_as(c, id, parent, NULL);
}

/* ---- the thread ---------------------------------------------------------------------- */

static void requeue(int64_t id, int tries);

/* The server up (started if need be); false with the reason in T.last_error. */
static bool ensure_server(struct tech_conf *c)
{
    char err[300] = "";
    bool up;

    pthread_mutex_lock(&T.lock);
    up = T.server > 0;
    pthread_mutex_unlock(&T.lock);
    if (up && server_up())
        return true;
    pthread_mutex_lock(&T.lock);
    server_stop();
    pthread_mutex_unlock(&T.lock);
    if (server_start(c, err, sizeof(err)))
        return true;
    pthread_mutex_lock(&T.lock);
    set_state("unavailable");
    snprintf(T.last_error, sizeof(T.last_error), "%s", err);
    T.failed++;
    pthread_mutex_unlock(&T.lock);
    log_warn("technician: %s", err);
    return false;
}

/* A question: the notes, how things are now, the conversation so far. */
static void answer(int k)
{
    struct tech_conf c;
    char question[1200], *sys = NULL, *ctx = NULL, *req = NULL, *out = NULL, url[64];
    size_t sl = 0, cl = 0, ql = 0;
    FILE *f;
    int st;
    int64_t id;

    pthread_mutex_lock(&T.lock);
    c = T.c;
    snprintf(question, sizeof(question), "%s", T.asks[k].question);
    id = T.asks[k].id;
    T.asks[k].state = 1;
    pthread_mutex_unlock(&T.lock);
    if ((f = open_memstream(&sys, &sl))) {
        knowledge(f, &c);
        fprintf(f, "\n\n# Questions in plain words\n\nBetween incidents an engineer may ask you anything about "
                   "this transcoder in plain words. Answer from the state you are given and your notes, in plain "
                   "sentences, briefly (a few sentences; a short list when there are several things). Name the "
                   "channels, sources and figures you mean. When the state does not say, say so and say what "
                   "would tell. Do not answer in JSON.\n");
        fclose(f);
    }
    if ((f = open_memstream(&ctx, &cl))) {
        if (T.context)
            T.context(f, T.context_op);
        else
            fputs("{}", f);
        fclose(f);
        if (ctx)
            compact(ctx);
    }
    if ((f = open_memstream(&req, &ql))) {
        char *u = NULL;
        size_t ul = 0;
        FILE *uf = open_memstream(&u, &ul);

        if (uf) {
            fprintf(uf, "This transcoder is %s. How things are now (JSON; times are Unix seconds):\n%.40000s\n\n"
                        "The question: %s", c.my_name, ctx ? ctx : "{}", question);
            fclose(uf);
        }
        fprintf(f, "{\"messages\": [{\"role\": \"system\", \"content\": ");
        jesc(f, sys ? sys : "", 120000);
        fprintf(f, "}, {\"role\": \"user\", \"content\": ");
        jesc(f, u ? u : "", ul);
        fprintf(f, "}], \"temperature\": 0.3, \"max_tokens\": 700, \"stream\": false}");
        fclose(f);
        free(u);
    }
    free(sys);
    free(ctx);
    if (!req || !ensure_server(&c)) {
        pthread_mutex_lock(&T.lock);
        for (int i = 0; i < ASKS_MAX; i++)
            if (T.asks[i].id == id) {
                T.asks[i].state = 3;
                free(T.asks[i].answer);
                T.asks[i].answer = strdup(T.last_error[0] ? T.last_error : "the model could not be started");
            }
        pthread_mutex_unlock(&T.lock);
        free(req);
        return;
    }
    pthread_mutex_lock(&T.lock);
    set_state("thinking");
    pthread_mutex_unlock(&T.lock);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d", c.port);
    st = peer_http(url, "POST", "/v1/chat/completions", NULL, req, 600000, &out);
    free(req);
    {
        struct json_doc *doc = malloc(sizeof(*doc));
        char e2[200];
        const char *content = NULL;
        int root = doc && st == 200 && out ? json_parse(doc, out, e2, sizeof(e2)) : -1;

        if (root >= 0) {
            int ch = json_get(doc, root, "choices");

            if (ch >= 0 && doc->node[ch].first_child >= 0) {
                int msg = json_get(doc, doc->node[ch].first_child, "message");

                content = msg >= 0 ? json_get_str(doc, msg, "content", NULL) : NULL;
            }
        }
        if (content)
            utf8_fix((char *)content);
        pthread_mutex_lock(&T.lock);
        for (int i = 0; i < ASKS_MAX; i++)
            if (T.asks[i].id == id) {
                T.asks[i].state = content ? 2 : 3;
                free(T.asks[i].answer);
                T.asks[i].answer = strdup(content ? content : "no answer from the model");
            }
        T.last_used = time(NULL);
        set_state("idle");
        pthread_mutex_unlock(&T.lock);
        if (content)
            history_message(time(NULL), "technician", "out", content, "{}", 0);
        free(doc);
    }
    free(out);
}

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
    (void)up;
    if (!ensure_server(&c)) {
        free(req);
        return;
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
        if (content)
            utf8_fix((char *)content);
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
                utf8_fix(text);
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
        {
            int k = -1;

            for (int i = 0; i < ASKS_MAX; i++)
                if (T.asks[i].id && T.asks[i].state == 0 && (k < 0 || T.asks[i].id < T.asks[k].id))
                    k = i;
            if (k >= 0) {
                pthread_mutex_unlock(&T.lock);
                answer(k);
                continue;
            }
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

/* The answer it should have given, as a person said: its own when right;
 * theirs when it was something else. NULL: nothing to learn from. */
static char *target_answer(const char *diag, const char *verdict, const char *note, const char *actual)
{
    struct json_doc *a = malloc(sizeof(*a));
    char e[100], *out = NULL;
    size_t len = 0;
    FILE *f;
    int r = a && actual && *actual == '{' ? json_parse(a, actual, e, sizeof(e)) : -1;
    const char *cat = r >= 0 ? json_get_str(a, r, "category", "") : "", *cause = r >= 0 ? json_get_str(a, r, "cause", "") : "";

    if (!strcmp(verdict, "right") || (!strcmp(verdict, "partly") && !*cause && !*cat)) {
        free(a);
        return diag && *diag == '{' ? strdup(diag) : NULL;
    }
    if ((!*cause && !*cat) || !(f = open_memstream(&out, &len))) {
        free(a);
        return NULL;                /* wrong, but not said what it was */
    }
    fprintf(f, "{\"cause\": ");
    jesc(f, *cause ? cause : "", 300);
    fprintf(f, ", \"category\": ");
    jesc(f, *cat ? cat : "unknown", 40);
    fprintf(f, ", \"confidence\": 0.8, \"explanation\": ");
    jesc(f, note ? note : "", 1200);
    fprintf(f, ", \"next_checks\": [], \"ticket\": \"\"}");
    fclose(f);
    free(a);
    return out;
}

int tech_export(FILE *f)
{
    static int64_t ids[20000];
    int n = history_verdict_ids(ids, 20000), written = 0;
    struct tech_conf c;

    pthread_mutex_lock(&T.lock);
    c = T.c;
    pthread_mutex_unlock(&T.lock);
    for (int i = 0; i < n; i++) {
        char *d, *v, *no, *ac, *ans, *line;
        int64_t parent = 0;

        if (!history_incident_answer(ids[i], &d, &v, &no, &ac))
            continue;
        ans = target_answer(d, v, no, ac);
        line = ans ? build_request_as(&c, ids[i], &parent, ans) : NULL;
        if (line) {
            /* {"messages": [...]} with the incident and verdict alongside */
            fprintf(f, "{\"incident\": %lld, \"verdict\": \"%s\", %s\n", (long long)ids[i], v, line + 1);
            written++;
        }
        free(line);
        free(ans);
        free(d);
        free(v);
        free(no);
        free(ac);
    }
    return written;
}

void tech_set_context(void (*fn)(FILE *f, void *op), void *op)
{
    pthread_mutex_lock(&T.lock);
    T.context = fn;
    T.context_op = op;
    pthread_mutex_unlock(&T.lock);
}

int64_t tech_ask(const char *question)
{
    int k = 0;
    int64_t id;

    pthread_mutex_lock(&T.lock);
    if (!T.c.enabled || !T.running) {
        pthread_mutex_unlock(&T.lock);
        return 0;
    }
    for (int i = 1; i < ASKS_MAX; i++)
        if (T.asks[i].id < T.asks[k].id)
            k = i;                          /* the oldest gives way */
    free(T.asks[k].answer);
    memset(&T.asks[k], 0, sizeof(T.asks[k]));
    id = T.asks[k].id = ++T.next_ask;
    T.asks[k].t = time(NULL);
    snprintf(T.asks[k].question, sizeof(T.asks[k].question), "%s", question);
    pthread_cond_signal(&T.cond);
    pthread_mutex_unlock(&T.lock);
    history_message(time(NULL), "person", "in", question, "{}", 0);
    return id;
}

bool tech_answer_json(FILE *f, int64_t id)
{
    static const char *const states[] = { "waiting", "thinking", "answered", "failed" };
    bool found = false;

    pthread_mutex_lock(&T.lock);
    for (int i = 0; i < ASKS_MAX; i++)
        if (T.asks[i].id == id && id) {
            fprintf(f, "{\"id\": %lld, \"state\": \"%s\", \"question\": ", (long long)id, states[T.asks[i].state]);
            jesc(f, T.asks[i].question, sizeof(T.asks[i].question));
            fprintf(f, ", \"answer\": ");
            if (T.asks[i].answer)
                jesc(f, T.asks[i].answer, strlen(T.asks[i].answer));
            else
                fputs("null", f);
            fprintf(f, ", \"model_state\": \"%s\", \"where\": \"%s\"}", T.state, T.server > 0 ? T.where : "");
            found = true;
        }
    pthread_mutex_unlock(&T.lock);
    return found;
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
