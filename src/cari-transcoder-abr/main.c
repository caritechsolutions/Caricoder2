/*
 * CariTranscoder ABR - Multi-Bitrate MPEG-TS Transcoder
 * Copyright (c) 2024 CariTech Solutions
 *
 * Creates a single program MPEG-TS with multiple video streams
 * (different resolutions/bitrates) and one audio stream for
 * Adaptive Bitrate streaming.
 *
 * Usage:
 *   cari-transcoder-abr \
 *     --input 239.100.0.1:10000 \
 *     --variant "1920x1080:5000000:100" \
 *     --variant "1280x720:3000000:200" \
 *     --variant "854x480:1000000:300" \
 *     --audio-pid 50 \
 *     --program 1 \
 *     --output 239.100.0.101:5000
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>
#include <gst/gst.h>

#define MAX_VARIANTS 8
#define DEFAULT_KEYFRAME_INTERVAL 60
#define DEFAULT_AUDIO_BITRATE 128000
#define DEFAULT_AUDIO_PID 50
#define DEFAULT_PROGRAM 1
#define DEFAULT_PRESET "superfast"

/* Video codec types */
typedef enum {
    VIDEO_CODEC_H264,
    VIDEO_CODEC_H265,
    VIDEO_CODEC_MPEG2
} VideoCodec;

/* Audio codec types */
typedef enum {
    AUDIO_CODEC_AAC,
    AUDIO_CODEC_AC3,
    AUDIO_CODEC_MP2
} AudioCodec;

/* Variant configuration */
typedef struct {
    int width;
    int height;
    int video_bitrate;      /* bits/second */
    int video_pid;
} Variant;

/* Detected stream info */
typedef struct {
    int video_detected;
    int audio_detected;
    char video_codec[32];
    char audio_codec[32];
    int width;
    int height;
    int audio_channels;
    int audio_samplerate;
} StreamInfo;

/* Application context */
typedef struct {
    /* Input settings */
    char input_address[256];
    int input_port;

    /* Variant settings */
    Variant variants[MAX_VARIANTS];
    int num_variants;

    /* Video settings (common to all variants) */
    VideoCodec video_codec;
    char video_preset[32];
    int keyframe_interval;

    /* Audio settings */
    AudioCodec audio_codec;
    int audio_bitrate;
    int audio_pid;

    /* Program number (single program for all streams) */
    int program_number;

    /* Output settings */
    char output_host[256];
    int output_port;

    /* General settings */
    int debug;
    volatile int running;

    /* Detected stream info */
    StreamInfo stream_info;

    /* GStreamer */
    GstElement *pipeline;
    GMainLoop *main_loop;

    /* Threading */
    pthread_mutex_t lock;
} AppContext;

/* Global context */
static AppContext g_ctx;

/* Forward declarations */
static void signal_handler(int signum);
static void print_help(const char *prog);
static int parse_args(int argc, char *argv[]);
static int detect_stream(void);
static char *build_pipeline(void);
static int run_pipeline(void);

/*
 * Signal handler for clean shutdown
 */
static void signal_handler(int signum) {
    fprintf(stderr, "\nReceived signal %d, shutting down...\n", signum);
    g_ctx.running = 0;
    if (g_ctx.main_loop) {
        g_main_loop_quit(g_ctx.main_loop);
    }
}

/*
 * Print help message
 */
