/*
 * caricoder3 listen - which language a source or a channel speaks
 * Copyright (c) 2026 CariTech Solutions
 *
 *   caricoder3 listen URL [--seconds 20] [--program N] [--pid P] [--save FILE.wav]
 *
 * Reads URL (as the probe does: a channel's output, a source) for a while,
 * takes one audio stream out of it (--pid, or the program's first), and
 * asks Whisper (whisper.cpp, offline: scripts/whisper-setup.sh) which
 * language is spoken in it. Prints JSON:
 *
 *   {"ok": true, "language": "spa", "whisper": "es", "probability": 0.93,
 *    "mean_volume_db": -24.1, "seconds": 20, "pid": 2162}
 *
 * The language as the PMT writes it (ISO 639-2: eng, spa, ...), to compare
 * with what a channel should carry. A clip with no sound ("mean_volume_db"
 * near -91) or music says little: "probability" is then low, and the
 * answer is to be taken as "not sure". --save keeps the clip (16 kHz mono
 * WAV) for a person to listen to.
 *
 * The parts: this program's own probe (--save) for the clip, ffmpeg for the
 * sound, caricoder3-whisper-cli and its model; CC3_WHISPER_CLI,
 * CC3_WHISPER_MODEL and CC3_FFMPEG name others.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WHISPER_CLI "/usr/local/bin/caricoder3-whisper-cli"
#define WHISPER_MODEL "/var/lib/caricoder3/models/whisper/ggml-base.bin"

static const struct { const char *w, *iso; } LANGS[] = {
    { "en", "eng" }, { "es", "spa" }, { "fr", "fra" }, { "pt", "por" }, { "de", "deu" }, { "it", "ita" },
    { "nl", "nld" }, { "ru", "rus" }, { "zh", "zho" }, { "ja", "jpn" }, { "ko", "kor" }, { "ar", "ara" },
    { "hi", "hin" }, { "pl", "pol" }, { "tr", "tur" }, { "sv", "swe" }, { "he", "heb" }, { "ht", "hat" },
    { NULL, NULL }
};

static const char *iso_of(const char *w)
{
    for (int i = 0; LANGS[i].w; i++)
        if (!strcmp(LANGS[i].w, w))
            return LANGS[i].iso;
    return w;
}

static void jstr(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            printf("\\%c", *s);
        else if ((unsigned char)*s < 32)
            printf("\\u%04x", *s);
        else
            putchar(*s);
    }
    putchar('"');
}

static int fail(const char *msg, const char *arg)
{
    char m[600];

    snprintf(m, sizeof(m), msg, arg ? arg : "");
    printf("{\"ok\": false, \"error\": ");
    jstr(m);
    printf("}\n");
    return 1;
}

/* Runs argv, its standard error (and output) into `out` (malloc'd); the
 * exit status, -1 if it could not run, 124 when it took longer than
 * `timeout_s`. */
