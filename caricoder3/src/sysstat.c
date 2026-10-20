/*
 * caricoder3 - what the machine is doing: CPU, memory, GPUs, interfaces
 * Copyright (c) 2026 CariTech Solutions
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "sysstat.h"

#define IFACES_MAX 16
#define GPUS_MAX 8
#define SAMPLE_S 2
#define GPU_EVERY 5                     /* seconds */

struct iface {
    char name[32];
    uint64_t rx_bytes, tx_bytes, rx_pkts, tx_pkts, rx_err, tx_err, rx_drop, tx_drop;
    uint64_t rx_lost;                   /* missed + FIFO overruns: the card had no room (real loss) */
    double rx_bps, tx_bps;              /* since the previous sample */
    bool up;
    int speed_mbps;                     /* -1 unknown */
};

struct gpu {
    int index;
    char name[64];
    int util, enc, dec, mem_used, mem_total, temp, power;   /* -1 unknown */
};

struct sample {
    int64_t when_ns;
    uint64_t cpu_total, cpu_idle;       /* jiffies, all cores */
    double cpu_pct;                     /* since the previous sample */
    int cores;
    double load1, load5, load15;
    uint64_t mem_total, mem_available, swap_total, swap_free;   /* kB */
    struct iface ifs[IFACES_MAX];
    int n_ifs;
    struct gpu gpus[GPUS_MAX];
    int n_gpus;
    bool have_smi;
    char smi_error[80];
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct sample cur;
static pthread_t th;
static volatile bool running;

static int64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void read_cpu(struct sample *s)
{
    FILE *f = fopen("/proc/stat", "r");
    char line[512];

    s->cores = 0;
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "cpu ", 4)) {
            unsigned long long v[10] = { 0 };
            int n = sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                           &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);

            s->cpu_total = 0;
            for (int i = 0; i < n; i++)
                s->cpu_total += v[i];
            s->cpu_idle = v[3] + v[4];  /* idle + iowait */
        } else if (!strncmp(line, "cpu", 3)) {
            s->cores++;
        }
    }
    fclose(f);
    f = fopen("/proc/loadavg", "r");
    if (f) {
        if (fscanf(f, "%lf %lf %lf", &s->load1, &s->load5, &s->load15) != 3)
            s->load1 = s->load5 = s->load15 = 0;
        fclose(f);
    }
}

static void read_mem(struct sample *s)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256];

    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long v;

        if (sscanf(line, "MemTotal: %llu", &v) == 1) s->mem_total = v;
        else if (sscanf(line, "MemAvailable: %llu", &v) == 1) s->mem_available = v;
        else if (sscanf(line, "SwapTotal: %llu", &v) == 1) s->swap_total = v;
        else if (sscanf(line, "SwapFree: %llu", &v) == 1) s->swap_free = v;
    }
    fclose(f);
}

static void read_net(struct sample *s)
{
    FILE *f = fopen("/proc/net/dev", "r");
    char line[512];

    s->n_ifs = 0;
    if (!f)
        return;
    while (fgets(line, sizeof(line), f) && s->n_ifs < IFACES_MAX) {
        struct iface *i = &s->ifs[s->n_ifs];
        char *colon = strchr(line, ':');
        unsigned long long v[16];
        char *p = line;

        if (!colon)
            continue;
        *colon = 0;
        while (*p == ' ')
            p++;
        if (!strcmp(p, "lo") || !strncmp(p, "ifb", 3) || !strncmp(p, "veth", 4) ||
            !strncmp(p, "docker", 6) || !strncmp(p, "br-", 3))
            continue;
        snprintf(i->name, sizeof(i->name), "%.31s", p);
        if (sscanf(colon + 1, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
                   &v[8], &v[9], &v[10], &v[11]) < 12)
            continue;
        i->rx_bytes = v[0]; i->rx_pkts = v[1]; i->rx_err = v[2]; i->rx_drop = v[3];
        i->tx_bytes = v[8]; i->tx_pkts = v[9]; i->tx_err = v[10]; i->tx_drop = v[11];
        {
            char path[128], buf[32];
            FILE *g;

            i->up = false;
            i->speed_mbps = -1;
            snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", i->name);
            if ((g = fopen(path, "r"))) {
                if (fgets(buf, sizeof(buf), g))
                    i->up = !strncmp(buf, "up", 2) || !strncmp(buf, "unknown", 7);
                fclose(g);
            }
            i->rx_lost = 0;
            for (int k = 0; k < 2; k++) {
                snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/%s", i->name,
                         k ? "rx_fifo_errors" : "rx_missed_errors");
                if ((g = fopen(path, "r"))) {
                    if (fgets(buf, sizeof(buf), g))
                        i->rx_lost += strtoull(buf, NULL, 10);
                    fclose(g);
                }
            }
            snprintf(path, sizeof(path), "/sys/class/net/%s/speed", i->name);
            if ((g = fopen(path, "r"))) {
                if (fgets(buf, sizeof(buf), g))
                    i->speed_mbps = atoi(buf);
                fclose(g);
            }
        }
        s->n_ifs++;
    }
    fclose(f);
}

