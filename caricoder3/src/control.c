/*
 * caricoder3 - the control socket
 * Copyright (c) 2026 CariTech Solutions
 */
#include "control.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "log.h"

#define LINE_MAX_BYTES  JSON_TEXT_MAX

struct control {
    int fd;
    char path[108];
    control_fn fn;
    void *opaque;
    pthread_t th;
    int stop_pipe[2];
};

/* Reads one line (the request) from a connected socket, with a limit on
 * size and time. */
static int read_line(int fd, char *buf, size_t len)
{
    size_t n = 0;

    while (n + 1 < len) {
        struct pollfd pf = { fd, POLLIN, 0 };
        ssize_t r;

        if (poll(&pf, 1, 5000) <= 0)
            return -1;
        r = read(fd, buf + n, 1);
        if (r <= 0)
            return n ? (int)n : -1;
        if (buf[n] == '\n')
            break;
        n++;
    }
    buf[n] = 0;
    return (int)n;
}

static void serve(struct control *c, int fd)
{
    char *line = malloc(LINE_MAX_BYTES);
    struct json_doc *doc = malloc(sizeof(*doc));
    char err[200];
    char *reply = NULL;
    size_t reply_len = 0;
    FILE *f;
    int root;

    if (!line || !doc || read_line(fd, line, LINE_MAX_BYTES) < 0) {
        free(line);
        free(doc);
        return;
    }
    f = open_memstream(&reply, &reply_len);
    if (!f) {
        free(line);
        free(doc);
        return;
    }
    root = json_parse(doc, line, err, sizeof(err));
    if (root < 0 || doc->node[root].type != JSON_OBJECT) {
        char q[400];

        fprintf(f, "{\"ok\": false, \"error\": %s}",
                json_quote(root < 0 ? err : "the request must be a JSON object", q, sizeof(q)));
    } else {
        c->fn(c->opaque, doc, f);
    }
    fputc('\n', f);
    fclose(f);
    free(line);
    free(doc);
    if (reply) {
        size_t off = 0;

        while (off < reply_len) {
            ssize_t w = write(fd, reply + off, reply_len - off);

            if (w <= 0)
                break;
            off += (size_t)w;
        }
        free(reply);
    }
}

static void *control_thread(void *arg)
{
    struct control *c = arg;

    for (;;) {
        struct pollfd pf[2] = { { c->fd, POLLIN, 0 }, { c->stop_pipe[0], POLLIN, 0 } };
        int fd;

        if (poll(pf, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pf[1].revents)
            break;
        fd = accept(c->fd, NULL, NULL);
        if (fd < 0)
            continue;
        serve(c, fd);
        close(fd);
    }
    return NULL;
}

struct control *control_start(const char *path, control_fn fn, void *opaque,
                              char *err, size_t errlen)
{
    struct control *c = calloc(1, sizeof(*c));
    struct sockaddr_un sa;

    if (!c) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    if (strlen(path) >= sizeof(sa.sun_path)) {
        snprintf(err, errlen, "the socket path is too long");
        free(c);
        return NULL;
    }
    c->fn = fn;
    c->opaque = opaque;
    snprintf(c->path, sizeof(c->path), "%s", path);
    c->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (c->fd < 0 || pipe(c->stop_pipe) < 0) {
        snprintf(err, errlen, "socket: %s", strerror(errno));
        free(c);
        return NULL;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, path);
    unlink(path);
    if (bind(c->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(c->fd, 8) < 0) {
        snprintf(err, errlen, "%s: %s", path, strerror(errno));
        close(c->fd);
        close(c->stop_pipe[0]);
        close(c->stop_pipe[1]);
        free(c);
        return NULL;
    }
    chmod(path, 0660);
    if (pthread_create(&c->th, NULL, control_thread, c) != 0) {
        snprintf(err, errlen, "cannot start the control thread");
        close(c->fd);
        unlink(path);
        close(c->stop_pipe[0]);
        close(c->stop_pipe[1]);
        free(c);
        return NULL;
    }
    return c;
}

void control_stop(struct control *c)
{
    if (!c)
        return;
    if (write(c->stop_pipe[1], "x", 1) < 0) { /* the thread then leaves */ }
    pthread_join(c->th, NULL);
    close(c->fd);
    unlink(c->path);
    close(c->stop_pipe[0]);
    close(c->stop_pipe[1]);
    free(c);
}

int control_client(const char *path, const char *request, FILE *out)
{
    struct sockaddr_un sa;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    char buf[4097];
    bool ok = false, any = false;
    ssize_t n;

    if (fd < 0 || strlen(path) >= sizeof(sa.sun_path)) {
        fprintf(stderr, "cannot make a socket\n");
        return 2;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "%s: %s (is the channel running with --control or --name?)\n",
                path, strerror(errno));
        close(fd);
        return 2;
    }
    if (write(fd, request, strlen(request)) < 0 || write(fd, "\n", 1) < 0) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        close(fd);
        return 2;
    }
    for (;;) {
        struct pollfd pf = { fd, POLLIN, 0 };

        if (poll(&pf, 1, 5000) <= 0)        /* a channel that does not answer in 5 s */
            break;
        n = read(fd, buf, sizeof(buf) - 1);
        if (n <= 0)
            break;
        buf[n] = 0;
        if (!any)
            ok = strstr(buf, "\"ok\": true") != NULL;
        any = true;
        fwrite(buf, 1, (size_t)n, out);
    }
    close(fd);
    return any ? (ok ? 0 : 1) : 2;
}
