/*
 * CariTranscoder - Input Application
 * Copyright (c) 2024 CariTech Solutions
 *
 * Handles various input sources and outputs to shared memory ring buffer.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include "config.h"
#include "logging.h"
#include "ring_buffer.h"
#include "ts_packet.h"
#include "license.h"

/* Application state */
typedef struct {
    /* Configuration */
    config_t config;
    char config_file[256];
    char id[64];
    char name[128];

    /* Input settings */
    char input_type[32];
    char source_uri[512];

    /* Output buffer */
    char buffer_name[128];
    ring_buffer_t *output_buffer;

    /* GStreamer */
    GstElement *pipeline;
    GstElement *appsink;
    GMainLoop *main_loop;

    /* Statistics */
    ts_stream_stats_t stats;
    uint64_t bytes_received;
    uint64_t packets_sent;

    /* State */
    volatile int running;
    volatile int reload_config;

    /* License */
    license_info_t license;
} app_state_t;

static app_state_t g_state = {0};

/* Signal handlers */
static void signal_handler(int signum) {
    switch (signum) {
        case SIGINT:
        case SIGTERM:
            LOG_INFO("Received signal %d, shutting down...", signum);
            g_state.running = 0;
            if (g_state.main_loop) {
                g_main_loop_quit(g_state.main_loop);
            }
            break;
        case SIGHUP:
            LOG_INFO("Received SIGHUP, reloading configuration...");
            g_state.reload_config = 1;
            break;
    }
}

static void setup_signals(void) {
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    /* Ignore SIGPIPE */
    signal(SIGPIPE, SIG_IGN);
}

/* Load configuration */
static int load_config(app_state_t *state) {
    if (config_load(&state->config, state->config_file) != 0) {
        LOG_ERROR("Failed to load configuration from %s", state->config_file);
        return -1;
    }

    /* Parse input settings */
    strncpy(state->id,
            config_get_string(&state->config, "input", "id", "input-001"),
            sizeof(state->id) - 1);

    strncpy(state->name,
            config_get_string(&state->config, "input", "name", "Unnamed Input"),
            sizeof(state->name) - 1);

    strncpy(state->input_type,
            config_get_string(&state->config, "input", "type", "udp"),
            sizeof(state->input_type) - 1);

    strncpy(state->buffer_name,
            config_get_string(&state->config, "output", "buffer_name", state->id),
            sizeof(state->buffer_name) - 1);

    /* Build source URI based on type */
    if (strcmp(state->input_type, "srt") == 0) {
        const char *mode = config_get_string(&state->config, "source", "mode", "listener");
        if (strcmp(mode, "listener") == 0) {
            snprintf(state->source_uri, sizeof(state->source_uri),
                     "srt://0.0.0.0:%s?mode=listener",
                     config_get_string(&state->config, "source", "listen_port", "4900"));
        } else {
            snprintf(state->source_uri, sizeof(state->source_uri),
                     "srt://%s:%s?mode=caller",
                     config_get_string(&state->config, "source", "remote_address", "127.0.0.1"),
                     config_get_string(&state->config, "source", "remote_port", "4900"));
        }
    } else if (strcmp(state->input_type, "udp") == 0) {
        snprintf(state->source_uri, sizeof(state->source_uri),
                 "udp://%s:%s",
                 config_get_string(&state->config, "source", "address", "0.0.0.0"),
                 config_get_string(&state->config, "source", "port", "5000"));
    } else if (strcmp(state->input_type, "rtmp") == 0) {
        strncpy(state->source_uri,
                config_get_string(&state->config, "source", "url", ""),
                sizeof(state->source_uri) - 1);
    } else if (strcmp(state->input_type, "file") == 0) {
        snprintf(state->source_uri, sizeof(state->source_uri),
                 "file://%s",
                 config_get_string(&state->config, "source", "path", ""));
    }

    LOG_INFO("Configured: %s (%s) - Type: %s, Source: %s",
             state->name, state->id, state->input_type, state->source_uri);

    return 0;
}

