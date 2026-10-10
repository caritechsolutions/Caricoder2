/*
 * caricoder3 - logging
 * Copyright (c) 2026 CariTech Solutions
 *
 * One line per message on stderr. Under systemd the journal adds its own
 * timestamp; the one printed here is for running by hand.
 */
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

int log_level = LOG_INFO;

void log_msg(int level, const char *fmt, ...)
{
    static const char *const names[] = { "ERROR", "WARN ", "INFO ", "DEBUG" };
    char line[1024];
    struct timespec ts;
    struct tm tm;
    va_list ap;

    if (level > log_level)
        return;
    if (level < LOG_ERR)
        level = LOG_ERR;
    if (level > LOG_DBG)
        level = LOG_DBG;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    /* Single fprintf so lines from different threads do not interleave. */
    fprintf(stderr, "%04d-%02d-%02d %02d:%02d:%02d.%03ld %s %s\n",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000,
            names[level], line);
}
