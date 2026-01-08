/*
 * CariMux - GStreamer-based MPTS Multiplexer
 *
 * Multiplexes multiple SPTS UDP inputs into a single MPTS output.
 * Uses tsdemux -> parse -> mpegtsmux pipeline.
 * Output can be piped to tsp for SDT/NIT injection.
 *
 * Copyright (c) 2024 CariTech Solutions
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <pthread.h>
#include <errno.h>
#include <gst/gst.h>

#define VERSION "3.0.0"
#define MAX_SERVICES 16

/* Stream types detected via ffprobe */
typedef enum {
    STREAM_TYPE_UNKNOWN = 0,
    STREAM_TYPE_H264,
    STREAM_TYPE_H265,
    STREAM_TYPE_MPEG2,
    STREAM_TYPE_AAC,
    STREAM_TYPE_AC3,
    STREAM_TYPE_EAC3,
    STREAM_TYPE_MP2
} StreamType;

/* Service/input definition */
typedef struct {
    int enabled;
    char address[64];
    int port;
    int program_number;         /* Service ID in MPTS */
    int video_pid;              /* Output video PID */
    int audio_pid;              /* Output audio PID */
    int pcr_pid;                /* Which PID carries PCR (video_pid or audio_pid) */
    StreamType video_type;      /* Detected video codec */
    StreamType audio_type;      /* Detected audio codec */
    gboolean video_linked;      /* Video pad connected */
    gboolean audio_linked;      /* Audio pad connected */
    gboolean pcr_linked;        /* PCR stream has been linked */
    GstElement *demux;          /* tsdemux element for this input */

    /* Pending pads for delayed linking (link PCR first) */
    GstPad *pending_video_pad;
    GstElement *pending_video_queue;
    GstElement *pending_video_parser;
    GstPad *pending_audio_pad;
    GstElement *pending_audio_queue;
    GstElement *pending_audio_parser;
} ServiceInput;

/* Application context */
typedef struct {
    /* Services */
    ServiceInput services[MAX_SERVICES];
    int service_count;

    /* Output settings */
    gboolean use_stdout;
    char udp_host[256];
    int udp_port;

    /* Runtime */
    volatile int running;
    GstElement *pipeline;
    GstElement *mux;
    GMainLoop *main_loop;

    /* Options */
    gboolean debug;
    gboolean detect_only;
} AppContext;

static AppContext g_ctx = {0};

/*
 * Signal handler
 */
static void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        fprintf(stderr, "\nReceived signal %d, shutting down...\n", signum);
        g_ctx.running = 0;
        if (g_ctx.main_loop) {
            g_main_loop_quit(g_ctx.main_loop);
        }
    }
}

/*
 * Get parser element name for stream type
 */
static const char *get_parser_for_type(StreamType type) {
    switch (type) {
        case STREAM_TYPE_H264:  return "h264parse";
        case STREAM_TYPE_H265:  return "h265parse";
        case STREAM_TYPE_MPEG2: return "mpegvideoparse";
        case STREAM_TYPE_AAC:   return "aacparse";
        case STREAM_TYPE_AC3:   return "ac3parse";
        case STREAM_TYPE_EAC3:  return "ac3parse";
        case STREAM_TYPE_MP2:   return "mpegaudioparse";
        default:                return NULL;
    }
}

/*
 * Get stream type from codec name (ffprobe output)
 */
static StreamType get_stream_type(const char *codec_name) {
    if (!codec_name) return STREAM_TYPE_UNKNOWN;

    if (strcmp(codec_name, "h264") == 0) return STREAM_TYPE_H264;
    if (strcmp(codec_name, "hevc") == 0 || strcmp(codec_name, "h265") == 0) return STREAM_TYPE_H265;
    if (strcmp(codec_name, "mpeg2video") == 0) return STREAM_TYPE_MPEG2;
    if (strcmp(codec_name, "aac") == 0) return STREAM_TYPE_AAC;
    if (strcmp(codec_name, "ac3") == 0) return STREAM_TYPE_AC3;
    if (strcmp(codec_name, "eac3") == 0) return STREAM_TYPE_EAC3;
    if (strcmp(codec_name, "mp2") == 0 || strcmp(codec_name, "mp3") == 0) return STREAM_TYPE_MP2;

    return STREAM_TYPE_UNKNOWN;
}