/* GStreamer callback for new samples */
static GstFlowReturn on_new_sample(GstAppSink *appsink, gpointer user_data) {
    app_state_t *state = (app_state_t *)user_data;
    GstSample *sample;
    GstBuffer *buffer;
    GstMapInfo map;

    sample = gst_app_sink_pull_sample(appsink);
    if (!sample) {
        return GST_FLOW_ERROR;
    }

    buffer = gst_sample_get_buffer(sample);
    if (!buffer) {
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        /* Process TS packets */
        size_t offset = 0;
        while (offset + TS_PACKET_SIZE <= map.size) {
            ts_packet_raw_t *packet = (ts_packet_raw_t *)(map.data + offset);

            /* Validate sync byte */
            if (ts_packet_valid(packet)) {
                /* Update statistics */
                ts_stats_update(&state->stats, packet);

                /* Write to output buffer */
                if (state->output_buffer) {
                    if (ring_buffer_write(state->output_buffer, packet) == 0) {
                        state->packets_sent++;
                    }
                    ring_buffer_heartbeat(state->output_buffer);
                }
            }

            offset += TS_PACKET_SIZE;
        }

        state->bytes_received += map.size;
        gst_buffer_unmap(buffer, &map);
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

/* Build GStreamer pipeline */
static int build_pipeline(app_state_t *state) {
    GError *error = NULL;
    char pipeline_str[2048];

    /* Build pipeline string based on input type */
    if (strcmp(state->input_type, "srt") == 0) {
        snprintf(pipeline_str, sizeof(pipeline_str),
                 "srtsrc uri=\"%s\" ! tsdemux ! mpegtsmux ! appsink name=sink",
                 state->source_uri);
    } else if (strcmp(state->input_type, "udp") == 0) {
        snprintf(pipeline_str, sizeof(pipeline_str),
                 "udpsrc uri=\"%s\" ! tsparse ! appsink name=sink",
                 state->source_uri);
    } else if (strcmp(state->input_type, "rtmp") == 0) {
        snprintf(pipeline_str, sizeof(pipeline_str),
                 "rtmpsrc location=\"%s\" ! flvdemux ! mpegtsmux ! appsink name=sink",
                 state->source_uri);
    } else if (strcmp(state->input_type, "file") == 0) {
        snprintf(pipeline_str, sizeof(pipeline_str),
                 "filesrc location=\"%s\" ! tsparse ! appsink name=sink",
                 state->source_uri + 7); /* Skip "file://" */
    } else {
        LOG_ERROR("Unsupported input type: %s", state->input_type);
        return -1;
    }

    LOG_DEBUG("Pipeline: %s", pipeline_str);

    /* Parse pipeline */
    state->pipeline = gst_parse_launch(pipeline_str, &error);
    if (!state->pipeline) {
        LOG_ERROR("Failed to create pipeline: %s", error ? error->message : "unknown");
        if (error) g_error_free(error);
        return -1;
    }

    /* Get appsink */
    state->appsink = gst_bin_get_by_name(GST_BIN(state->pipeline), "sink");
    if (!state->appsink) {
        LOG_ERROR("Failed to get appsink element");
        return -1;
    }

    /* Configure appsink */
    g_object_set(state->appsink,
                 "emit-signals", TRUE,
                 "sync", FALSE,
                 "max-buffers", 100,
                 "drop", TRUE,
                 NULL);

    /* Connect callback */
    g_signal_connect(state->appsink, "new-sample",
                     G_CALLBACK(on_new_sample), state);

    return 0;
}

/* Print usage */
static void print_usage(const char *prog) {
    printf("CariTranscoder Input - v1.0.0\n");
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -c, --config FILE   Configuration file path\n");
    printf("  -d, --debug         Enable debug logging\n");
    printf("  -h, --help          Show this help message\n");
    printf("  -v, --version       Show version information\n");
}

/* Main function */
int main(int argc, char *argv[]) {
    int opt;
    int debug = 0;
    const char *config_file = NULL;

    static struct option long_options[] = {
        {"config",  required_argument, 0, 'c'},
        {"debug",   no_argument,       0, 'd'},
        {"help",    no_argument,       0, 'h'},
        {"version", no_argument,       0, 'v'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "c:dhv", long_options, NULL)) != -1) {
        switch (opt) {
            case 'c':
                config_file = optarg;
                break;
            case 'd':
                debug = 1;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            case 'v':
                printf("cari-input version 1.0.0\n");
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    if (!config_file) {
        fprintf(stderr, "Error: Configuration file required\n");
        print_usage(argv[0]);
        return 1;
    }

    /* Initialize logging */
    log_config_t log_cfg = LOG_CONFIG_DEFAULT;
    strncpy(log_cfg.ident, "cari-input", sizeof(log_cfg.ident));
    if (debug) {
        log_cfg.min_level = LOG_LEVEL_DEBUG;
    }
    log_init(&log_cfg);

    LOG_INFO("CariTranscoder Input starting...");

    /* Copy config file path */
    strncpy(g_state.config_file, config_file, sizeof(g_state.config_file) - 1);

    /* Load configuration */
    if (load_config(&g_state) != 0) {
        return 1;
    }

    /* Load and check license */
    const char *license_file = config_get_string(&g_state.config, "system", "license_file",
                                                  "/etc/caritrans/license.key");
    license_load(license_file, &g_state.license);

    if (!license_can_add(&g_state.license, "inputs", 0)) {
        LOG_ERROR("License limit reached for inputs");
        return 1;
    }

    /* Setup signals */
    setup_signals();

    /* Initialize GStreamer */
    gst_init(&argc, &argv);

    /* Create output buffer */
    ring_buffer_options_t rb_opts = RING_BUFFER_OPTIONS_DEFAULT;
    rb_opts.capacity = config_get_uint(&g_state.config, "buffers", "packets_per_buffer",
                                        RING_BUFFER_DEFAULT_PACKETS);

    g_state.output_buffer = ring_buffer_open(g_state.buffer_name, &rb_opts, true);
    if (!g_state.output_buffer) {
        LOG_ERROR("Failed to create output buffer: %s", g_state.buffer_name);
        return 1;
    }

    LOG_INFO("Created output buffer: %s", g_state.buffer_name);

    /* Initialize statistics */
    ts_stats_init(&g_state.stats);

    /* Build pipeline */
    if (build_pipeline(&g_state) != 0) {
        ring_buffer_close(g_state.output_buffer, true);
        return 1;
    }

    /* Create main loop */
    g_state.main_loop = g_main_loop_new(NULL, FALSE);

    /* Start pipeline */
    GstStateChangeReturn ret = gst_element_set_state(g_state.pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("Failed to start pipeline");
        gst_object_unref(g_state.pipeline);
        ring_buffer_close(g_state.output_buffer, true);
        return 1;
    }

    g_state.running = 1;
    LOG_INFO("Input %s (%s) started, listening on %s",
             g_state.name, g_state.id, g_state.source_uri);

    /* Run main loop */
    g_main_loop_run(g_state.main_loop);

    /* Cleanup */
    LOG_INFO("Shutting down...");

    gst_element_set_state(g_state.pipeline, GST_STATE_NULL);
    gst_object_unref(g_state.pipeline);
    g_main_loop_unref(g_state.main_loop);

    /* Calculate final stats */
    ts_stats_calculate_bitrate(&g_state.stats);
    LOG_INFO("Final stats: %lu packets, %lu bytes, %lu CC errors, bitrate: %u bps",
             g_state.stats.total_packets, g_state.stats.total_bytes,
             g_state.stats.cc_errors, g_state.stats.bitrate);

    ring_buffer_close(g_state.output_buffer, true);
    config_free(&g_state.config);
    log_shutdown();

    return 0;
}
