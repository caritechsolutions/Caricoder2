/*
 * CariTranscoder - Output Application
 * Copyright (c) 2024 CariTech Solutions
 *
 * Reads from ring buffer and outputs to various destinations.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include "config.h"
#include "logging.h"
#include "ring_buffer.h"
#include "ts_packet.h"
#include "license.h"

/* Output state */
typedef struct {
    config_t config;
    char config_file[256];
    char id[64];
    char name[128];
    char output_type[32];       /* udp, srt, rtmp, hls, file */

    /* Input buffer */
    char input_buffer_name[128];
    ring_buffer_t *input_buffer;

    /* UDP output */
    int udp_socket;
    struct sockaddr_in udp_dest;

    /* GStreamer (for complex outputs) */
    GstElement *pipeline;
    GstElement *appsrc;
    GMainLoop *main_loop;

    /* Reader thread */
    pthread_t reader_thread;
    volatile int reader_running;

    /* Statistics */
    uint64_t packets_sent;
    uint64_t bytes_sent;

    /* State */
    volatile int running;
    license_info_t license;
} output_state_t;

static output_state_t g_state = {0};

static void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        CARI_LOG_INFO("Received signal %d, shutting down...", signum);
        g_state.running = 0;
        g_state.reader_running = 0;
        if (g_state.main_loop) {
            g_main_loop_quit(g_state.main_loop);
        }
    }
}

static int load_config(output_state_t *state) {
    if (config_load(&state->config, state->config_file) != 0) {
        return -1;
    }

    strncpy(state->id,
            config_get_string(&state->config, "output", "id", "output-001"),
            sizeof(state->id) - 1);

    strncpy(state->name,
            config_get_string(&state->config, "output", "name", "Unnamed Output"),
            sizeof(state->name) - 1);

    strncpy(state->output_type,
            config_get_string(&state->config, "output", "type", "udp"),
            sizeof(state->output_type) - 1);

    strncpy(state->input_buffer_name,
            config_get_string(&state->config, "input", "buffer_name", ""),
            sizeof(state->input_buffer_name) - 1);

    CARI_LOG_INFO("Configured: %s (%s) - Type: %s", state->name, state->id, state->output_type);

    return 0;
}

/* Initialize UDP output */
static int init_udp_output(output_state_t *state) {
    const char *address = config_get_string(&state->config, "destination", "address", "239.1.1.1");
    int port = config_get_int(&state->config, "destination", "port", 5000);
    int ttl = config_get_int(&state->config, "destination", "ttl", 64);

    state->udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (state->udp_socket < 0) {
        CARI_LOG_ERROR("Failed to create UDP socket: %s", strerror(errno));
        return -1;
    }

    /* Set TTL for multicast */
    if (IN_MULTICAST(ntohl(inet_addr(address)))) {
        unsigned char mc_ttl = ttl;
        setsockopt(state->udp_socket, IPPROTO_IP, IP_MULTICAST_TTL, &mc_ttl, sizeof(mc_ttl));
    }

    /* Set socket buffer size */
    int bufsize = config_get_int(&state->config, "destination", "buffer_size", 2097152);
    setsockopt(state->udp_socket, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));

    memset(&state->udp_dest, 0, sizeof(state->udp_dest));
    state->udp_dest.sin_family = AF_INET;
    state->udp_dest.sin_addr.s_addr = inet_addr(address);
    state->udp_dest.sin_port = htons(port);

    CARI_LOG_INFO("UDP output configured: %s:%d (TTL=%d)", address, port, ttl);

    return 0;
}

/* Send packets via UDP */
static int send_udp_packets(output_state_t *state, ts_packet_raw_t *packets, int count) {
    /* Send as 7 TS packets per UDP datagram (1316 bytes) */
    int sent = 0;
    int i = 0;

    while (i < count) {
        int batch = (count - i > 7) ? 7 : (count - i);
        ssize_t ret = sendto(state->udp_socket,
                              &packets[i],
                              batch * TS_PACKET_SIZE,
                              0,
                              (struct sockaddr *)&state->udp_dest,
                              sizeof(state->udp_dest));
        if (ret > 0) {
            sent += batch;
            state->bytes_sent += ret;
        }
        i += batch;
    }

    return sent;
}