static void print_help(const char *prog) {
    printf("CariTranscoder ABR - Multi-Bitrate MPEG-TS Transcoder\n");
    printf("Usage: %s [OPTIONS]\n\n", prog);
    printf("Required Options:\n");
    printf("  -i, --input ADDRESS:PORT     Input multicast address and port\n");
    printf("  -v, --variant SPEC           Variant specification (can be repeated)\n");
    printf("                               Format: WIDTHxHEIGHT:BITRATE:VIDEO_PID\n");
    printf("                               Example: 1920x1080:5000000:100\n");
    printf("  -o, --output ADDRESS:PORT    Output multicast address and port\n");
    printf("\nOptional Options:\n");
    printf("  --program N                  Program number for all streams (default: 1)\n");
    printf("  --audio-pid PID              Audio PID (default: 50)\n");
    printf("  --video-codec CODEC          Video codec: h264 (default), h265, mpeg2\n");
    printf("  --video-preset PRESET        Encoder preset (default: superfast)\n");
    printf("  --keyframe-interval N        Keyframe interval in frames (default: 60)\n");
    printf("  --audio-codec CODEC          Audio codec: aac (default), ac3, mp2\n");
    printf("  --audio-bitrate RATE         Audio bitrate in bits/sec (default: 128000)\n");
    printf("  -d, --debug                  Enable debug output\n");
    printf("  -h, --help                   Show this help message\n");
    printf("\nExample:\n");
    printf("  %s \\\n", prog);
    printf("    --input 239.100.0.1:10000 \\\n");
    printf("    --variant 1920x1080:5000000:100 \\\n");
    printf("    --variant 1280x720:3000000:200 \\\n");
    printf("    --variant 854x480:1000000:300 \\\n");
    printf("    --audio-pid 50 \\\n");
    printf("    --output 239.100.0.101:5000\n");
    printf("\n  This creates a single program with:\n");
    printf("    - Video PID 100: 1080p @ 5000 kbps\n");
    printf("    - Video PID 200: 720p @ 3000 kbps\n");
    printf("    - Video PID 300: 480p @ 1000 kbps\n");
    printf("    - Audio PID 50: shared audio track\n");
}

/*
 * Parse variant specification: WIDTHxHEIGHT:BITRATE:VIDEO_PID
 */
static int parse_variant(const char *spec, Variant *v) {
    int width, height, bitrate, pid;
    if (sscanf(spec, "%dx%d:%d:%d", &width, &height, &bitrate, &pid) != 4) {
        fprintf(stderr, "Error: Invalid variant format: %s\n", spec);
        fprintf(stderr, "Expected: WIDTHxHEIGHT:BITRATE:VIDEO_PID\n");
        return -1;
    }
    /* MPEG-TS PIDs must be >= 64 (0x40) */
    if (pid < 64) {
        fprintf(stderr, "Error: VIDEO_PID must be >= 64 (MPEG-TS requirement), got %d\n", pid);
        return -1;
    }
    v->width = width;
    v->height = height;
    v->video_bitrate = bitrate;
    v->video_pid = pid;
    return 0;
}

/*
 * Parse video codec string
 */
static VideoCodec parse_video_codec(const char *str) {
    if (strcasecmp(str, "h265") == 0 || strcasecmp(str, "hevc") == 0) {
        return VIDEO_CODEC_H265;
    } else if (strcasecmp(str, "mpeg2") == 0) {
        return VIDEO_CODEC_MPEG2;
    }
    return VIDEO_CODEC_H264;
}

/*
 * Parse audio codec string
 */
static AudioCodec parse_audio_codec(const char *str) {
    if (strcasecmp(str, "ac3") == 0) {
        return AUDIO_CODEC_AC3;
    } else if (strcasecmp(str, "mp2") == 0) {
        return AUDIO_CODEC_MP2;
    }
    return AUDIO_CODEC_AAC;
}

/*
 * Initialize context with defaults
 */
static void init_context(void) {
    memset(&g_ctx, 0, sizeof(g_ctx));

    g_ctx.video_codec = VIDEO_CODEC_H264;
    strcpy(g_ctx.video_preset, DEFAULT_PRESET);
    g_ctx.keyframe_interval = DEFAULT_KEYFRAME_INTERVAL;

    g_ctx.audio_codec = AUDIO_CODEC_AAC;
    g_ctx.audio_bitrate = DEFAULT_AUDIO_BITRATE;
    g_ctx.audio_pid = DEFAULT_AUDIO_PID;
    g_ctx.program_number = DEFAULT_PROGRAM;

    g_ctx.running = 1;
    pthread_mutex_init(&g_ctx.lock, NULL);
}

/*
 * Parse command line arguments
 */