static int run(char *const argv[], char **out, int timeout_s)
{
    int p[2], status = -1;
    pid_t pid;
    size_t len = 0, cap = 0;
    char *buf = NULL;
    time_t end = time(NULL) + timeout_s;

    *out = NULL;
    if (pipe(p) < 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(p[1], 1);
        dup2(p[1], 2);
        close(p[0]);
        close(p[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(p[1]);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    for (;;) {
        char tmp[4096];
        ssize_t r = read(p[0], tmp, sizeof(tmp));

        if (r > 0) {
            if (len + (size_t)r + 1 > cap) {
                size_t nc = cap ? cap * 2 : 8192;
                char *nb;

                while (nc < len + (size_t)r + 1)
                    nc *= 2;
                if (nc > (4u << 20) || !(nb = realloc(buf, nc)))
                    continue;           /* enough of it: the rest is dropped */
                buf = nb;
                cap = nc;
            }
            memcpy(buf + len, tmp, (size_t)r);
            len += (size_t)r;
            continue;
        }
        if (r == 0)
            break;
        if (errno != EAGAIN && errno != EINTR)
            break;
        if (time(NULL) > end) {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            close(p[0]);
            if (buf)
                buf[len] = 0;
            *out = buf;
            return 124;
        }
        usleep(50000);
    }
    close(p[0]);
    waitpid(pid, &status, 0);
    if (buf)
        buf[len] = 0;
    *out = buf;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void cleanup(const char *dir)
{
    char p[600];

    snprintf(p, sizeof(p), "%s/clip.ts", dir);
    unlink(p);
    snprintf(p, sizeof(p), "%s/clip.wav", dir);
    unlink(p);
    rmdir(dir);
}

static void usage(FILE *f)
{
    fprintf(f, "Usage: caricoder3 listen URL [--seconds 20] [--program N] [--pid P] [--save FILE.wav]\n"
               "  Which language is spoken in an audio stream of URL (a channel's output, a source):\n"
               "  Whisper's answer as JSON. --pid: that stream (default: the program's first audio);\n"
               "  --save: keep the clip (16 kHz mono WAV).\n");
}

int listen_main(int argc, char **argv);
int listen_main(int argc, char **argv)
{
    const char *url = NULL, *save = NULL;
    const char *cli = getenv("CC3_WHISPER_CLI"), *model = getenv("CC3_WHISPER_MODEL"), *ffmpeg = getenv("CC3_FFMPEG");
    int seconds = 20, program = 0, pid = 0, rc;
    char dir[] = "/tmp/caricoder3-listen-XXXXXX", ts[600], wav[600], self[600], secs[16], prog[16], map[32],
         threads[16];
    char *out = NULL;
    double vol = NAN, p = NAN;
    char lang[16] = "";

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--program") && i + 1 < argc)
            program = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pid") && i + 1 < argc)
            pid = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--save") && i + 1 < argc)
            save = argv[++i];
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
    if (seconds < 5 || seconds > 60)
        return fail("--seconds must be 5..60", NULL);
    if (!cli)
        cli = WHISPER_CLI;
    if (!model)
        model = WHISPER_MODEL;
    if (!ffmpeg)
        ffmpeg = "ffmpeg";
    if (access(cli, X_OK) || access(model, R_OK))
        return fail("Whisper is not installed on this server (scripts/whisper-setup.sh): %s", access(cli, X_OK) ? cli : model);
    if (!mkdtemp(dir))
        return fail("a folder for the clip: %s", strerror(errno));
    snprintf(ts, sizeof(ts), "%s/clip.ts", dir);
    snprintf(wav, sizeof(wav), "%s/clip.wav", dir);
    {
        ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);

        if (n <= 0) {
            cleanup(dir);
            return fail("cannot find this program", NULL);
        }
        self[n] = 0;
    }

    /* 1. the clip, as the probe reads (any URL it can) */
    snprintf(secs, sizeof(secs), "%d", seconds);
    snprintf(prog, sizeof(prog), "%d", program);
    {
        char *a[] = { self, "probe", (char *)url, "--seconds", secs, "--save", ts, program ? "--program" : NULL, prog, NULL };

        rc = run(a, &out, seconds + 30);
        if (rc != 0 || !out || !strstr(out, "\"ok\": true")) {
            char m[300];
            const char *e = out ? strstr(out, "\"error\": \"") : NULL;

            snprintf(m, sizeof(m), "%.*s", e ? (int)strcspn(e + 10, "\"") : 40, e ? e + 10 : "the clip could not be taken");
            free(out);
            cleanup(dir);
            return fail("%s", m);
        }
        free(out);
    }

    /* 2. its sound: 16 kHz mono, and how loud */
    if (pid)
        snprintf(map, sizeof(map), "0:i:%d", pid);
    else
        snprintf(map, sizeof(map), "0:a:0");
    {
        char *a[] = { (char *)ffmpeg, "-hide_banner", "-nostdin", "-y", "-i", ts, "-map", map, "-ac", "1", "-ar", "16000",
                      "-af", "volumedetect", "-f", "wav", wav, NULL };
        const char *m;

        rc = run(a, &out, 60);
        if (rc != 0 || access(wav, R_OK)) {
            char e[300];

            snprintf(e, sizeof(e), "%s", rc == 127 ? "ffmpeg is not installed" :
                     pid ? "no sound could be taken from that PID" : "no sound could be taken from it");
            free(out);
            cleanup(dir);
            return fail("%s", e);
        }
        if (out && (m = strstr(out, "mean_volume:")))
            vol = atof(m + 12);
        free(out);
    }

    /* 3. Whisper: which language */
    {
        long nproc = sysconf(_SC_NPROCESSORS_ONLN);
        char *a[] = { (char *)cli, "-m", (char *)model, "-f", wav, "-l", "auto", "-dl", "-t", threads, NULL };
        const char *m;

        snprintf(threads, sizeof(threads), "%ld", nproc > 16 ? 8 : nproc > 2 ? nproc / 2 : 1);
        rc = run(a, &out, 120);
        if (out && (m = strstr(out, "auto-detected language:"))) {
            const char *q = strstr(m, "p = ");

            sscanf(m + 23, " %15[a-z]", lang);
            if (q)
                p = atof(q + 4);
        }
        if (!lang[0]) {
            char e[300];

            snprintf(e, sizeof(e), "Whisper gave no answer (exit %d)", rc);
            free(out);
            cleanup(dir);
            return fail("%s", e);
        }
        free(out);
    }
    if (save) {
        char *a[] = { "cp", wav, (char *)save, NULL };

        if (run(a, &out, 30) != 0)
            save = NULL;
        free(out);
    }
    cleanup(dir);
    printf("{\"ok\": true, \"url\": ");
    jstr(url);
    printf(", \"seconds\": %d, \"pid\": %d, \"language\": ", seconds, pid);
    jstr(iso_of(lang));
    printf(", \"whisper\": ");
    jstr(lang);
    printf(", \"probability\": %.3f, \"mean_volume_db\": ", isnan(p) ? 0.0 : p);
    if (isnan(vol))
        printf("null");
    else
        printf("%.1f", vol);
    if (save) {
        printf(", \"clip\": ");
        jstr(save);
    }
    printf("}\n");
    return 0;
}