/*
 * Simple JSON string extractor
 */
static char *json_get_string(const char *json, const char *key) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);

    const char *pos = strstr(json, pattern);
    if (!pos) return NULL;

    pos += strlen(pattern);
    while (*pos == ' ' || *pos == '\t') pos++;

    if (*pos != '"') return NULL;
    pos++;

    const char *end = strchr(pos, '"');
    if (!end) return NULL;

    size_t len = end - pos;
    char *result = malloc(len + 1);
    if (result) {
        memcpy(result, pos, len);
        result[len] = '\0';
    }
    return result;
}

/*
 * Detect streams using ffprobe
 */
static int detect_stream(ServiceInput *svc) {
    char cmd[512];
    char url[256];
    FILE *fp;
    char *output = NULL;
    size_t output_size = 0;
    size_t output_capacity = 32768;

    snprintf(url, sizeof(url), "udp://@%s:%d", svc->address, svc->port);

    snprintf(cmd, sizeof(cmd),
             "ffprobe -v quiet -print_format json -show_streams "
             "-analyzeduration 3000000 -probesize 3000000 -i \"%s\" 2>/dev/null",
             url);

    if (g_ctx.debug) {
        fprintf(stderr, "Probing: %s\n", url);
    }

    fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "Error: Failed to run ffprobe for %s:%d\n", svc->address, svc->port);
        return -1;
    }

    output = malloc(output_capacity);
    if (!output) {
        pclose(fp);
        return -1;
    }

    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), fp)) {
        size_t len = strlen(buffer);
        if (output_size + len >= output_capacity) {
            output_capacity *= 2;
            char *new_output = realloc(output, output_capacity);
            if (!new_output) {
                free(output);
                pclose(fp);
                return -1;
            }
            output = new_output;
        }
        memcpy(output + output_size, buffer, len);
        output_size += len;
    }
    output[output_size] = '\0';

    int status = pclose(fp);
    if (status != 0) {
        fprintf(stderr, "Warning: ffprobe returned status %d for %s:%d\n",
                status, svc->address, svc->port);
        free(output);
        return -1;
    }

    /* Parse streams */
    const char *stream_pos = output;
    while ((stream_pos = strstr(stream_pos, "\"codec_type\"")) != NULL) {
        /* Find the enclosing object */
        const char *obj_start = stream_pos;
        int brace_count = 0;
        while (obj_start > output) {
            obj_start--;
            if (*obj_start == '{') {
                brace_count++;
                if (brace_count == 1) break;
            } else if (*obj_start == '}') {
                brace_count--;
            }
        }

        const char *obj_end = stream_pos;
        brace_count = 0;
        while (*obj_end) {
            if (*obj_end == '{') brace_count++;
            else if (*obj_end == '}') {
                brace_count--;
                if (brace_count < 0) break;
            }
            obj_end++;
        }

        size_t obj_len = obj_end - obj_start + 1;
        char *stream_json = malloc(obj_len + 1);
        if (!stream_json) break;
        memcpy(stream_json, obj_start, obj_len);
        stream_json[obj_len] = '\0';

        char *codec_type = json_get_string(stream_json, "codec_type");
        char *codec_name = json_get_string(stream_json, "codec_name");

        if (codec_type && codec_name) {
            if (strcmp(codec_type, "video") == 0 && svc->video_type == STREAM_TYPE_UNKNOWN) {
                svc->video_type = get_stream_type(codec_name);
                if (g_ctx.debug) {
                    fprintf(stderr, "  Video: %s\n", codec_name);
                }
            } else if (strcmp(codec_type, "audio") == 0 && svc->audio_type == STREAM_TYPE_UNKNOWN) {
                svc->audio_type = get_stream_type(codec_name);
                if (g_ctx.debug) {
                    fprintf(stderr, "  Audio: %s\n", codec_name);
                }
            }
        }

        free(codec_type);
        free(codec_name);
        free(stream_json);
        stream_pos++;
    }

    free(output);

    return (svc->video_type != STREAM_TYPE_UNKNOWN ||
            svc->audio_type != STREAM_TYPE_UNKNOWN) ? 0 : -1;
}

