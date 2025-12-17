/*
 * CariTranscoder - Transcoder Application
 * Copyright (c) 2024 CariTech Solutions
 *
 * Reads from input ring buffer, transcodes video/audio, outputs to ring buffer.
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

/* Transcoder state */
typedef struct {
    /* Configuration */
    config_t config;
    char config_file[256];
    char id[64];
    char name[128];

    /* Video settings */
    char video_mode[32];        /* transcode, passthrough, drop */
    char video_codec[32];       /* h264, h265, mpeg2, etc. */
    char encoder_type[32];      /* auto, software, nvenc, vaapi, qsv */
    int video_bitrate;
    int video_width;
    int video_height;

    /* Audio settings */
    char audio_mode[32];
    char audio_codec[32];
    int audio_bitrate;

    /* Buffers */
    char input_buffer_name[128];
    char output_buffer_name[128];
    ring_buffer_t *input_buffer;
    ring_buffer_t *output_buffer;

    /* GStreamer */
    GstElement *pipeline;
    GstElement *appsrc;
    GstElement *appsink;
    GMainLoop *main_loop;

    /* Reader thread */
    pthread_t reader_thread;
    volatile int reader_running;

    /* Statistics */
    uint64_t frames_in;
    uint64_t frames_out;
    uint64_t packets_in;
    uint64_t packets_out;

    /* State */
    volatile int running;
    license_info_t license;
} transcoder_state_t;

static transcoder_state_t g_state = {0};

/* Signal handler */
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

/* Load configuration */
static int load_config(transcoder_state_t *state) {
    if (config_load(&state->config, state->config_file) != 0) {
        CARI_LOG_ERROR("Failed to load configuration");
        return -1;
    }

    strncpy(state->id,
            config_get_string(&state->config, "transcoder", "id", "transcode-001"),
            sizeof(state->id) - 1);

    strncpy(state->name,
            config_get_string(&state->config, "transcoder", "name", "Unnamed"),
            sizeof(state->name) - 1);

    /* Video settings */
    strncpy(state->video_mode,
            config_get_string(&state->config, "video", "mode", "transcode"),
            sizeof(state->video_mode) - 1);

    strncpy(state->video_codec,
            config_get_string(&state->config, "video", "codec", "h265"),
            sizeof(state->video_codec) - 1);

    strncpy(state->encoder_type,
            config_get_string(&state->config, "video", "encoder", "auto"),
            sizeof(state->encoder_type) - 1);

    state->video_bitrate = config_get_int(&state->config, "video", "bitrate", 8000000);
    state->video_width = config_get_int(&state->config, "video_scaling", "width", 0);
    state->video_height = config_get_int(&state->config, "video_scaling", "height", 0);

    /* Audio settings */
    strncpy(state->audio_mode,
            config_get_string(&state->config, "audio", "mode", "transcode"),
            sizeof(state->audio_mode) - 1);

    strncpy(state->audio_codec,
            config_get_string(&state->config, "audio", "codec", "aac"),
            sizeof(state->audio_codec) - 1);

    state->audio_bitrate = config_get_int(&state->config, "audio", "bitrate", 192000);

    /* Buffer names */
    strncpy(state->input_buffer_name,
            config_get_string(&state->config, "input", "buffer_name", ""),
            sizeof(state->input_buffer_name) - 1);

    strncpy(state->output_buffer_name,
            config_get_string(&state->config, "output", "buffer_name", state->id),
            sizeof(state->output_buffer_name) - 1);

    CARI_LOG_INFO("Configured: %s (%s)", state->name, state->id);
    CARI_LOG_INFO("Video: %s -> %s @ %d bps", state->video_mode, state->video_codec, state->video_bitrate);
    CARI_LOG_INFO("Audio: %s -> %s @ %d bps", state->audio_mode, state->audio_codec, state->audio_bitrate);

    return 0;
}

/* Get encoder element name */
static const char* get_encoder_element(transcoder_state_t *state) {
    if (strcmp(state->video_codec, "h264") == 0) {
        if (strcmp(state->encoder_type, "nvenc") == 0) return "nvh264enc";
        if (strcmp(state->encoder_type, "vaapi") == 0) return "vaapih264enc";
        if (strcmp(state->encoder_type, "qsv") == 0) return "qsvh264enc";
        return "x264enc";
    } else if (strcmp(state->video_codec, "h265") == 0) {
        if (strcmp(state->encoder_type, "nvenc") == 0) return "nvh265enc";
        if (strcmp(state->encoder_type, "vaapi") == 0) return "vaapih265enc";
        if (strcmp(state->encoder_type, "qsv") == 0) return "qsvh265enc";
        return "x265enc";
    } else if (strcmp(state->video_codec, "mpeg2") == 0) {
        return "mpeg2enc";
    }
    return "x264enc";
}

