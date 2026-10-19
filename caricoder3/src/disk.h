/*
 * caricoder3 - the disks: how full, and what of ours is on them
 * Copyright (c) 2026 CariTech Solutions
 *
 * The filesystems that hold the paths the supervisor cares about (its
 * history, run and channels directories, the folders outputs write to,
 * and /), each once; how big a folder is; and folders that are ours to
 * keep within a size, oldest files going first (disk_trim).
 */
#ifndef CC3_DISK_H
#define CC3_DISK_H

#include <stdbool.h>
#include <stdint.h>

#define DISK_MAX 16

struct disk_fs {
    char mount[128];
    char dev[96];
    char fstype[24];
    uint64_t total, avail;      /* bytes; avail: what an ordinary user may still write */
    double used_pct;
    char paths[4][128];         /* which of the paths asked about it holds */
    int n_paths;
};

/* The filesystems holding `paths` (and /), one entry each; returns how many. */
int disk_list(const char *const *paths, int n_paths, struct disk_fs *out, int max);

/* Bytes in the files under `path` (not following links, at most
 * `max_files` looked at; -1 when it cannot be read). */
int64_t disk_du(const char *path, int max_files);

/* Deletes the oldest regular files under `path` (by modification time)
 * until what is there is at most `keep_bytes`. Returns the bytes freed. */
int64_t disk_trim(const char *path, int64_t keep_bytes);

#endif