/*
 * Find service by demux element
 */
static ServiceInput *find_service_by_demux(GstElement *demux) {
    for (int i = 0; i < g_ctx.service_count; i++) {
        if (g_ctx.services[i].demux == demux) {
            return &g_ctx.services[i];
        }
    }
    return NULL;
}

/*
 * Link a parser to the mux on a specific PID
 */
static gboolean link_parser_to_mux(GstElement *parser, int pid, const char *stream_type) {
    char sink_pad_name[32];
    snprintf(sink_pad_name, sizeof(sink_pad_name), "sink_%d", pid);

    GstPad *mux_sink = gst_element_request_pad_simple(g_ctx.mux, sink_pad_name);
    if (!mux_sink) {
        /* Fallback to generic request */
        mux_sink = gst_element_request_pad_simple(g_ctx.mux, "sink_%d");
    }

    if (mux_sink) {
        GstPad *parser_src = gst_element_get_static_pad(parser, "src");
        GstPadLinkReturn ret = gst_pad_link(parser_src, mux_sink);
        gst_object_unref(parser_src);
        gst_object_unref(mux_sink);

        if (ret != GST_PAD_LINK_OK) {
            fprintf(stderr, "Error: Failed to link %s parser to mux (PID %d)\n", stream_type, pid);
            return FALSE;
        }
        return TRUE;
    }
    return FALSE;
}

/*
 * Link any pending (non-PCR) streams after PCR is linked
 */
static void link_pending_streams(ServiceInput *svc) {
    if (svc->pending_video_parser && !svc->video_linked) {
        if (link_parser_to_mux(svc->pending_video_parser, svc->video_pid, "video")) {
            svc->video_linked = TRUE;
            fprintf(stderr, "Service %d: Video linked (PID %d) [deferred]\n",
                    svc->program_number, svc->video_pid);
        }
        svc->pending_video_pad = NULL;
        svc->pending_video_queue = NULL;
        svc->pending_video_parser = NULL;
    }

    if (svc->pending_audio_parser && !svc->audio_linked) {
        if (link_parser_to_mux(svc->pending_audio_parser, svc->audio_pid, "audio")) {
            svc->audio_linked = TRUE;
            fprintf(stderr, "Service %d: Audio linked (PID %d) [deferred]\n",
                    svc->program_number, svc->audio_pid);
        }
        svc->pending_audio_pad = NULL;
        svc->pending_audio_queue = NULL;
        svc->pending_audio_parser = NULL;
    }
}

/*
 * Callback when tsdemux discovers a new pad
 *
 * PCR Control via Delayed Linking:
 * - The first stream linked to mpegtsmux for a program becomes the PCR carrier
 * - We want pcr_pid to be PCR, so we link it first
 * - If a non-PCR stream arrives before PCR stream, we queue it
 * - Once PCR stream is linked, we link any pending streams
 */
