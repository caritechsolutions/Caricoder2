/*
 * CariTranscoder - Statistics Daemon
 * Copyright (c) 2024 CariTech Solutions
 *
 * Collects statistics from all running services and provides
 * WebSocket interface for real-time dashboard updates.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "config.h"
#include "logging.h"

static volatile int g_running = 1;

static void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        g_running = 0;
    }
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    log_config_t log_cfg = LOG_CONFIG_DEFAULT;
    strncpy(log_cfg.ident, "cari-stats", sizeof(log_cfg.ident));
    log_init(&log_cfg);

    LOG_INFO("CariTranscoder Stats Daemon starting...");

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* TODO: Implement statistics collection and WebSocket server */

    while (g_running) {
        sleep(1);
    }

    LOG_INFO("Stats daemon shutting down...");
    log_shutdown();

    return 0;
}
