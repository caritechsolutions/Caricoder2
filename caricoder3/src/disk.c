/*
 * caricoder3 - the disks
 * Copyright (c) 2026 CariTech Solutions
 */
#include "disk.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "log.h"

/* The mount point that holds `path`: the longest one it is under. */
static bool mount_of(const char *path, char *mnt, size_t mlen, char *dev, size_t dlen, char *type, size_t tlen)
{
    FILE *f = fopen("/proc/self/mounts", "r");
    char line[1024], real[PATH_MAX], best[256] = "";
    size_t best_len = 0;

    if (!f)
        return false;
    if (!realpath(path, real)) {
        /* not there (yet): the nearest folder above it that is */
        char tmp[PATH_MAX];
        char *slash;

        snprintf(tmp, sizeof(tmp), "%s", path);
        while ((slash = strrchr(tmp, '/')) && slash != tmp) {
            *slash = 0;
            if (realpath(tmp, real))
                break;
        }
        if (!slash || slash == tmp)
            snprintf(real, sizeof(real), "/");
    }
    while (fgets(line, sizeof(line), f)) {
        char d[256], m[256], t[64];
        size_t n;

        if (sscanf(line, "%255s %255s %63s", d, m, t) != 3)
            continue;
        n = strlen(m);
        if ((!strcmp(m, "/") || (!strncmp(real, m, n) && (real[n] == '/' || real[n] == 0))) && n >= best_len) {
            best_len = n;
            snprintf(best, sizeof(best), "%s", m);
            snprintf(dev, dlen, "%.95s", d);
            snprintf(type, tlen, "%.23s", t);
        }
    }
    fclose(f);
    if (!best_len)
        return false;
    snprintf(mnt, mlen, "%s", best);
    return true;
}

int disk_list(const char *const *paths, int n_paths, struct disk_fs *out, int max)
{
    int n = 0;

    for (int i = -1; i < n_paths; i++) {
        const char *p = i < 0 ? "/" : paths[i];
        char mnt[128], dev[96], type[24];
        struct statvfs sv;
        int k;

        if (!p || !*p || !mount_of(p, mnt, sizeof(mnt), dev, sizeof(dev), type, sizeof(type)))
            continue;
        for (k = 0; k < n; k++)
            if (!strcmp(out[k].mount, mnt))
                break;
        if (k == n) {
            if (n == max || statvfs(mnt, &sv) != 0)
                continue;
            memset(&out[n], 0, sizeof(out[n]));
            snprintf(out[n].mount, sizeof(out[n].mount), "%s", mnt);
            snprintf(out[n].dev, sizeof(out[n].dev), "%s", dev);
            snprintf(out[n].fstype, sizeof(out[n].fstype), "%s", type);
            out[n].total = (uint64_t)sv.f_blocks * sv.f_frsize;
            out[n].avail = (uint64_t)sv.f_bavail * sv.f_frsize;
            if (out[n].total)
                out[n].used_pct = 100.0 * (double)(out[n].total - (uint64_t)sv.f_bfree * sv.f_frsize) /
                                  (double)out[n].total;
            n++;
        }
        if (i >= 0 && out[k].n_paths < 4) {
            bool dup = false;

            for (int j = 0; j < out[k].n_paths; j++)
                dup |= !strcmp(out[k].paths[j], p);
            if (!dup)
                snprintf(out[k].paths[out[k].n_paths++], sizeof(out[k].paths[0]), "%s", p);
        }
    }
    return n;
}

static int64_t du(const char *path, int *left, int depth)
{
    DIR *d;
    struct dirent *e;
    int64_t sum = 0;
    struct stat sb;

    if (lstat(path, &sb) != 0)
        return -1;
    if (S_ISREG(sb.st_mode))
        return (int64_t)sb.st_blocks * 512;
    if (!S_ISDIR(sb.st_mode) || depth > 16 || !(d = opendir(path)))
        return 0;
    while ((e = readdir(d)) && *left > 0) {
        char sub[PATH_MAX];
        int64_t v;

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
        (*left)--;
        v = du(sub, left, depth + 1);
        if (v > 0)
            sum += v;
    }
    closedir(d);
    return sum;
}

int64_t disk_du(const char *path, int max_files)
{
    int left = max_files > 0 ? max_files : 100000;

    return du(path, &left, 0);
}

struct file_age {
    char path[PATH_MAX];
    time_t mtime;
    int64_t bytes;
};

static void collect(const char *path, struct file_age **v, int *n, int *cap, int depth)
{
    DIR *d = opendir(path);
    struct dirent *e;

    if (!d)
        return;
    while ((e = readdir(d))) {
        char sub[PATH_MAX];
        struct stat sb;

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
        if (lstat(sub, &sb) != 0)
            continue;
        if (S_ISDIR(sb.st_mode) && depth < 8) {
            collect(sub, v, n, cap, depth + 1);
        } else if (S_ISREG(sb.st_mode)) {
            if (*n == *cap) {
                int nc = *cap ? *cap * 2 : 256;
                struct file_age *nv = realloc(*v, sizeof(**v) * (size_t)nc);

                if (!nv)
                    break;
                *v = nv;
                *cap = nc;
            }
            snprintf((*v)[*n].path, sizeof((*v)[*n].path), "%s", sub);
            (*v)[*n].mtime = sb.st_mtime;
            (*v)[*n].bytes = (int64_t)sb.st_blocks * 512;
            (*n)++;
        }
    }
    closedir(d);
}

static int by_age(const void *a, const void *b)
{
    const struct file_age *x = a, *y = b;

    return x->mtime < y->mtime ? -1 : x->mtime > y->mtime;
}

int64_t disk_trim(const char *path, int64_t keep_bytes)
{
    struct file_age *v = NULL;
    int n = 0, cap = 0, gone = 0;
    int64_t total = 0, freed = 0;

    collect(path, &v, &n, &cap, 0);
    for (int i = 0; i < n; i++)
        total += v[i].bytes;
    if (total > keep_bytes) {
        qsort(v, (size_t)n, sizeof(*v), by_age);
        for (int i = 0; i < n && total - freed > keep_bytes; i++)
            if (unlink(v[i].path) == 0) {
                freed += v[i].bytes;
                gone++;
            }
        log_info("disk: %s was %.0f MB, over its %.0f MB; the %d oldest file%s went (%.0f MB)", path,
                 (double)total / 1048576, (double)keep_bytes / 1048576, gone, gone == 1 ? "" : "s",
                 (double)freed / 1048576);
    }
    free(v);
    return freed;
}