/* Reader thread */
static void* reader_thread_func(void *arg) {
    output_state_t *state = (output_state_t *)arg;
    ts_packet_raw_t packets[7];

    CARI_LOG_DEBUG("Reader thread started");

    while (state->reader_running) {
        int count = ring_buffer_read_batch(state->input_buffer, packets, 7);

        if (count > 0) {
            if (strcmp(state->output_type, "udp") == 0) {
                int sent = send_udp_packets(state, packets, count);
                state->packets_sent += sent;
            }
            /* TODO: Add other output types */

            ring_buffer_heartbeat(state->input_buffer);
        } else {
            usleep(500); /* 0.5ms */
        }
    }

    CARI_LOG_DEBUG("Reader thread stopped");
    return NULL;
}

static void print_usage(const char *prog) {
    printf("CariTranscoder Output - v1.0.0\n");
    printf("Usage: %s -c <config_file> [options]\n", prog);
}

int main(int argc, char *argv[]) {
    int opt, debug = 0;
    const char *config_file = NULL;

    static struct option long_options[] = {
        {"config", required_argument, 0, 'c'},
        {"debug", no_argument, 0, 'd'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "c:dh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'c': config_file = optarg; break;
            case 'd': debug = 1; break;
            case 'h': print_usage(argv[0]); return 0;
            default: return 1;
        }
    }

    if (!config_file) {
        fprintf(stderr, "Configuration file required\n");
        return 1;
    }

    log_config_t log_cfg = LOG_CONFIG_DEFAULT;
    strncpy(log_cfg.ident, "cari-output", sizeof(log_cfg.ident));
    if (debug) log_cfg.min_level = LOG_LEVEL_DEBUG;
    log_init(&log_cfg);

    CARI_LOG_INFO("CariTranscoder Output starting...");

    strncpy(g_state.config_file, config_file, sizeof(g_state.config_file) - 1);
    if (load_config(&g_state) != 0) return 1;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    gst_init(&argc, &argv);

    /* Open input buffer */
    g_state.input_buffer = ring_buffer_open(g_state.input_buffer_name, NULL, false);
    if (!g_state.input_buffer) {
        CARI_LOG_ERROR("Failed to open input buffer: %s", g_state.input_buffer_name);
        return 1;
    }

    /* Initialize output based on type */
    if (strcmp(g_state.output_type, "udp") == 0) {
        if (init_udp_output(&g_state) != 0) {
            ring_buffer_close(g_state.input_buffer, false);
            return 1;
        }
    }
    /* TODO: Initialize other output types (SRT, RTMP, HLS, etc.) */

    /* Start reader thread */
    g_state.running = 1;
    g_state.reader_running = 1;
    pthread_create(&g_state.reader_thread, NULL, reader_thread_func, &g_state);

    CARI_LOG_INFO("Output %s started, sending to %s", g_state.id, g_state.output_type);

    /* Main loop - periodically log stats */
    while (g_state.running) {
        sleep(5);
        ts_stats_calculate_bitrate((ts_stream_stats_t *)NULL); /* Placeholder */
        CARI_LOG_INFO("Stats: %lu packets, %.2f Mbps",
                 g_state.packets_sent,
                 (g_state.bytes_sent * 8.0) / 5000000.0);
        g_state.bytes_sent = 0;
    }

    /* Cleanup */
    CARI_LOG_INFO("Shutting down...");

    g_state.reader_running = 0;
    pthread_join(g_state.reader_thread, NULL);

    if (g_state.udp_socket > 0) {
        close(g_state.udp_socket);
    }

    ring_buffer_close(g_state.input_buffer, false);
    config_free(&g_state.config);
    log_shutdown();

    return 0;
}