static void on_demux_pad_added(GstElement *demux, GstPad *pad, gpointer user_data) {
    (void)user_data;

    ServiceInput *svc = find_service_by_demux(demux);
    if (!svc) {
        fprintf(stderr, "Warning: pad-added from unknown demux\n");
        return;
    }

    const gchar *pad_name = GST_PAD_NAME(pad);
    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps) {
        caps = gst_pad_query_caps(pad, NULL);
    }

    if (!caps) {
        fprintf(stderr, "Warning: No caps on pad %s\n", pad_name);
        return;
    }

    const gchar *media_type = gst_structure_get_name(gst_caps_get_structure(caps, 0));

    if (g_ctx.debug) {
        fprintf(stderr, "Service %d: pad-added '%s' type=%s (pcr_pid=%d, pcr_linked=%d)\n",
                svc->program_number, pad_name, media_type, svc->pcr_pid, svc->pcr_linked);
    }

    gboolean is_video = g_str_has_prefix(media_type, "video/");
    gboolean is_audio = g_str_has_prefix(media_type, "audio/");

    /* Determine which PID this stream will use and if it's the PCR stream */
    int this_pid = 0;
    StreamType stream_type = STREAM_TYPE_UNKNOWN;
    gboolean *linked_flag = NULL;
    GstPad **pending_pad = NULL;
    GstElement **pending_queue = NULL;
    GstElement **pending_parser = NULL;
    const char *stream_name = NULL;

    if (is_video && !svc->video_linked) {
        this_pid = svc->video_pid;
        stream_type = svc->video_type;
        linked_flag = &svc->video_linked;
        pending_pad = &svc->pending_video_pad;
        pending_queue = &svc->pending_video_queue;
        pending_parser = &svc->pending_video_parser;
        stream_name = "Video";
    } else if (is_audio && !svc->audio_linked) {
        this_pid = svc->audio_pid;
        stream_type = svc->audio_type;
        linked_flag = &svc->audio_linked;
        pending_pad = &svc->pending_audio_pad;
        pending_queue = &svc->pending_audio_queue;
        pending_parser = &svc->pending_audio_parser;
        stream_name = "Audio";
    } else {
        /* Already linked or unknown type */
        gst_caps_unref(caps);
        return;
    }

    gboolean is_pcr_stream = (this_pid == svc->pcr_pid);

    const char *parser_name = get_parser_for_type(stream_type);
    if (!parser_name) {
        fprintf(stderr, "Warning: No parser for %s type %d\n", stream_name, stream_type);
        gst_caps_unref(caps);
        return;
    }

    /* Create queue -> parser chain */
    GstElement *queue = gst_element_factory_make("queue", NULL);
    GstElement *parser = gst_element_factory_make(parser_name, NULL);

    if (!queue || !parser) {
        fprintf(stderr, "Error: Failed to create %s elements\n", stream_name);
        gst_caps_unref(caps);
        return;
    }

    gst_bin_add_many(GST_BIN(g_ctx.pipeline), queue, parser, NULL);
    gst_element_sync_state_with_parent(queue);
    gst_element_sync_state_with_parent(parser);

    /* Link: demux pad -> queue -> parser */
    GstPad *queue_sink = gst_element_get_static_pad(queue, "sink");
    if (gst_pad_link(pad, queue_sink) != GST_PAD_LINK_OK) {
        fprintf(stderr, "Error: Failed to link demux to %s queue\n", stream_name);
    }
    gst_object_unref(queue_sink);

    gst_element_link(queue, parser);

    /*
     * Delayed linking logic for PCR control:
     * - If this is the PCR stream: link immediately, then link any pending
     * - If PCR already linked: link immediately
     * - Otherwise: queue for later (wait for PCR stream)
     */
    if (is_pcr_stream) {
        /* This is the PCR stream - link it first */
        if (link_parser_to_mux(parser, this_pid, stream_name)) {
            *linked_flag = TRUE;
            svc->pcr_linked = TRUE;
            fprintf(stderr, "Service %d: %s linked (PID %d) [PCR]\n",
                    svc->program_number, stream_name, this_pid);

            /* Now link any pending non-PCR streams */
            link_pending_streams(svc);
        }
    } else if (svc->pcr_linked) {
        /* PCR already linked, we can link this stream now */
        if (link_parser_to_mux(parser, this_pid, stream_name)) {
            *linked_flag = TRUE;
            fprintf(stderr, "Service %d: %s linked (PID %d)\n",
                    svc->program_number, stream_name, this_pid);
        }
    } else {
        /* PCR not linked yet - queue this stream for later */
        *pending_pad = pad;
        *pending_queue = queue;
        *pending_parser = parser;
        fprintf(stderr, "Service %d: %s queued (PID %d) - waiting for PCR (PID %d)\n",
                svc->program_number, stream_name, this_pid, svc->pcr_pid);
    }

    gst_caps_unref(caps);
}

/*
 * Build program-map string for mpegtsmux
 * Format: "program_map,sink_PID=program_number,sink_PID=program_number,..."
 * This tells the mux which program each PID belongs to
 */
static char *build_prog_map(void) {
    static char prog_map[2048];
    char *p = prog_map;
    int remaining = sizeof(prog_map);
    int n;

    /* Start with the GstStructure name */
    n = snprintf(p, remaining, "program_map");
    p += n; remaining -= n;

    for (int i = 0; i < g_ctx.service_count; i++) {
        ServiceInput *svc = &g_ctx.services[i];

        /* Add video PID mapping */
        n = snprintf(p, remaining, ",sink_%d=%d",
                     svc->video_pid, svc->program_number);
        p += n; remaining -= n;

        /* Add audio PID mapping */
        n = snprintf(p, remaining, ",sink_%d=%d",
                     svc->audio_pid, svc->program_number);
        p += n; remaining -= n;
    }

    return prog_map;
}

