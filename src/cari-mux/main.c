/*
 * CariTranscoder - Mux Application
 * Copyright (c) 2024 CariTech Solutions
 *
 * Multiplexes multiple SPTS into MPTS or performs stream manipulation.
 * Integrates with TSDuck for advanced PSI/SI operations.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <pthread.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>

#include "config.h"
#include "logging.h"
#include "ring_buffer.h"
#include "ts_packet.h"
#include "license.h"

#define MAX_INPUTS 16

/* Input source */
typedef struct {
    char buffer_name[128];
    ring_buffer_t *buffer;
    int program_number;
    pthread_t thread;
    volatile int running;
} mux_input_t;

/* Mux state */
typedef struct {
    config_t config;
    char config_file[256];
    char id[64];
    char name[128];
    char mode[32];              /* mpts, spts, remux */

    /* Inputs */
    mux_input_t inputs[MAX_INPUTS];
    int input_count;

    /* Output */
    char output_buffer_name[128];
    ring_buffer_t *output_buffer;

    /* GStreamer */
    GstElement *pipeline;
    GstElement *muxer;
    GstElement *appsink;
    GMainLoop *main_loop;

    /* TSDuck settings */
    int tsduck_enabled;
    int regulate_bitrate;
    int target_bitrate;
    int pcr_restamp;

    /* Statistics */
    uint64_t packets_in;
    uint64_t packets_out;

    /* State */
    volatile int running;
    license_info_t license;
} mux_state_t;

static mux_state_t g_state = {0};

static void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        LOG_INFO("Received signal %d, shutting down...", signum);
        g_state.running = 0;
        for (int i = 0; i < g_state.input_count; i++) {
            g_state.inputs[i].running = 0;
        }
        if (g_state.main_loop) {
            g_main_loop_quit(g_state.main_loop);
        }
    }
}

static int load_config(mux_state_t *state) {
    if (config_load(&state->config, state->config_file) != 0) {
        return -1;
    }

    strncpy(state->id,
            config_get_string(&state->config, "muxer", "id", "mux-001"),
            sizeof(state->id) - 1);

    strncpy(state->name,
            config_get_string(&state->config, "muxer", "name", "Unnamed Mux"),
            sizeof(state->name) - 1);

    strncpy(state->mode,
            config_get_string(&state->config, "muxer", "mode", "mpts"),
            sizeof(state->mode) - 1);

    /* Output buffer */
    strncpy(state->output_buffer_name,
            config_get_string(&state->config, "output", "buffer_name", state->id),
            sizeof(state->output_buffer_name) - 1);

    /* TSDuck settings */
    state->tsduck_enabled = config_get_bool(&state->config, "tsduck", "enabled", false);
    state->regulate_bitrate = config_get_bool(&state->config, "tsduck", "regulate_bitrate", false);
    state->target_bitrate = config_get_int(&state->config, "tsduck", "target_bitrate", 0);
    state->pcr_restamp = config_get_bool(&state->config, "tsduck", "pcr_restamp", true);

    /* Parse input sources from programs section */
    state->input_count = 0;
    for (int i = 1; i <= MAX_INPUTS; i++) {
        char key[64];
        snprintf(key, sizeof(key), "program.%d.enabled", i);
        if (!config_get_bool(&state->config, "programs", key, false)) {
            continue;
        }

        mux_input_t *input = &state->inputs[state->input_count];

        snprintf(key, sizeof(key), "program.%d.source", i);
        strncpy(input->buffer_name,
                config_get_string(&state->config, "programs", key, ""),
                sizeof(input->buffer_name) - 1);

        snprintf(key, sizeof(key), "program.%d.number", i);
        input->program_number = config_get_int(&state->config, "programs", key, i);

        if (input->buffer_name[0]) {
            state->input_count++;
            LOG_INFO("Input %d: %s (program %d)", state->input_count,
                     input->buffer_name, input->program_number);
        }
    }

    LOG_INFO("Configured: %s (%s) - Mode: %s, Inputs: %d",
             state->name, state->id, state->mode, state->input_count);

    return 0;
}

/* Input reader thread */
static void* input_reader_func(void *arg) {
    mux_input_t *input = (mux_input_t *)arg;
    ts_packet_raw_t packets[7];

    while (input->running) {
        int count = ring_buffer_read_batch(input->buffer, packets, 7);
        if (count > 0) {
            /* TODO: Push to muxer pipeline */
            ring_buffer_heartbeat(input->buffer);
            g_state.packets_in += count;
        } else {
            usleep(1000);
        }
    }

    return NULL;
}

/* Output callback */
static GstFlowReturn on_new_sample(GstAppSink *appsink, gpointer user_data) {
    mux_state_t *state = (mux_state_t *)user_data;
    GstSample *sample = gst_app_sink_pull_sample(appsink);
    if (!sample) return GST_FLOW_ERROR;

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;

    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        size_t offset = 0;
        while (offset + TS_PACKET_SIZE <= map.size) {
            ts_packet_raw_t *packet = (ts_packet_raw_t *)(map.data + offset);
            if (ts_packet_valid(packet)) {
                ring_buffer_write(state->output_buffer, packet);
                state->packets_out++;
            }
            offset += TS_PACKET_SIZE;
        }
        ring_buffer_heartbeat(state->output_buffer);
        gst_buffer_unmap(buffer, &map);
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static void print_usage(const char *prog) {
    printf("CariTranscoder Mux - v1.0.0\n");
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
    strncpy(log_cfg.ident, "cari-mux", sizeof(log_cfg.ident));
    if (debug) log_cfg.min_level = LOG_LEVEL_DEBUG;
    log_init(&log_cfg);

    LOG_INFO("CariTranscoder Mux starting...");

    strncpy(g_state.config_file, config_file, sizeof(g_state.config_file) - 1);
    if (load_config(&g_state) != 0) return 1;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    gst_init(&argc, &argv);

    /* Open input buffers */
    for (int i = 0; i < g_state.input_count; i++) {
        g_state.inputs[i].buffer = ring_buffer_open(g_state.inputs[i].buffer_name, NULL, false);
        if (!g_state.inputs[i].buffer) {
            LOG_ERROR("Failed to open input buffer: %s", g_state.inputs[i].buffer_name);
            return 1;
        }
    }

    /* Create output buffer */
    ring_buffer_options_t rb_opts = RING_BUFFER_OPTIONS_DEFAULT;
    g_state.output_buffer = ring_buffer_open(g_state.output_buffer_name, &rb_opts, true);
    if (!g_state.output_buffer) {
        LOG_ERROR("Failed to create output buffer");
        return 1;
    }

    /* Start input reader threads */
    g_state.running = 1;
    for (int i = 0; i < g_state.input_count; i++) {
        g_state.inputs[i].running = 1;
        pthread_create(&g_state.inputs[i].thread, NULL, input_reader_func, &g_state.inputs[i]);
    }

    LOG_INFO("Mux %s started with %d inputs", g_state.id, g_state.input_count);

    /* Simple loop - in real implementation, use GStreamer pipeline */
    while (g_state.running) {
        sleep(1);
        LOG_DEBUG("Stats: %lu in, %lu out", g_state.packets_in, g_state.packets_out);
    }

    /* Cleanup */
    LOG_INFO("Shutting down...");

    for (int i = 0; i < g_state.input_count; i++) {
        g_state.inputs[i].running = 0;
        pthread_join(g_state.inputs[i].thread, NULL);
        ring_buffer_close(g_state.inputs[i].buffer, false);
    }

    ring_buffer_close(g_state.output_buffer, true);
    config_free(&g_state.config);
    log_shutdown();

    return 0;
}