/* nvidia-smi, one line per card, with a 3 s limit on it. */
static void read_gpus(struct sample *s)
{
    static const char *const smi[] = { "/usr/bin/nvidia-smi", "/usr/local/bin/nvidia-smi", NULL };
    const char *exe = NULL;
    int pfd[2];
    pid_t pid;
    char out[4096];
    size_t len = 0;

    s->n_gpus = 0;
    for (int i = 0; smi[i]; i++)
        if (access(smi[i], X_OK) == 0) {
            exe = smi[i];
            break;
        }
    s->have_smi = exe != NULL;
    s->smi_error[0] = 0;
    if (!exe || pipe2(pfd, O_CLOEXEC) < 0)
        return;
    pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);

        dup2(pfd[1], 1);
        if (devnull >= 0) {
            dup2(devnull, 0);
            dup2(devnull, 2);
        }
        close(pfd[0]);
        close(pfd[1]);
        execl(exe, "nvidia-smi",
              "--query-gpu=index,name,utilization.gpu,utilization.encoder,utilization.decoder,"
              "memory.used,memory.total,temperature.gpu,power.draw",
              "--format=csv,noheader,nounits", (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    if (pid < 0) {
        close(pfd[0]);
        return;
    }
    {
        int64_t deadline = now_ns() + 3000000000LL;

        for (;;) {
            struct pollfd pf = { pfd[0], POLLIN, 0 };
            int64_t left = deadline - now_ns();
            ssize_t r;

            if (left <= 0 || poll(&pf, 1, (int)(left / 1000000)) <= 0)
                break;
            r = read(pfd[0], out + len, sizeof(out) - 1 - len);
            if (r <= 0)
                break;
            len += (size_t)r;
            if (len >= sizeof(out) - 1)
                break;
        }
    }
    close(pfd[0]);
    {
        int status = 0;
        int k;

        /* stdout closed is not quite exited: a moment is allowed */
        for (k = 0; k < 20 && waitpid(pid, &status, WNOHANG) == 0; k++)
            usleep(50000);
        if (k == 20) {
            kill(pid, SIGKILL);
            for (k = 0; k < 20 && waitpid(pid, &status, WNOHANG) == 0; k++)
                usleep(50000);          /* a process stuck in the driver may not die: not waited for */
            snprintf(s->smi_error, sizeof(s->smi_error), "nvidia-smi did not answer");
            return;
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            snprintf(s->smi_error, sizeof(s->smi_error), "nvidia-smi failed (no driver?)");
            return;
        }
    }
    out[len] = 0;
    for (char *line = strtok(out, "\n"); line && s->n_gpus < GPUS_MAX; line = strtok(NULL, "\n")) {
        struct gpu *g = &s->gpus[s->n_gpus];
        char *fields[9];
        int n = 0;
        char *p = line;

        /* comma-separated; the name may hold spaces but not commas */
        while (n < 9 && p) {
            char *c = strchr(p, ',');

            if (c)
                *c++ = 0;
            while (*p == ' ')
                p++;
            fields[n++] = p;
            p = c;
        }
        if (n < 9)
            continue;
        g->index = atoi(fields[0]);
        snprintf(g->name, sizeof(g->name), "%s", fields[1]);
        {
            size_t l = strlen(g->name);

            while (l && g->name[l - 1] == ' ')
                g->name[--l] = 0;
        }
#define NUM(i) (strstr(fields[i], "N/A") || strstr(fields[i], "Not") ? -1 : atoi(fields[i]))
        g->util = NUM(2);
        g->enc = NUM(3);
        g->dec = NUM(4);
        g->mem_used = NUM(5);
        g->mem_total = NUM(6);
        g->temp = NUM(7);
        g->power = NUM(8);
#undef NUM
        s->n_gpus++;
    }
}

static void *sampler(void *arg)
{
    struct sample prev;
    int since_gpu = GPU_EVERY;

    (void)arg;
    memset(&prev, 0, sizeof(prev));
    while (running) {
        struct sample s;

        memset(&s, 0, sizeof(s));
        s.when_ns = now_ns();
        read_cpu(&s);
        read_mem(&s);
        read_net(&s);
        if (prev.when_ns) {
            double dt = (double)(s.when_ns - prev.when_ns) / 1e9;
            uint64_t dtot = s.cpu_total - prev.cpu_total, didle = s.cpu_idle - prev.cpu_idle;

            s.cpu_pct = dtot ? 100.0 * (double)(dtot - didle) / (double)dtot : 0;
            for (int i = 0; i < s.n_ifs; i++)
                for (int k = 0; k < prev.n_ifs; k++)
                    if (!strcmp(s.ifs[i].name, prev.ifs[k].name) && dt > 0) {
                        s.ifs[i].rx_bps = (double)(s.ifs[i].rx_bytes - prev.ifs[k].rx_bytes) * 8.0 / dt;
                        s.ifs[i].tx_bps = (double)(s.ifs[i].tx_bytes - prev.ifs[k].tx_bytes) * 8.0 / dt;
                    }
        }
        if (since_gpu >= GPU_EVERY) {
            read_gpus(&s);
            since_gpu = 0;
        } else {
            s.n_gpus = prev.n_gpus;
            memcpy(s.gpus, prev.gpus, sizeof(s.gpus));
            s.have_smi = prev.have_smi;
            memcpy(s.smi_error, prev.smi_error, sizeof(s.smi_error));
        }
        since_gpu += SAMPLE_S;
        pthread_mutex_lock(&lock);
        cur = s;
        pthread_mutex_unlock(&lock);
        prev = s;
        for (int i = 0; i < SAMPLE_S * 10 && running; i++)
            usleep(100000);
    }
    return NULL;
}

void sysstat_start(void)
{
    if (running)
        return;
    running = true;
    if (pthread_create(&th, NULL, sampler, NULL) != 0)
        running = false;
}

void sysstat_stop(void)
{
    if (!running)
        return;
    running = false;
    pthread_join(th, NULL);
}

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

void sysstat_json(FILE *f)
{
    struct sample s;

    pthread_mutex_lock(&lock);
    s = cur;
    pthread_mutex_unlock(&lock);
    fprintf(f, "{\"sampled\": %s, \"cpu\": {\"percent\": %.1f, \"cores\": %d, \"load1\": %.2f, \"load5\": %.2f, "
               "\"load15\": %.2f}, \"memory\": {\"total_kb\": %llu, \"available_kb\": %llu, \"swap_total_kb\": %llu, "
               "\"swap_free_kb\": %llu}, \"gpus\": [",
            s.when_ns ? "true" : "false", s.cpu_pct, s.cores, s.load1, s.load5, s.load15,
            (unsigned long long)s.mem_total, (unsigned long long)s.mem_available,
            (unsigned long long)s.swap_total, (unsigned long long)s.swap_free);
    for (int i = 0; i < s.n_gpus; i++) {
        const struct gpu *g = &s.gpus[i];

        fprintf(f, "%s{\"index\": %d, \"name\": ", i ? ", " : "", g->index);
        jstr(f, g->name);
        fprintf(f, ", \"util\": %d, \"encoder\": %d, \"decoder\": %d, \"mem_used_mb\": %d, \"mem_total_mb\": %d, "
                   "\"temp_c\": %d, \"power_w\": %d}",
                g->util, g->enc, g->dec, g->mem_used, g->mem_total, g->temp, g->power);
    }
    fprintf(f, "], \"nvidia_smi\": %s, \"gpu_error\": ", s.have_smi ? "true" : "false");
    jstr(f, s.smi_error);
    fprintf(f, ", \"interfaces\": [");
    for (int i = 0; i < s.n_ifs; i++) {
        const struct iface *n = &s.ifs[i];

        fprintf(f, "%s{\"name\": ", i ? ", " : "");
        jstr(f, n->name);
        fprintf(f, ", \"up\": %s, \"speed_mbps\": %d, \"rx_bps\": %.0f, \"tx_bps\": %.0f, \"rx_packets\": %llu, "
                   "\"tx_packets\": %llu, \"rx_errors\": %llu, \"tx_errors\": %llu, \"rx_dropped\": %llu, "
                   "\"tx_dropped\": %llu}",
                n->up ? "true" : "false", n->speed_mbps, n->rx_bps, n->tx_bps,
                (unsigned long long)n->rx_pkts, (unsigned long long)n->tx_pkts,
                (unsigned long long)n->rx_err, (unsigned long long)n->tx_err,
                (unsigned long long)n->rx_drop, (unsigned long long)n->tx_drop);
    }
    fprintf(f, "]}");
}

int sysstat_gpu_free(int *index, int *free_mb, int *busy_pct, int max)
{
    int n = 0;

    pthread_mutex_lock(&lock);
    for (int i = 0; i < cur.n_gpus && n < max; i++) {
        const struct gpu *g = &cur.gpus[i];

        index[n] = g->index;
        free_mb[n] = g->mem_total > 0 && g->mem_used >= 0 ? g->mem_total - g->mem_used : -1;
        busy_pct[n] = g->util;
        n++;
    }
    pthread_mutex_unlock(&lock);
    return n;
}

double sysstat_cpu_percent(void)
{
    double v;

    pthread_mutex_lock(&lock);
    v = cur.when_ns ? cur.cpu_pct : -1;
    pthread_mutex_unlock(&lock);
    return v;
}

void sysstat_metrics(void (*cb)(const char *name, double v, void *op), void *op)
{
    static struct { char name[32]; uint64_t drop, err, lost; } prev[IFACES_MAX];
    static int n_prev;
    static uint64_t udp_prev;
    static bool udp_have;
    struct sample s;
    char name[96];

    pthread_mutex_lock(&lock);
    s = cur;
    pthread_mutex_unlock(&lock);
    if (!s.when_ns)
        return;
    cb("cpu.percent", s.cpu_pct, op);
    cb("load.1", s.load1, op);
    if (s.mem_total)
        cb("mem.used_pct", 100.0 * (double)(s.mem_total - s.mem_available) / (double)s.mem_total, op);
    for (int i = 0; i < s.n_gpus; i++) {
        const struct gpu *g = &s.gpus[i];
        const struct { const char *k; int v; } f[] = {
            { "util", g->util }, { "encoder", g->enc }, { "decoder", g->dec }, { "mem_mb", g->mem_used },
            { "temp_c", g->temp }, { "power_w", g->power },
        };

        for (size_t k = 0; k < sizeof(f) / sizeof(f[0]); k++)
            if (f[k].v >= 0) {
                snprintf(name, sizeof(name), "gpu%d.%s", g->index, f[k].k);
                cb(name, f[k].v, op);
            }
    }
    for (int i = 0; i < s.n_ifs; i++) {
        const struct iface *n = &s.ifs[i];
        int p = -1;

        if (!strcmp(n->name, "lo"))
            continue;
        snprintf(name, sizeof(name), "net.%s.rx_mbps", n->name);
        cb(name, n->rx_bps / 1e6, op);
        snprintf(name, sizeof(name), "net.%s.tx_mbps", n->name);
        cb(name, n->tx_bps / 1e6, op);
        for (int j = 0; j < n_prev; j++)
            if (!strcmp(prev[j].name, n->name))
                p = j;
        if (p < 0 && n_prev < IFACES_MAX) {
            p = n_prev++;
            snprintf(prev[p].name, sizeof(prev[p].name), "%s", n->name);
            prev[p].drop = n->rx_drop;
            prev[p].err = n->rx_err;
            prev[p].lost = n->rx_lost;
        }
        if (p >= 0) {
            snprintf(name, sizeof(name), "net.%s.rx_lost", n->name);
            cb(name, (double)(n->rx_lost >= prev[p].lost ? n->rx_lost - prev[p].lost : 0), op);
            prev[p].lost = n->rx_lost;
            snprintf(name, sizeof(name), "net.%s.rx_drop", n->name);
            cb(name, (double)(n->rx_drop >= prev[p].drop ? n->rx_drop - prev[p].drop : 0), op);
            snprintf(name, sizeof(name), "net.%s.rx_err", n->name);
            cb(name, (double)(n->rx_err >= prev[p].err ? n->rx_err - prev[p].err : 0), op);
            prev[p].drop = n->rx_drop;
            prev[p].err = n->rx_err;
        }
    }
    {
        /* UDP datagrams dropped because a socket's buffer was full: this
         * server did not read fast enough (/proc/net/snmp, RcvbufErrors) */
        FILE *f = fopen("/proc/net/snmp", "r");
        char hdr[1024], val[1024];
        uint64_t v = 0;
        bool got = false;

        while (f && fgets(hdr, sizeof(hdr), f)) {
            if (strncmp(hdr, "Udp: ", 5) || !fgets(val, sizeof(val), f))
                continue;
            {
                char *hs = NULL, *vs = NULL, *h = strtok_r(hdr, " \n", &hs), *x = strtok_r(val, " \n", &vs);

                for (; h && x; h = strtok_r(NULL, " \n", &hs), x = strtok_r(NULL, " \n", &vs))
                    if (!strcmp(h, "RcvbufErrors")) {
                        v = strtoull(x, NULL, 10);
                        got = true;
                    }
            }
            break;
        }
        if (f)
            fclose(f);
        if (got) {
            if (udp_have)
                cb("udp.rcvbuf_errors", (double)(v >= udp_prev ? v - udp_prev : 0), op);
            udp_prev = v;
            udp_have = true;
        }
    }
}