/*
 * GStreamer bus message handler
 */
static gboolean on_bus_message(GstBus *bus, GstMessage *msg, gpointer data) {
    (void)bus;
    (void)data;

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar *debug = NULL;
            gst_message_parse_error(msg, &err, &debug);
            fprintf(stderr, "Error from %s: %s\n",
                    GST_OBJECT_NAME(msg->src), err->message);
            if (debug && g_ctx.debug) {
                fprintf(stderr, "Debug: %s\n", debug);
            }
            g_error_free(err);
            g_free(debug);
            g_ctx.running = 0;
            if (g_ctx.main_loop) {
                g_main_loop_quit(g_ctx.main_loop);
            }
            break;
        }

        case GST_MESSAGE_WARNING: {
            GError *err = NULL;
            gchar *debug = NULL;
            gst_message_parse_warning(msg, &err, &debug);
            fprintf(stderr, "Warning from %s: %s\n",
                    GST_OBJECT_NAME(msg->src), err->message);
            g_error_free(err);
            g_free(debug);
            break;
        }

        case GST_MESSAGE_EOS:
            fprintf(stderr, "End of stream\n");
            g_ctx.running = 0;
            if (g_ctx.main_loop) {
                g_main_loop_quit(g_ctx.main_loop);
            }
            break;

        case GST_MESSAGE_STATE_CHANGED:
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(g_ctx.pipeline) && g_ctx.debug) {
                GstState old_state, new_state, pending_state;
                gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
                fprintf(stderr, "Pipeline state: %s -> %s\n",
                        gst_element_state_get_name(old_state),
                        gst_element_state_get_name(new_state));
            }
            break;

        default:
            break;
    }

    return TRUE;
}

/*
 * Create the pipeline
 */
static int create_pipeline(void) {
    g_ctx.pipeline = gst_pipeline_new("mux-pipeline");
    if (!g_ctx.pipeline) {
        fprintf(stderr, "Error: Failed to create pipeline\n");
        return -1;
    }

    /* Create mpegtsmux */
    g_ctx.mux = gst_element_factory_make("mpegtsmux", "mux");
    if (!g_ctx.mux) {
        fprintf(stderr, "Error: Failed to create mpegtsmux (is gst-plugins-bad installed?)\n");
        return -1;
    }

    /* Set program map for multi-program output */
    char *prog_map = build_prog_map();
    fprintf(stderr, "Program map: %s\n", prog_map);

    /* Configure mux settings:
     * - prog-map: program membership for each PID
     * - alignment: 7 for proper UDP packet alignment (7 * 188 = 1316 bytes)
     */
    g_object_set(g_ctx.mux,
                 "prog-map", prog_map,
                 "alignment", 7,
                 NULL);

    gst_bin_add(GST_BIN(g_ctx.pipeline), g_ctx.mux);

    /* Create output sink */
    GstElement *sink;
    if (g_ctx.use_stdout) {
        sink = gst_element_factory_make("fdsink", "sink");
        g_object_set(sink, "fd", 1, NULL);  /* stdout */
        g_object_set(sink, "sync", FALSE, NULL);
    } else {
        sink = gst_element_factory_make("udpsink", "sink");
        g_object_set(sink,
                     "host", g_ctx.udp_host,
                     "port", g_ctx.udp_port,
                     "sync", FALSE,
                     "async", FALSE,
                     "auto-multicast", TRUE,
                     NULL);
    }

    if (!sink) {
        fprintf(stderr, "Error: Failed to create output sink\n");
        return -1;
    }

    gst_bin_add(GST_BIN(g_ctx.pipeline), sink);
    gst_element_link(g_ctx.mux, sink);

    /* Create input chain for each service */
    for (int i = 0; i < g_ctx.service_count; i++) {
        ServiceInput *svc = &g_ctx.services[i];
        char elem_name[64];

        /* udpsrc */
        snprintf(elem_name, sizeof(elem_name), "udpsrc_%d", i);
        GstElement *udpsrc = gst_element_factory_make("udpsrc", elem_name);
        if (!udpsrc) {
            fprintf(stderr, "Error: Failed to create udpsrc for service %d\n", i);
            return -1;
        }

        char uri[256];
        snprintf(uri, sizeof(uri), "udp://%s:%d", svc->address, svc->port);
        g_object_set(udpsrc,
                     "uri", uri,
                     "buffer-size", 2097152,
                     NULL);

        /* queue for input buffering */
        snprintf(elem_name, sizeof(elem_name), "queue_in_%d", i);
        GstElement *queue = gst_element_factory_make("queue", elem_name);

        /* tsparse for clean TS handling */
        snprintf(elem_name, sizeof(elem_name), "tsparse_%d", i);
        GstElement *tsparse = gst_element_factory_make("tsparse", elem_name);

        /* tsdemux */
        snprintf(elem_name, sizeof(elem_name), "tsdemux_%d", i);
        GstElement *tsdemux = gst_element_factory_make("tsdemux", elem_name);
        if (!tsdemux) {
            fprintf(stderr, "Error: Failed to create tsdemux for service %d\n", i);
            return -1;
        }

        svc->demux = tsdemux;

        /* Connect pad-added signal for dynamic linking */
        g_signal_connect(tsdemux, "pad-added", G_CALLBACK(on_demux_pad_added), NULL);

        /* Add elements to pipeline */
        gst_bin_add_many(GST_BIN(g_ctx.pipeline), udpsrc, queue, tsparse, tsdemux, NULL);

        /* Link static elements */
        if (!gst_element_link_many(udpsrc, queue, tsparse, tsdemux, NULL)) {
            fprintf(stderr, "Error: Failed to link input chain for service %d\n", i);
            return -1;
        }

        fprintf(stderr, "Service %d: %s:%d -> Program %d (V:%d A:%d PCR:%d)\n",
                i + 1, svc->address, svc->port, svc->program_number,
                svc->video_pid, svc->audio_pid, svc->pcr_pid);
    }

    /* Set up bus watch */
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(g_ctx.pipeline));
    gst_bus_add_watch(bus, on_bus_message, NULL);
    gst_object_unref(bus);

    return 0;
}

