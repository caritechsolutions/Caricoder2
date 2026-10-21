/*
 * caricoder3 - logging
 * Copyright (c) 2026 CariTech Solutions
 */
#ifndef CC3_LOG_H
#define CC3_LOG_H

enum { LOG_ERR = 0, LOG_WARN = 1, LOG_INFO = 2, LOG_DBG = 3 };

extern int log_level;

void log_thread_prefix(const char *p);
void log_msg(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define log_err(...)  log_msg(LOG_ERR, __VA_ARGS__)
#define log_warn(...) log_msg(LOG_WARN, __VA_ARGS__)
#define log_info(...) log_msg(LOG_INFO, __VA_ARGS__)
#define log_dbg(...)  do { if (log_level >= LOG_DBG) log_msg(LOG_DBG, __VA_ARGS__); } while (0)

#endif