static int parse_args(int argc, char *argv[]) {
    enum {
        OPT_PROGRAM = 1000,
        OPT_VIDEO_CODEC,
        OPT_VIDEO_PRESET,
        OPT_KEYFRAME_INTERVAL,
        OPT_AUDIO_CODEC,
        OPT_AUDIO_BITRATE,
        OPT_AUDIO_PID
    };

    static struct option long_options[] = {
        {"input",              required_argument, 0, 'i'},
        {"variant",            required_argument, 0, 'v'},
        {"output",             required_argument, 0, 'o'},
        {"program",            required_argument, 0, OPT_PROGRAM},
        {"video-codec",        required_argument, 0, OPT_VIDEO_CODEC},
        {"video-preset",       required_argument, 0, OPT_VIDEO_PRESET},
        {"keyframe-interval",  required_argument, 0, OPT_KEYFRAME_INTERVAL},
        {"audio-codec",        required_argument, 0, OPT_AUDIO_CODEC},
        {"audio-bitrate",      required_argument, 0, OPT_AUDIO_BITRATE},
        {"audio-pid",          required_argument, 0, OPT_AUDIO_PID},
        {"debug",              no_argument,       0, 'd'},
        {"help",               no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    char *colon;

    while ((opt = getopt_long(argc, argv, "i:v:o:dh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'i':
                colon = strrchr(optarg, ':');
                if (!colon) {
                    fprintf(stderr, "Error: Invalid input format. Use ADDRESS:PORT\n");
                    return -1;
                }
                *colon = '\0';
                strncpy(g_ctx.input_address, optarg, sizeof(g_ctx.input_address) - 1);
                g_ctx.input_port = atoi(colon + 1);
                break;

            case 'v':
                if (g_ctx.num_variants >= MAX_VARIANTS) {
                    fprintf(stderr, "Error: Maximum %d variants supported\n", MAX_VARIANTS);
                    return -1;
                }
                if (parse_variant(optarg, &g_ctx.variants[g_ctx.num_variants]) < 0) {
                    return -1;
                }
                g_ctx.num_variants++;
                break;

            case 'o':
                colon = strrchr(optarg, ':');
                if (!colon) {
                    fprintf(stderr, "Error: Invalid output format. Use ADDRESS:PORT\n");
                    return -1;
                }
                *colon = '\0';
                strncpy(g_ctx.output_host, optarg, sizeof(g_ctx.output_host) - 1);
                g_ctx.output_port = atoi(colon + 1);
                break;

            case OPT_PROGRAM:
                g_ctx.program_number = atoi(optarg);
                break;

            case OPT_VIDEO_CODEC:
                g_ctx.video_codec = parse_video_codec(optarg);
                break;

            case OPT_VIDEO_PRESET:
                strncpy(g_ctx.video_preset, optarg, sizeof(g_ctx.video_preset) - 1);
                break;

            case OPT_KEYFRAME_INTERVAL:
                g_ctx.keyframe_interval = atoi(optarg);
                break;

            case OPT_AUDIO_CODEC:
                g_ctx.audio_codec = parse_audio_codec(optarg);
                break;

            case OPT_AUDIO_BITRATE:
                g_ctx.audio_bitrate = atoi(optarg);
                break;

            case OPT_AUDIO_PID:
                g_ctx.audio_pid = atoi(optarg);
                if (g_ctx.audio_pid < 64) {
                    fprintf(stderr, "Error: AUDIO_PID must be >= 64 (MPEG-TS requirement), got %d\n", g_ctx.audio_pid);
                    return -1;
                }
                break;

            case 'd':
                g_ctx.debug = 1;
                break;

            case 'h':
                print_help(argv[0]);
                exit(0);

            default:
                return -1;
        }
    }

    /* Validate required arguments */
    if (g_ctx.input_port == 0) {
        fprintf(stderr, "Error: --input is required\n");
        return -1;
    }
    if (g_ctx.num_variants == 0) {
        fprintf(stderr, "Error: At least one --variant is required\n");
        return -1;
    }
    if (g_ctx.output_port == 0) {
        fprintf(stderr, "Error: --output is required\n");
        return -1;
    }

    return 0;
}

/*
 * Get video parser for input codec
 */
static const char *get_video_parser(const char *codec) {
    if (strstr(codec, "h264") || strstr(codec, "avc")) return "h264parse";
    if (strstr(codec, "h265") || strstr(codec, "hevc")) return "h265parse";
    if (strstr(codec, "mpeg2")) return "mpegvideoparse";
    return NULL;
}

/*
 * Get video decoder for input codec
 */
static const char *get_video_decoder(const char *codec) {
    if (strstr(codec, "h264") || strstr(codec, "avc")) return "avdec_h264";
    if (strstr(codec, "h265") || strstr(codec, "hevc")) return "avdec_h265";
    if (strstr(codec, "mpeg2")) return "avdec_mpeg2video";
    return NULL;
}

/*
 * Get audio parser for input codec
 */
static const char *get_audio_parser(const char *codec) {
    if (strstr(codec, "aac")) return "aacparse";
    if (strstr(codec, "mp3")) return "mpegaudioparse";
    if (strstr(codec, "mp2") || strstr(codec, "mp1")) return "mpegaudioparse";
    if (strstr(codec, "ac3") || strstr(codec, "ac-3")) return "ac3parse";
    if (strstr(codec, "eac3") || strstr(codec, "e-ac-3")) return "ac3parse";
    return "aacparse";
}

/*
 * Get audio decoder for input codec
 */
static const char *get_audio_decoder(const char *codec) {
    if (strstr(codec, "aac")) return "avdec_aac";
    if (strstr(codec, "mp3")) return "avdec_mp3";
    if (strstr(codec, "mp2") || strstr(codec, "mp1")) return "avdec_mp2float";
    if (strstr(codec, "ac3") || strstr(codec, "ac-3")) return "avdec_ac3";
    if (strstr(codec, "eac3") || strstr(codec, "e-ac-3")) return "avdec_eac3";
    return "avdec_aac";
}

/*
 * Detect input stream using ffprobe
 */
static int detect_stream(void) {
    char cmd[512];
    FILE *fp;
    char line[256];

    printf("Step 1: Detecting input stream...\n");

    snprintf(cmd, sizeof(cmd),
        "timeout 10 ffprobe -v error -select_streams v:0 "
        "-show_entries stream=codec_name,width,height "
        "-of csv=p=0 udp://%s:%d 2>/dev/null",
        g_ctx.input_address, g_ctx.input_port);

    fp = popen(cmd, "r");
    if (fp && fgets(line, sizeof(line), fp)) {
        char codec[32];
        int w, h;
        if (sscanf(line, "%31[^,],%d,%d", codec, &w, &h) >= 1) {
            g_ctx.stream_info.video_detected = 1;
            snprintf(g_ctx.stream_info.video_codec, sizeof(g_ctx.stream_info.video_codec), "%s", codec);
            g_ctx.stream_info.width = w;
            g_ctx.stream_info.height = h;
            printf("  Video detected: %s %dx%d\n", codec, w, h);
        }
    }
    if (fp) pclose(fp);

    /* Detect audio */
    snprintf(cmd, sizeof(cmd),
        "timeout 10 ffprobe -v error -select_streams a:0 "
        "-show_entries stream=codec_name,channels,sample_rate "
        "-of csv=p=0 udp://%s:%d 2>/dev/null",
        g_ctx.input_address, g_ctx.input_port);

    fp = popen(cmd, "r");
    if (fp && fgets(line, sizeof(line), fp)) {
        char codec[32];
        int ch = 2, sr = 48000;
        if (sscanf(line, "%31[^,],%d,%d", codec, &ch, &sr) >= 1) {
            g_ctx.stream_info.audio_detected = 1;
            snprintf(g_ctx.stream_info.audio_codec, sizeof(g_ctx.stream_info.audio_codec), "%s", codec);
            g_ctx.stream_info.audio_channels = ch;
            g_ctx.stream_info.audio_samplerate = sr;
            printf("  Audio detected: %s %d ch @ %d Hz\n", codec, ch, sr);
        }
    }
    if (fp) pclose(fp);

    if (!g_ctx.stream_info.video_detected) {
        fprintf(stderr, "Error: No video stream detected\n");
        return -1;
    }

    return 0;
}

/*
 * Build the GStreamer pipeline string
 */
static char *build_pipeline(void) {
    static char pipeline[16384];
    char *p = pipeline;
    int remaining = sizeof(pipeline);
    int n, i;

    const char *video_parser = get_video_parser(g_ctx.stream_info.video_codec);
    const char *video_decoder = get_video_decoder(g_ctx.stream_info.video_codec);
    const char *audio_parser = get_audio_parser(g_ctx.stream_info.audio_codec);
    const char *audio_decoder = get_audio_decoder(g_ctx.stream_info.audio_codec);

    if (!video_parser || !video_decoder) {
        fprintf(stderr, "Error: Unsupported video codec: %s\n", g_ctx.stream_info.video_codec);
        return NULL;
    }

    /* Input: udpsrc -> tsparse -> tsdemux */
    n = snprintf(p, remaining,
        "udpsrc uri=udp://%s:%d do-timestamp=false buffer-size=4194304 ! "
        "tsparse set-timestamps=true ! tsdemux name=demux ",
        g_ctx.input_address, g_ctx.input_port);
    p += n; remaining -= n;

    /* Video branch: demux -> parse -> decode -> tee */
    n = snprintf(p, remaining,
        "demux. ! queue max-size-buffers=100 ! %s ! queue ! %s ! "
        "videoconvert ! videorate ! tee name=videotee ",
        video_parser, video_decoder);
    p += n; remaining -= n;

    /* Create encoder branch for each variant */
    for (i = 0; i < g_ctx.num_variants; i++) {
        Variant *v = &g_ctx.variants[i];

        n = snprintf(p, remaining,
            "videotee. ! queue max-size-buffers=100 ! "
            "videoscale ! video/x-raw,width=%d,height=%d ! ",
            v->width, v->height);
        p += n; remaining -= n;

        /* Encoder based on codec */
        switch (g_ctx.video_codec) {
            case VIDEO_CODEC_H264:
                n = snprintf(p, remaining,
                    "x264enc bitrate=%d speed-preset=%s key-int-max=%d "
                    "bframes=0 tune=zerolatency ! "
                    "h264parse config-interval=1 ! queue ! mux.sink_%d ",
                    v->video_bitrate / 1000,
                    g_ctx.video_preset,
                    g_ctx.keyframe_interval,
                    v->video_pid);
                break;

            case VIDEO_CODEC_H265:
                n = snprintf(p, remaining,
                    "x265enc bitrate=%d speed-preset=%s key-int-max=%d "
                    "tune=zerolatency ! "
                    "h265parse config-interval=1 ! queue ! mux.sink_%d ",
                    v->video_bitrate / 1000,
                    g_ctx.video_preset,
                    g_ctx.keyframe_interval,
                    v->video_pid);
                break;

            case VIDEO_CODEC_MPEG2:
                n = snprintf(p, remaining,
                    "avenc_mpeg2video bitrate=%d maxrate=%d gop-size=%d ! "
                    "queue ! mux.sink_%d ",
                    v->video_bitrate,
                    v->video_bitrate,
                    g_ctx.keyframe_interval,
                    v->video_pid);
                break;
        }
        p += n; remaining -= n;
    }

    /* Audio branch: demux -> parse -> decode -> encode -> mux */
    if (g_ctx.stream_info.audio_detected && audio_parser && audio_decoder) {
        n = snprintf(p, remaining,
            "demux. ! queue max-size-buffers=100 ! %s ! %s ! "
            "audioconvert ! audioresample ! ",
            audio_parser, audio_decoder);
        p += n; remaining -= n;

        /* Audio encoder */
        switch (g_ctx.audio_codec) {
            case AUDIO_CODEC_AAC:
                n = snprintf(p, remaining,
                    "fdkaacenc bitrate=%d ! queue ! mux.sink_%d ",
                    g_ctx.audio_bitrate, g_ctx.audio_pid);
                break;
            case AUDIO_CODEC_AC3:
                n = snprintf(p, remaining,
                    "avenc_ac3 bitrate=%d ! queue ! mux.sink_%d ",
                    g_ctx.audio_bitrate, g_ctx.audio_pid);
                break;
            case AUDIO_CODEC_MP2:
                n = snprintf(p, remaining,
                    "avenc_mp2 bitrate=%d ! queue ! mux.sink_%d ",
                    g_ctx.audio_bitrate, g_ctx.audio_pid);
                break;
        }
        p += n; remaining -= n;
    }

    /* Muxer with prog-map - all streams in ONE program */
    /* Calculate total bitrate */
    int total_video_bitrate = 0;
    for (i = 0; i < g_ctx.num_variants; i++) {
        total_video_bitrate += g_ctx.variants[i].video_bitrate;
    }
    int mux_bitrate = (total_video_bitrate + g_ctx.audio_bitrate) * 103 / 100;

    /* Build prog-map string: all sinks assigned to same program */
    char prog_map[1024];
    char *pm = prog_map;
    int pm_remaining = sizeof(prog_map);

    pm += snprintf(pm, pm_remaining, "program_map");
    pm_remaining = sizeof(prog_map) - (pm - prog_map);

    /* Add all video sinks to the program */
    for (i = 0; i < g_ctx.num_variants; i++) {
        Variant *v = &g_ctx.variants[i];
        int len = snprintf(pm, pm_remaining, ",sink_%d=%d",
            v->video_pid, g_ctx.program_number);
        pm += len;
        pm_remaining -= len;
    }

    /* Add audio sink to the program */
    if (g_ctx.stream_info.audio_detected) {
        int len = snprintf(pm, pm_remaining, ",sink_%d=%d",
            g_ctx.audio_pid, g_ctx.program_number);
        pm += len;
        pm_remaining -= len;
    }

    n = snprintf(p, remaining,
        "mpegtsmux name=mux bitrate=%d prog-map=\"%s\" ! "
        "queue ! udpsink host=%s port=%d sync=true async=false",
        mux_bitrate, prog_map, g_ctx.output_host, g_ctx.output_port);
    p += n;

    return pipeline;
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
            fprintf(stderr, "Pipeline error: %s\n", err->message);
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
            fprintf(stderr, "Warning: %s\n", err->message);
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
        default:
            break;
    }
    return TRUE;
}

/*
 * Run the pipeline
 */
static int run_pipeline(void) {
    GError *error = NULL;
    GstBus *bus;
    char *pipeline_str;

    printf("Step 2: Building pipeline...\n");

    pipeline_str = build_pipeline();
    if (!pipeline_str) {
        return -1;
    }

    printf("\n=== Pipeline ===\n%s\n================\n\n", pipeline_str);

    printf("Step 3: Starting transcoder...\n");

    g_ctx.pipeline = gst_parse_launch(pipeline_str, &error);
    if (error) {
        fprintf(stderr, "Pipeline error: %s\n", error->message);
        g_error_free(error);
        return -1;
    }

    bus = gst_element_get_bus(g_ctx.pipeline);
    gst_bus_add_watch(bus, on_bus_message, NULL);
    gst_object_unref(bus);

    gst_element_set_state(g_ctx.pipeline, GST_STATE_PLAYING);

    printf("Transcoding to udp://%s:%d\n",
        g_ctx.output_host, g_ctx.output_port);
    printf("Press Ctrl+C to stop.\n\n");

    /* Print program info */
    printf("Program %d:\n", g_ctx.program_number);
    for (int i = 0; i < g_ctx.num_variants; i++) {
        Variant *v = &g_ctx.variants[i];
        printf("  Video PID %d: %dx%d @ %d kbps\n",
            v->video_pid, v->width, v->height, v->video_bitrate / 1000);
    }
    if (g_ctx.stream_info.audio_detected) {
        printf("  Audio PID %d: %d kbps\n",
            g_ctx.audio_pid, g_ctx.audio_bitrate / 1000);
    }

    g_ctx.main_loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(g_ctx.main_loop);

    /* Cleanup */
    gst_element_set_state(g_ctx.pipeline, GST_STATE_NULL);
    gst_object_unref(g_ctx.pipeline);
    g_main_loop_unref(g_ctx.main_loop);

    return 0;
}

/*
 * Main entry point
 */
int main(int argc, char *argv[]) {
    /* Initialize */
    init_context();

    /* Parse arguments */
    if (parse_args(argc, argv) < 0) {
        print_help(argv[0]);
        return 1;
    }

    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Initialize GStreamer */
    gst_init(&argc, &argv);

    /* Detect input stream */
    if (detect_stream() < 0) {
        return 1;
    }

    /* Run pipeline */
    if (run_pipeline() < 0) {
        return 1;
    }

    printf("Shutdown complete.\n");
    return 0;
}