/*
 * Print help
 */
static void print_help(const char *prog) {
    printf("CariMux v%s - GStreamer MPTS Multiplexer\n\n", VERSION);
    printf("Usage: %s [options] -i ADDR:PORT[:PROG:VPID:APID:PCRPID] ...\n\n", prog);
    printf("Options:\n");
    printf("  -i, --input ADDR:PORT[:PROG:VPID:APID:PCRPID]\n");
    printf("                         Add input service (can specify multiple)\n");
    printf("                         PROG   = program number (default: auto 1,2,3...)\n");
    printf("                         VPID   = video PID (default: 100,200,300...)\n");
    printf("                         APID   = audio PID (default: 101,201,301...)\n");
    printf("                         PCRPID = which PID carries PCR (default: VPID)\n");
    printf("  -o, --output HOST:PORT UDP output address\n");
    printf("  --stdout               Output to stdout (for piping to tsp)\n");
    printf("  --detect-only          Detect streams and exit\n");
    printf("  -d, --debug            Enable debug output\n");
    printf("  -h, --help             Show this help\n");
    printf("\n");
    printf("PCR Control:\n");
    printf("  The PCRPID parameter controls which PID carries the Program Clock Reference.\n");
    printf("  By default, the video PID carries PCR. Set PCRPID to the audio PID to have\n");
    printf("  audio carry PCR instead.\n");
    printf("\n");
    printf("Examples:\n");
    printf("  Single input to UDP:\n");
    printf("    %s -i 239.100.0.1:10000 -o 239.1.1.100:5500\n\n", prog);
    printf("  Multiple inputs with custom PIDs (video carries PCR):\n");
    printf("    %s -i 239.100.0.1:10000:1:100:101:100 \\\n", prog);
    printf("       -i 239.100.0.2:10000:2:200:201:200 \\\n");
    printf("       -o 239.1.1.100:5500\n\n");
    printf("  Audio carrying PCR:\n");
    printf("    %s -i 239.100.0.1:10000:1:100:101:101 -o 239.1.1.100:5500\n\n", prog);
    printf("  Output to tsp for SDT injection and CBR:\n");
    printf("    %s -i 239.100.0.1:10000:1:100:101 --stdout | \\\n", prog);
    printf("       tsp -I file - \\\n");
    printf("           -P inject sdt.xml --pid 17 --replace --stuffing \\\n");
    printf("           -P regulate \\\n");
    printf("           -O ip 239.1.1.100:5500\n");
}