/* Reader thread - reads from input buffer and feeds appsrc */
static void* reader_thread_func(void *arg) {
    transcoder_state_t *state = (transcoder_state_t *)arg;
    ts_packet_raw_t packets[7]; /* Read 7 packets at a time (1316 bytes) */

    CARI_LOG_DEBUG("Reader thread started");

    while (state->reader_running) {
        int count = ring_buffer_read_batch(state->input_buffer, packets, 7);

        if (count > 0) {
            /* Push to appsrc */
            GstBuffer *buffer = gst_buffer_new_allocate(NULL, count * TS_PACKET_SIZE, NULL);
            GstMapInfo map;

            if (gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
                memcpy(map.data, packets, count * TS_PACKET_SIZE);
                gst_buffer_unmap(buffer, &map);

                GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(state->appsrc), buffer);
                if (ret != GST_FLOW_OK) {
                    CARI_LOG_WARNING("Failed to push buffer to appsrc");
                }
                state->packets_in += count;
            } else {
                gst_buffer_unref(buffer);
            }

            ring_buffer_heartbeat(state->input_buffer);
        } else {
            /* No data, wait a bit */
            usleep(1000);
        }
    }

    CARI_LOG_DEBUG("Reader thread stopped");
    return NULL;
}

/* Callback for new samples from appsink */
static GstFlowReturn on_new_sample(GstAppSink *appsink, gpointer user_data) {
    transcoder_state_t *state = (transcoder_state_t *)user_data;
    GstSample *sample = gst_app_sink_pull_sample(appsink);

    if (!sample) return GST_FLOW_ERROR;

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;

    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        /* Write to output buffer */
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

    state->frames_out++;
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

/* Build transcoding pipeline */
static int build_pipeline(transcoder_state_t *state) {
    GError *error = NULL;
    char pipeline_str[4096];
    const char *encoder = get_encoder_element(state);

    /* Build pipeline based on modes */
    if (strcmp(state->video_mode, "passthrough") == 0 &&
        strcmp(state->audio_mode, "passthrough") == 0) {
        /* Full passthrough */
        snprintf(pipeline_str, sizeof(pipeline_str),
                 "appsrc name=src ! tsparse ! appsink name=sink");
    } else {
        /* Transcoding pipeline */
        char video_branch[1024] = "";
        char audio_branch[512] = "";

        if (strcmp(state->video_mode, "transcode") == 0) {
            /* Video transcoding */
            char scale_str[128] = "";
            if (state->video_width > 0 && state->video_height > 0) {
                snprintf(scale_str, sizeof(scale_str),
                         "videoscale ! video/x-raw,width=%d,height=%d ! ",
                         state->video_width, state->video_height);
            }

            snprintf(video_branch, sizeof(video_branch),
                     "tsdemux name=demux ! queue ! h264parse ! avdec_h264 ! "
                     "%s%s bitrate=%d ! h264parse ! queue ! mux.",
                     scale_str, encoder, state->video_bitrate / 1000);
        } else if (strcmp(state->video_mode, "passthrough") == 0) {
            snprintf(video_branch, sizeof(video_branch),
                     "tsdemux name=demux ! queue ! h264parse ! mux.");
        }

        if (strcmp(state->audio_mode, "transcode") == 0) {
            const char *audio_enc = "faac";
            if (strcmp(state->audio_codec, "ac3") == 0) audio_enc = "avenc_ac3";
            else if (strcmp(state->audio_codec, "mp2") == 0) audio_enc = "twolame";

            snprintf(audio_branch, sizeof(audio_branch),
                     "demux. ! queue ! aacparse ! avdec_aac ! audioconvert ! "
                     "%s bitrate=%d ! aacparse ! queue ! mux.",
                     audio_enc, state->audio_bitrate);
        } else if (strcmp(state->audio_mode, "passthrough") == 0) {
            snprintf(audio_branch, sizeof(audio_branch),
                     "demux. ! queue ! aacparse ! mux.");
        }

        snprintf(pipeline_str, sizeof(pipeline_str),
                 "appsrc name=src format=time ! tsparse ! %s %s "
                 "mpegtsmux name=mux ! appsink name=sink",
                 video_branch, audio_branch);
    }

    CARI_LOG_DEBUG("Pipeline: %s", pipeline_str);

    state->pipeline = gst_parse_launch(pipeline_str, &error);
    if (!state->pipeline) {
        CARI_LOG_ERROR("Failed to create pipeline: %s", error ? error->message : "unknown");
        if (error) g_error_free(error);
        return -1;
    }

    state->appsrc = gst_bin_get_by_name(GST_BIN(state->pipeline), "src");
    state->appsink = gst_bin_get_by_name(GST_BIN(state->pipeline), "sink");

    if (!state->appsrc || !state->appsink) {
        CARI_LOG_ERROR("Failed to get appsrc/appsink");
        return -1;
    }

    /* Configure appsrc */
    g_object_set(state->appsrc,
                 "stream-type", 0, /* GST_APP_STREAM_TYPE_STREAM */
                 "format", GST_FORMAT_TIME,
                 "is-live", TRUE,
                 NULL);

    /* Configure appsink */
    g_object_set(state->appsink,
                 "emit-signals", TRUE,
                 "sync", FALSE,
                 NULL);

    g_signal_connect(state->appsink, "new-sample", G_CALLBACK(on_new_sample), state);

    return 0;
}

static void print_usage(const char *prog) {
    printf("CariTranscoder Transcoder - v1.0.0\n");
    printf("Usage: %s -c <config_file> [options]\n", prog);
    printf("Options:\n");
    printf("  -c, --config FILE   Configuration file\n");
    printf("  -d, --debug         Enable debug logging\n");
    printf("  -h, --help          Show help\n");
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
            default: print_usage(argv[0]); return 1;
        }
    }

    if (!config_file) {
        fprintf(stderr, "Error: Configuration file required\n");
        return 1;
    }

    /* Initialize logging */
    log_config_t log_cfg = LOG_CONFIG_DEFAULT;
    strncpy(log_cfg.ident, "cari-transcoder", sizeof(log_cfg.ident));
    if (debug) log_cfg.min_level = LOG_LEVEL_DEBUG;
    log_init(&log_cfg);

    CARI_LOG_INFO("CariTranscoder Transcoder starting...");

    strncpy(g_state.config_file, config_file, sizeof(g_state.config_file) - 1);

    if (load_config(&g_state) != 0) return 1;

    /* Setup signals */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Initialize GStreamer */
    gst_init(&argc, &argv);

    /* Open input buffer */
    g_state.input_buffer = ring_buffer_open(g_state.input_buffer_name, NULL, false);
    if (!g_state.input_buffer) {
        CARI_LOG_ERROR("Failed to open input buffer: %s", g_state.input_buffer_name);
        return 1;
    }

    /* Create output buffer */
    ring_buffer_options_t rb_opts = RING_BUFFER_OPTIONS_DEFAULT;
    g_state.output_buffer = ring_buffer_open(g_state.output_buffer_name, &rb_opts, true);
    if (!g_state.output_buffer) {
        CARI_LOG_ERROR("Failed to create output buffer");
        ring_buffer_close(g_state.input_buffer, false);
        return 1;
    }

    /* Build pipeline */
    if (build_pipeline(&g_state) != 0) {
        ring_buffer_close(g_state.input_buffer, false);
        ring_buffer_close(g_state.output_buffer, true);
        return 1;
    }

    /* Start reader thread */
    g_state.reader_running = 1;
    pthread_create(&g_state.reader_thread, NULL, reader_thread_func, &g_state);

    /* Create and run main loop */
    g_state.main_loop = g_main_loop_new(NULL, FALSE);

    if (gst_element_set_state(g_state.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        CARI_LOG_ERROR("Failed to start pipeline");
        g_state.reader_running = 0;
        pthread_join(g_state.reader_thread, NULL);
        return 1;
    }

    g_state.running = 1;
    CARI_LOG_INFO("Transcoder %s started", g_state.id);

    g_main_loop_run(g_state.main_loop);

    /* Cleanup */
    CARI_LOG_INFO("Shutting down...");

    g_state.reader_running = 0;
    pthread_join(g_state.reader_thread, NULL);

    gst_element_set_state(g_state.pipeline, GST_STATE_NULL);
    gst_object_unref(g_state.pipeline);
    g_main_loop_unref(g_state.main_loop);

    CARI_LOG_INFO("Stats: %lu packets in, %lu packets out, %lu frames",
             g_state.packets_in, g_state.packets_out, g_state.frames_out);

    ring_buffer_close(g_state.input_buffer, false);
    ring_buffer_close(g_state.output_buffer, true);
    config_free(&g_state.config);
    log_shutdown();

    return 0;
}
