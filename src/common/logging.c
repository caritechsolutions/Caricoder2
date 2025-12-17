/*
 * CariTranscoder - Logging System Implementation
 * Copyright (c) 2024 CariTech Solutions
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <stdint.h>
#include <syslog.h>

#include "logging.h"

/* ANSI color codes */
#define ANSI_RESET      "\033[0m"
#define ANSI_RED        "\033[31m"
#define ANSI_GREEN      "\033[32m"
#define ANSI_YELLOW     "\033[33m"
#define ANSI_BLUE       "\033[34m"
#define ANSI_MAGENTA    "\033[35m"
#define ANSI_CYAN       "\033[36m"
#define ANSI_BOLD       "\033[1m"

/* Global state */
static log_config_t g_config;
static FILE *g_log_file = NULL;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_initialized = false;

/* Level names */
static const char *level_names[] = {
    "DEBUG", "INFO", "WARNING", "ERROR", "FATAL"
};

static const char *level_colors[] = {
    ANSI_CYAN, ANSI_GREEN, ANSI_YELLOW, ANSI_RED, ANSI_BOLD ANSI_RED
};

/* Map our levels to syslog levels */
static int syslog_levels[] = {
    LOG_DEBUG, LOG_INFO, LOG_WARNING, LOG_ERR, LOG_CRIT
};

int log_init(const log_config_t *config) {
    pthread_mutex_lock(&g_log_mutex);

    if (config) {
        g_config = *config;
    } else {
        log_config_t default_config = LOG_CONFIG_DEFAULT;
        g_config = default_config;
    }

    /* Open log file if specified */
    if ((g_config.targets & LOG_TARGET_FILE) && g_config.log_file[0]) {
        g_log_file = fopen(g_config.log_file, "a");
        if (!g_log_file) {
            fprintf(stderr, "Failed to open log file: %s\n", g_config.log_file);
            g_config.targets &= ~LOG_TARGET_FILE;
        }
    }

    /* Initialize syslog if enabled */
    if (g_config.targets & LOG_TARGET_SYSLOG) {
        openlog(g_config.ident, LOG_PID | LOG_NDELAY, LOG_DAEMON);
    }

    /* Check if console supports colors */
    if (g_config.colorize && !isatty(STDERR_FILENO)) {
        g_config.colorize = false;
    }

    g_initialized = true;
    pthread_mutex_unlock(&g_log_mutex);

    return 0;
}

void log_shutdown(void) {
    pthread_mutex_lock(&g_log_mutex);

    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }

    if (g_config.targets & LOG_TARGET_SYSLOG) {
        closelog();
    }

    g_initialized = false;
    pthread_mutex_unlock(&g_log_mutex);
}

void log_set_level(log_level_t level) {
    g_config.min_level = level;
}

log_level_t log_get_level(void) {
    return g_config.min_level;
}

int log_set_level_str(const char *level_str) {
    if (!level_str) return -1;

    if (strcasecmp(level_str, "debug") == 0) {
        g_config.min_level = LOG_LEVEL_DEBUG;
    } else if (strcasecmp(level_str, "info") == 0) {
        g_config.min_level = LOG_LEVEL_INFO;
    } else if (strcasecmp(level_str, "warning") == 0 ||
               strcasecmp(level_str, "warn") == 0) {
        g_config.min_level = LOG_LEVEL_WARNING;
    } else if (strcasecmp(level_str, "error") == 0) {
        g_config.min_level = LOG_LEVEL_ERROR;
    } else if (strcasecmp(level_str, "fatal") == 0) {
        g_config.min_level = LOG_LEVEL_FATAL;
    } else {
        return -1;
    }

    return 0;
}

static void format_timestamp(char *buf, size_t len) {
    struct timeval tv;
    struct tm tm;

    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);

    snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             (int)(tv.tv_usec / 1000));
}

void log_message(log_level_t level, const char *file, int line,
                 const char *func, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_message_v(level, file, line, func, fmt, args);
    va_end(args);
}

void log_message_v(log_level_t level, const char *file, int line,
                   const char *func, const char *fmt, va_list args) {
    if (level < g_config.min_level) {
        return;
    }

    /* Auto-initialize if needed */
    if (!g_initialized) {
        log_init(NULL);
    }

    pthread_mutex_lock(&g_log_mutex);

    char timestamp[32] = "";
    char message[4096];
    char full_message[8192];

    /* Format timestamp */
    if (g_config.include_timestamp) {
        format_timestamp(timestamp, sizeof(timestamp));
    }

    /* Format message */
    vsnprintf(message, sizeof(message), fmt, args);

    /* Build full message */
    int offset = 0;

    if (g_config.include_timestamp) {
        offset += snprintf(full_message + offset, sizeof(full_message) - offset,
                          "%s ", timestamp);
    }

    if (g_config.include_level) {
        offset += snprintf(full_message + offset, sizeof(full_message) - offset,
                          "[%-7s] ", level_names[level]);
    }

    if (g_config.include_file_line && file) {
        /* Get just filename */
        const char *basename = strrchr(file, '/');
        basename = basename ? basename + 1 : file;
        offset += snprintf(full_message + offset, sizeof(full_message) - offset,
                          "%s:%d ", basename, line);
    }

    snprintf(full_message + offset, sizeof(full_message) - offset, "%s", message);

    /* Output to console */
    if (g_config.targets & LOG_TARGET_CONSOLE) {
        if (g_config.colorize) {
            fprintf(stderr, "%s%s%s\n", level_colors[level], full_message, ANSI_RESET);
        } else {
            fprintf(stderr, "%s\n", full_message);
        }
        fflush(stderr);
    }

    /* Output to file */
    if ((g_config.targets & LOG_TARGET_FILE) && g_log_file) {
        fprintf(g_log_file, "%s\n", full_message);
        fflush(g_log_file);
    }

    /* Output to syslog */
    if (g_config.targets & LOG_TARGET_SYSLOG) {
        syslog(syslog_levels[level], "%s", message);
    }

    pthread_mutex_unlock(&g_log_mutex);

    /* Exit on fatal */
    if (level == LOG_LEVEL_FATAL) {
        log_shutdown();
        exit(1);
    }
}

void log_hexdump(log_level_t level, const char *prefix,
                 const void *data, size_t len) {
    if (level < g_config.min_level) {
        return;
    }

    const uint8_t *p = (const uint8_t *)data;
    char line[128];
    char hex[64];
    char ascii[32];

    for (size_t i = 0; i < len; i += 16) {
        int hex_offset = 0;
        int ascii_offset = 0;

        for (size_t j = 0; j < 16 && (i + j) < len; j++) {
            hex_offset += snprintf(hex + hex_offset, sizeof(hex) - hex_offset,
                                   "%02x ", p[i + j]);
            ascii[ascii_offset++] = (p[i + j] >= 32 && p[i + j] < 127) ?
                                    p[i + j] : '.';
        }
        ascii[ascii_offset] = '\0';

        snprintf(line, sizeof(line), "%s %04zx: %-48s |%s|",
                 prefix ? prefix : "", i, hex, ascii);
        log_message(level, NULL, 0, NULL, "%s", line);
    }
}