/*
 * Parse input argument: ADDR:PORT[:PROG:VPID:APID:PCRPID]
 */
static int parse_input(const char *arg, ServiceInput *svc, int index) {
    char buf[256];
    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Default values */
    svc->program_number = index + 1;
    svc->video_pid = 100 + index * 100;
    svc->audio_pid = 101 + index * 100;
    svc->pcr_pid = 0;  /* Will default to video_pid if not set */
    svc->video_type = STREAM_TYPE_UNKNOWN;
    svc->audio_type = STREAM_TYPE_UNKNOWN;
    svc->enabled = 1;
    svc->pcr_linked = FALSE;
    svc->pending_video_pad = NULL;
    svc->pending_audio_pad = NULL;

    /* Parse ADDR:PORT */
    char *colon1 = strchr(buf, ':');
    if (!colon1) {
        fprintf(stderr, "Error: Input must be ADDR:PORT format\n");
        return -1;
    }
    *colon1 = '\0';
    strncpy(svc->address, buf, sizeof(svc->address) - 1);

    char *rest = colon1 + 1;
    char *colon2 = strchr(rest, ':');

    if (colon2) {
        *colon2 = '\0';
        svc->port = atoi(rest);

        /* Parse optional PROG:VPID:APID:PCRPID */
        char *parts[4] = {NULL, NULL, NULL, NULL};
        parts[0] = colon2 + 1;

        char *c = strchr(parts[0], ':');
        if (c) {
            *c = '\0';
            parts[1] = c + 1;
            c = strchr(parts[1], ':');
            if (c) {
                *c = '\0';
                parts[2] = c + 1;
                c = strchr(parts[2], ':');
                if (c) {
                    *c = '\0';
                    parts[3] = c + 1;
                }
            }
        }

        if (parts[0] && strlen(parts[0]) > 0) svc->program_number = atoi(parts[0]);
        if (parts[1] && strlen(parts[1]) > 0) svc->video_pid = atoi(parts[1]);
        if (parts[2] && strlen(parts[2]) > 0) svc->audio_pid = atoi(parts[2]);
        if (parts[3] && strlen(parts[3]) > 0) svc->pcr_pid = atoi(parts[3]);
    } else {
        svc->port = atoi(rest);
    }

    /* Default PCR to video PID if not specified */
    if (svc->pcr_pid == 0) {
        svc->pcr_pid = svc->video_pid;
    }

    /* Validate PCR PID is either video or audio */
    if (svc->pcr_pid != svc->video_pid && svc->pcr_pid != svc->audio_pid) {
        fprintf(stderr, "Warning: PCRPID %d is not video (%d) or audio (%d), using video\n",
                svc->pcr_pid, svc->video_pid, svc->audio_pid);
        svc->pcr_pid = svc->video_pid;
    }

    if (svc->port <= 0) {
        fprintf(stderr, "Error: Invalid port in input\n");
        return -1;
    }

    return 0;
}

/*
 * Parse arguments
 */
