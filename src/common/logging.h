/*
 * CariTranscoder - Logging System
 * Copyright (c) 2024 CariTech Solutions
 */

#ifndef CARI_LOGGING_H
#define CARI_LOGGING_H

#include <stdarg.h>
#include <stdbool.h>
#include <syslog.h>

/* Log levels */
typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARNING,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_FATAL
} log_level_t;

/* Log output targets */
typedef enum {
    LOG_TARGET_NONE     = 0,
    LOG_TARGET_CONSOLE  = (1 << 0),
    LOG_TARGET_FILE     = (1 << 1),
    LOG_TARGET_SYSLOG   = (1 << 2),
    LOG_TARGET_ALL      = 0xFF
} log_target_t;

/* Log configuration */
typedef struct {
    log_level_t min_level;
    log_target_t targets;
    char log_file[256];
    char ident[64];             /* Syslog identifier */
    bool include_timestamp;
    bool include_level;
    bool include_file_line;
    bool colorize;              /* ANSI colors for console */
} log_config_t;

/* Default configuration */
#define LOG_CONFIG_DEFAULT { \
    .min_level = LOG_LEVEL_INFO, \
    .targets = LOG_TARGET_CONSOLE | LOG_TARGET_SYSLOG, \
    .log_file = "", \
    .ident = "caritrans", \
    .include_timestamp = true, \
    .include_level = true, \
    .include_file_line = false, \
    .colorize = true \
}

/**
 * Initialize logging system
 *
 * @param config Logging configuration (NULL for defaults)
 * @return 0 on success, -1 on error
 */
int log_init(const log_config_t *config);

/**
 * Shutdown logging system
 */
void log_shutdown(void);

/**
 * Set minimum log level
 *
 * @param level Minimum level to log
 */
void log_set_level(log_level_t level);

/**
 * Get current log level
 *
 * @return Current minimum log level
 */
log_level_t log_get_level(void);

/**
 * Set log level from string
 *
 * @param level_str Level string (debug, info, warning, error, fatal)
 * @return 0 on success, -1 if invalid
 */
int log_set_level_str(const char *level_str);

/**
 * Log a message
 *
 * @param level Log level
 * @param file Source file name
 * @param line Source line number
 * @param func Function name
 * @param fmt Format string
 * @param ... Format arguments
 */
void log_message(log_level_t level, const char *file, int line,
                 const char *func, const char *fmt, ...);

/**
 * Log a message (va_list version)
 */
void log_message_v(log_level_t level, const char *file, int line,
                   const char *func, const char *fmt, va_list args);

/* Convenience macros */
#define LOG_DEBUG(fmt, ...) \
    log_message(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define LOG_INFO(fmt, ...) \
    log_message(LOG_LEVEL_INFO, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define LOG_WARNING(fmt, ...) \
    log_message(LOG_LEVEL_WARNING, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define LOG_ERROR(fmt, ...) \
    log_message(LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define LOG_FATAL(fmt, ...) \
    log_message(LOG_LEVEL_FATAL, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

/* Conditional logging */
#define LOG_DEBUG_IF(cond, fmt, ...) \
    do { if (cond) LOG_DEBUG(fmt, ##__VA_ARGS__); } while(0)

/* Hex dump for debugging */
void log_hexdump(log_level_t level, const char *prefix,
                 const void *data, size_t len);

#endif /* CARI_LOGGING_H */