static int parse_args(int argc, char *argv[]) {
    static struct option long_options[] = {
        {"input",       required_argument, 0, 'i'},
        {"output",      required_argument, 0, 'o'},
        {"stdout",      no_argument,       0, 'S'},
        {"detect-only", no_argument,       0, 'D'},
        {"debug",       no_argument,       0, 'd'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "i:o:Ddh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'i':
                if (g_ctx.service_count >= MAX_SERVICES) {
                    fprintf(stderr, "Error: Maximum %d services supported\n", MAX_SERVICES);
                    return -1;
                }
                if (parse_input(optarg, &g_ctx.services[g_ctx.service_count],
                               g_ctx.service_count) != 0) {
                    return -1;
                }
                g_ctx.service_count++;
                break;

            case 'o': {
                char *colon = strchr(optarg, ':');
                if (!colon) {
                    fprintf(stderr, "Error: Output must be HOST:PORT format\n");
                    return -1;
                }
                *colon = '\0';
                strncpy(g_ctx.udp_host, optarg, sizeof(g_ctx.udp_host) - 1);
                g_ctx.udp_port = atoi(colon + 1);
                break;
            }

            case 'S':
                g_ctx.use_stdout = TRUE;
                break;

            case 'D':
                g_ctx.detect_only = TRUE;
                break;

            case 'd':
                g_ctx.debug = TRUE;
                break;

            case 'h':
                print_help(argv[0]);
                exit(0);

            default:
                return -1;
        }
    }

    if (g_ctx.service_count == 0) {
        fprintf(stderr, "Error: At least one input (-i) is required\n");
        return -1;
    }

    if (!g_ctx.use_stdout && g_ctx.udp_host[0] == '\0') {
        fprintf(stderr, "Error: Either --output or --stdout is required\n");
        return -1;
    }

    return 0;
}

/*
 * Cleanup
 */
static void cleanup(void) {
    if (g_ctx.pipeline) {
        gst_element_set_state(g_ctx.pipeline, GST_STATE_NULL);
        gst_object_unref(g_ctx.pipeline);
        g_ctx.pipeline = NULL;
    }

    if (g_ctx.main_loop) {
        g_main_loop_unref(g_ctx.main_loop);
        g_ctx.main_loop = NULL;
    }
}

/*
 * Main
 */
int main(int argc, char *argv[]) {
    int ret = 0;

    /* Parse arguments first (before gst_init modifies argc/argv) */
    if (parse_args(argc, argv) != 0) {
        print_help(argv[0]);
        return 1;
    }

    /* Set up signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    g_ctx.running = 1;

    /* Detect streams for all inputs */
    fprintf(stderr, "Detecting streams...\n");
    for (int i = 0; i < g_ctx.service_count; i++) {
        ServiceInput *svc = &g_ctx.services[i];
        fprintf(stderr, "Input %d: %s:%d\n", i + 1, svc->address, svc->port);

        if (detect_stream(svc) != 0) {
            fprintf(stderr, "Warning: Could not detect streams for %s:%d\n",
                    svc->address, svc->port);
            /* Default to H.264 + AAC */
            svc->video_type = STREAM_TYPE_H264;
            svc->audio_type = STREAM_TYPE_AAC;
        }
    }

    if (g_ctx.detect_only) {
        printf("{\n  \"services\": [\n");
        for (int i = 0; i < g_ctx.service_count; i++) {
            ServiceInput *svc = &g_ctx.services[i];
            printf("    {\"address\": \"%s\", \"port\": %d, \"program\": %d, "
                   "\"video_pid\": %d, \"audio_pid\": %d, \"pcr_pid\": %d, "
                   "\"video_type\": %d, \"audio_type\": %d}%s\n",
                   svc->address, svc->port, svc->program_number,
                   svc->video_pid, svc->audio_pid, svc->pcr_pid,
                   svc->video_type, svc->audio_type,
                   (i < g_ctx.service_count - 1) ? "," : "");
        }
        printf("  ]\n}\n");
        return 0;
    }

    /* Initialize GStreamer */
    gst_init(&argc, &argv);

    /* Create main loop */
    g_ctx.main_loop = g_main_loop_new(NULL, FALSE);

    /* Create pipeline */
    if (create_pipeline() != 0) {
        cleanup();
        return 1;
    }

    /* Start pipeline */
    fprintf(stderr, "Starting muxer...\n");
    GstStateChangeReturn state_ret = gst_element_set_state(g_ctx.pipeline, GST_STATE_PLAYING);
    if (state_ret == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "Error: Failed to start pipeline\n");
        cleanup();
        return 1;
    }

    if (g_ctx.use_stdout) {
        fprintf(stderr, "Muxing to stdout. Press Ctrl+C to stop.\n");
    } else {
        fprintf(stderr, "Muxing to %s:%d. Press Ctrl+C to stop.\n",
                g_ctx.udp_host, g_ctx.udp_port);
    }

    /* Run main loop */
    g_main_loop_run(g_ctx.main_loop);

    /* Cleanup */
    cleanup();

    return ret;
}
