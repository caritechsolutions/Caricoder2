/*
 * CariTranscoder - GStreamer-based Video/Audio Transcoder
 *
 * Receives MPEG-TS via UDP, transcodes video/audio using GStreamer,
 * and outputs to stdout for piping to tsp.
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
#include <sys/time.h>
#include <gst/gst.h>

/* Version */
#define VERSION "1.0.0"

/* Defaults */
#define DEFAULT_API_PORT 9200
#define DEFAULT_VIDEO_BITRATE 5000000
#define DEFAULT_AUDIO_BITRATE 128000
#define DEFAULT_KEYFRAME_INTERVAL 60
#define DEFAULT_BFRAMES 2
#define DEFAULT_AUDIO_SAMPLERATE 48000

/* Codec types */
typedef enum {
    VIDEO_CODEC_UNKNOWN = 0,
    VIDEO_CODEC_H264,
    VIDEO_CODEC_H265,
    VIDEO_CODEC_MPEG2
} VideoCodec;

typedef enum {
    AUDIO_CODEC_UNKNOWN = 0,
    AUDIO_CODEC_AAC,
    AUDIO_CODEC_AC3,
    AUDIO_CODEC_EAC3,
    AUDIO_CODEC_MP2
} AudioCodec;

/* Processing modes */
typedef enum {
    MODE_TRANSCODE = 0,
    MODE_PASSTHROUGH,
    MODE_DROP
} ProcessingMode;

/* Video presets */
typedef enum {
    PRESET_ULTRAFAST = 0,
    PRESET_SUPERFAST,
    PRESET_VERYFAST,
    PRESET_FASTER,
    PRESET_FAST,
    PRESET_MEDIUM,
    PRESET_SLOW,
    PRESET_SLOWER,
    PRESET_VERYSLOW
} VideoPreset;

/* Detected stream information */
typedef struct {
    /* Video */
    VideoCodec video_codec;
    int video_width;
    int video_height;
    int video_fps_num;
    int video_fps_den;
    gboolean video_interlaced;
    char video_profile[32];

    /* Audio */
    AudioCodec audio_codec;
    int audio_channels;
    int audio_sample_rate;

    /* Detection state */
    gboolean video_detected;
    gboolean audio_detected;
} StreamInfo;

/* Application context */
typedef struct {
    /* Input settings */
    char input_address[64];
    int input_port;
    char input_interface[32];

    /* Video output settings */
    ProcessingMode video_mode;
    VideoCodec video_out_codec;
    int video_bitrate;
    VideoPreset video_preset;
    char video_profile[32];
    int keyframe_interval;
    int bframes;

    /* Scaling settings */
    int scale_width;
    int scale_height;
    gboolean deinterlace;
    int out_fps_num;
    int out_fps_den;

    /* Audio output settings */
    ProcessingMode audio_mode;
    AudioCodec audio_out_codec;
    int audio_bitrate;
    int audio_channels;  /* 0=passthrough, 1=mono, 2=stereo, 6=5.1 */
    int audio_samplerate;

    /* General settings */
    int api_port;
    char log_file[256];
    gboolean debug;
    gboolean detect_only;
    gboolean dump_pipeline;

    /* Runtime state */
    volatile int running;
    StreamInfo stream_info;

    /* GStreamer */
    GstElement *pipeline;
    GMainLoop *main_loop;

    /* Threading */
    pthread_mutex_t lock;

    /* Statistics */
    uint64_t packets_in;
    uint64_t packets_out;
    uint64_t bytes_in;
    uint64_t bytes_out;
    uint64_t frames_encoded;
    struct timeval start_time;
} AppContext;

/* Global context */
static AppContext g_ctx;

/* Forward declarations */
static void signal_handler(int signum);
static void print_help(const char *prog);
static int parse_args(int argc, char *argv[]);
static const char *video_codec_to_string(VideoCodec codec);
static const char *audio_codec_to_string(AudioCodec codec);
static VideoCodec parse_video_codec(const char *str);
static AudioCodec parse_audio_codec(const char *str);
static ProcessingMode parse_mode(const char *str);
static VideoPreset parse_preset(const char *str);
static const char *preset_to_string(VideoPreset preset);

/* Detection pipeline functions */
static int create_detection_pipeline(void);
static void on_demux_pad_added(GstElement *element, GstPad *pad, gpointer data);
static gboolean on_bus_message(GstBus *bus, GstMessage *msg, gpointer data);
static void print_detected_info(void);
static GstPadProbeReturn on_caps_probe(GstPad *pad, GstPadProbeInfo *info, gpointer data);

/* FFprobe-based detection */
static int detect_stream_with_ffprobe(void);

/* Full transcoding pipeline functions */
static int create_transcode_pipeline(void);

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
 * Print help message
 */
static void print_help(const char *prog) {
    printf("CariTranscoder v%s - GStreamer Video/Audio Transcoder\n\n", VERSION);
    printf("Usage: %s --input ADDRESS:PORT [options]\n\n", prog);

    printf("INPUT OPTIONS:\n");
    printf("  --input ADDRESS:PORT       UDP multicast input (required)\n");
    printf("  --input-interface IFACE    Network interface for multicast\n");
    printf("\n");

    printf("VIDEO OPTIONS:\n");
    printf("  --video-mode MODE          transcode|passthrough|drop (default: transcode)\n");
    printf("  --video-codec CODEC        h264|h265|mpeg2 (output codec, default: h264)\n");
    printf("  --video-bitrate BPS        Target bitrate in bps (default: 5000000)\n");
    printf("  --video-preset PRESET      ultrafast|superfast|veryfast|faster|fast|\n");
    printf("                             medium|slow|slower|veryslow (default: medium)\n");
    printf("  --video-profile PROFILE    baseline|main|high for H.264 (default: main)\n");
    printf("  --keyframe-interval FRAMES GOP size in frames (default: 60)\n");
    printf("  --bframes COUNT            Number of B-frames (default: 2)\n");
    printf("\n");

    printf("SCALING OPTIONS:\n");
    printf("  --scale WIDTHxHEIGHT       Output resolution (e.g., 1280x720)\n");
    printf("  --deinterlace              Enable deinterlacing\n");
    printf("  --fps NUM/DEN              Output framerate (e.g., 30/1 or 30000/1001)\n");
    printf("\n");

    printf("AUDIO OPTIONS:\n");
    printf("  --audio-mode MODE          transcode|passthrough|drop (default: transcode)\n");
    printf("  --audio-codec CODEC        aac|ac3|mp2 (default: aac)\n");
    printf("  --audio-bitrate BPS        Audio bitrate in bps (default: 128000)\n");
    printf("  --audio-channels MODE      1|2|6|passthrough (default: 2/stereo)\n");
    printf("  --audio-samplerate HZ      Sample rate (default: 48000)\n");
    printf("\n");

    printf("GENERAL OPTIONS:\n");
    printf("  --api-port PORT            REST API port for stats (default: 9200)\n");
    printf("  --log-file PATH            Log file path\n");
    printf("  --debug                    Enable debug logging\n");
    printf("  --detect-only              Detect stream info and exit (JSON output)\n");
    printf("  --dump-pipeline            Print pipeline graph and exit\n");
    printf("  --help                     Show this help\n");
    printf("\n");

    printf("EXAMPLES:\n");
    printf("  Detect stream format:\n");
    printf("    %s --input 239.100.0.1:5000 --detect-only\n\n", prog);

    printf("  Transcode H.264 to H.265 at 5 Mbps:\n");
    printf("    %s --input 239.100.0.1:5000 --video-codec h265 --video-bitrate 5000000 \\\n", prog);
    printf("        | tsp -I file - -P regulate --bitrate 6000000 -O ip 239.100.0.2:5000\n\n");

    printf("  Scale to 720p with deinterlacing:\n");
    printf("    %s --input 239.100.0.1:5000 --scale 1280x720 --deinterlace \\\n", prog);
    printf("        --video-bitrate 3000000 | tsp ...\n\n");

    printf("  Passthrough video, transcode audio to AAC:\n");
    printf("    %s --input 239.100.0.1:5000 --video-mode passthrough \\\n", prog);
    printf("        --audio-codec aac --audio-bitrate 128000 | tsp ...\n");
}

/*
 * Initialize context with defaults
 */
static void init_context(void) {
    memset(&g_ctx, 0, sizeof(g_ctx));

    /* Input defaults */
    g_ctx.input_port = 0;

    /* Video defaults */
    g_ctx.video_mode = MODE_TRANSCODE;
    g_ctx.video_out_codec = VIDEO_CODEC_H264;
    g_ctx.video_bitrate = DEFAULT_VIDEO_BITRATE;
    g_ctx.video_preset = PRESET_MEDIUM;
    strcpy(g_ctx.video_profile, "main");
    g_ctx.keyframe_interval = DEFAULT_KEYFRAME_INTERVAL;
    g_ctx.bframes = DEFAULT_BFRAMES;

    /* Scaling defaults (0 = no scaling) */
    g_ctx.scale_width = 0;
    g_ctx.scale_height = 0;
    g_ctx.deinterlace = FALSE;
    g_ctx.out_fps_num = 0;
    g_ctx.out_fps_den = 1;

    /* Audio defaults */
    g_ctx.audio_mode = MODE_TRANSCODE;
    g_ctx.audio_out_codec = AUDIO_CODEC_AAC;
    g_ctx.audio_bitrate = DEFAULT_AUDIO_BITRATE;
    g_ctx.audio_channels = 2;  /* Stereo */
    g_ctx.audio_samplerate = DEFAULT_AUDIO_SAMPLERATE;

    /* General defaults */
    g_ctx.api_port = DEFAULT_API_PORT;
    g_ctx.debug = FALSE;
    g_ctx.detect_only = FALSE;
    g_ctx.dump_pipeline = FALSE;
    g_ctx.running = 1;

    pthread_mutex_init(&g_ctx.lock, NULL);
}

/*
 * Parse command line arguments
 */
static int parse_args(int argc, char *argv[]) {
    static struct option long_options[] = {
        /* Input */
        {"input",              required_argument, 0, 'i'},
        {"input-interface",    required_argument, 0, 'I'},

        /* Video */
        {"video-mode",         required_argument, 0, 'V'},
        {"video-codec",        required_argument, 0, 'c'},
        {"video-bitrate",      required_argument, 0, 'b'},
        {"video-preset",       required_argument, 0, 'p'},
        {"video-profile",      required_argument, 0, 'P'},
        {"keyframe-interval",  required_argument, 0, 'k'},
        {"bframes",            required_argument, 0, 'B'},

        /* Scaling */
        {"scale",              required_argument, 0, 's'},
        {"deinterlace",        no_argument,       0, 'D'},
        {"fps",                required_argument, 0, 'f'},

        /* Audio */
        {"audio-mode",         required_argument, 0, 'A'},
        {"audio-codec",        required_argument, 0, 'C'},
        {"audio-bitrate",      required_argument, 0, 'a'},
        {"audio-channels",     required_argument, 0, 'n'},
        {"audio-samplerate",   required_argument, 0, 'r'},

        /* General */
        {"api-port",           required_argument, 0, 'x'},
        {"log-file",           required_argument, 0, 'l'},
        {"debug",              no_argument,       0, 'd'},
        {"detect-only",        no_argument,       0, 'O'},
        {"dump-pipeline",      no_argument,       0, 'G'},
        {"help",               no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;
    char *colon;

    while ((opt = getopt_long(argc, argv, "i:I:V:c:b:p:P:k:B:s:Df:A:C:a:n:r:x:l:dOGh",
                              long_options, &option_index)) != -1) {
        switch (opt) {
            /* Input */
            case 'i':  /* --input */
                colon = strchr(optarg, ':');
                if (!colon) {
                    fprintf(stderr, "Error: Input must be in ADDRESS:PORT format\n");
                    return -1;
                }
                *colon = '\0';
                strncpy(g_ctx.input_address, optarg, sizeof(g_ctx.input_address) - 1);
                g_ctx.input_port = atoi(colon + 1);
                break;

            case 'I':  /* --input-interface */
                strncpy(g_ctx.input_interface, optarg, sizeof(g_ctx.input_interface) - 1);
                break;

            /* Video */
            case 'V':  /* --video-mode */
                g_ctx.video_mode = parse_mode(optarg);
                break;

            case 'c':  /* --video-codec */
                g_ctx.video_out_codec = parse_video_codec(optarg);
                if (g_ctx.video_out_codec == VIDEO_CODEC_UNKNOWN) {
                    fprintf(stderr, "Error: Unknown video codec '%s'\n", optarg);
                    return -1;
                }
                break;

            case 'b':  /* --video-bitrate */
                g_ctx.video_bitrate = atoi(optarg);
                break;

            case 'p':  /* --video-preset */
                g_ctx.video_preset = parse_preset(optarg);
                break;

            case 'P':  /* --video-profile */
                strncpy(g_ctx.video_profile, optarg, sizeof(g_ctx.video_profile) - 1);
                break;

            case 'k':  /* --keyframe-interval */
                g_ctx.keyframe_interval = atoi(optarg);
                break;

            case 'B':  /* --bframes */
                g_ctx.bframes = atoi(optarg);
                break;

            /* Scaling */
            case 's':  /* --scale WIDTHxHEIGHT */
                if (sscanf(optarg, "%dx%d", &g_ctx.scale_width, &g_ctx.scale_height) != 2) {
                    fprintf(stderr, "Error: Scale must be in WIDTHxHEIGHT format\n");
                    return -1;
                }
                break;

            case 'D':  /* --deinterlace */
                g_ctx.deinterlace = TRUE;
                break;

            case 'f':  /* --fps NUM/DEN */
                if (sscanf(optarg, "%d/%d", &g_ctx.out_fps_num, &g_ctx.out_fps_den) != 2) {
                    /* Try just a number */
                    g_ctx.out_fps_num = atoi(optarg);
                    g_ctx.out_fps_den = 1;
                }
                break;

            /* Audio */
            case 'A':  /* --audio-mode */
                g_ctx.audio_mode = parse_mode(optarg);
                break;

            case 'C':  /* --audio-codec */
                g_ctx.audio_out_codec = parse_audio_codec(optarg);
                if (g_ctx.audio_out_codec == AUDIO_CODEC_UNKNOWN) {
                    fprintf(stderr, "Error: Unknown audio codec '%s'\n", optarg);
                    return -1;
                }
                break;

            case 'a':  /* --audio-bitrate */
                g_ctx.audio_bitrate = atoi(optarg);
                break;

            case 'n':  /* --audio-channels */
                if (strcasecmp(optarg, "passthrough") == 0) {
                    g_ctx.audio_channels = 0;
                } else if (strcasecmp(optarg, "mono") == 0) {
                    g_ctx.audio_channels = 1;
                } else if (strcasecmp(optarg, "stereo") == 0) {
                    g_ctx.audio_channels = 2;
                } else if (strcasecmp(optarg, "5.1") == 0) {
                    g_ctx.audio_channels = 6;
                } else {
                    g_ctx.audio_channels = atoi(optarg);
                }
                break;

            case 'r':  /* --audio-samplerate */
                g_ctx.audio_samplerate = atoi(optarg);
                break;

            /* General */
            case 'x':  /* --api-port */
                g_ctx.api_port = atoi(optarg);
                break;

            case 'l':  /* --log-file */
                strncpy(g_ctx.log_file, optarg, sizeof(g_ctx.log_file) - 1);
                break;

            case 'd':  /* --debug */
                g_ctx.debug = TRUE;
                break;

            case 'O':  /* --detect-only */
                g_ctx.detect_only = TRUE;
                break;

            case 'G':  /* --dump-pipeline */
                g_ctx.dump_pipeline = TRUE;
                break;

            case 'h':  /* --help */
                print_help(argv[0]);
                exit(0);

            default:
                return -1;
        }
    }

    /* Validate required options */
    if (g_ctx.input_port == 0) {
        fprintf(stderr, "Error: --input ADDRESS:PORT is required\n");
        return -1;
    }

    return 0;
}

/*
 * Codec and mode conversion functions
 */
static const char *video_codec_to_string(VideoCodec codec) {
    switch (codec) {
        case VIDEO_CODEC_H264:  return "h264";
        case VIDEO_CODEC_H265:  return "h265";
        case VIDEO_CODEC_MPEG2: return "mpeg2";
        default:                return "unknown";
    }
}

static const char *audio_codec_to_string(AudioCodec codec) {
    switch (codec) {
        case AUDIO_CODEC_AAC:   return "aac";
        case AUDIO_CODEC_AC3:   return "ac3";
        case AUDIO_CODEC_EAC3:  return "eac3";
        case AUDIO_CODEC_MP2:   return "mp2";
        default:                return "unknown";
    }
}

static VideoCodec parse_video_codec(const char *str) {
    if (strcasecmp(str, "h264") == 0 || strcasecmp(str, "avc") == 0)
        return VIDEO_CODEC_H264;
    if (strcasecmp(str, "h265") == 0 || strcasecmp(str, "hevc") == 0)
        return VIDEO_CODEC_H265;
    if (strcasecmp(str, "mpeg2") == 0 || strcasecmp(str, "mpeg2video") == 0)
        return VIDEO_CODEC_MPEG2;
    return VIDEO_CODEC_UNKNOWN;
}

static AudioCodec parse_audio_codec(const char *str) {
    if (strcasecmp(str, "aac") == 0)
        return AUDIO_CODEC_AAC;
    if (strcasecmp(str, "ac3") == 0)
        return AUDIO_CODEC_AC3;
    if (strcasecmp(str, "eac3") == 0 || strcasecmp(str, "e-ac3") == 0)
        return AUDIO_CODEC_EAC3;
    if (strcasecmp(str, "mp2") == 0)
        return AUDIO_CODEC_MP2;
    return AUDIO_CODEC_UNKNOWN;
}

static ProcessingMode parse_mode(const char *str) {
    if (strcasecmp(str, "passthrough") == 0 || strcasecmp(str, "copy") == 0)
        return MODE_PASSTHROUGH;
    if (strcasecmp(str, "drop") == 0 || strcasecmp(str, "none") == 0)
        return MODE_DROP;
    return MODE_TRANSCODE;
}

static VideoPreset parse_preset(const char *str) {
    if (strcasecmp(str, "ultrafast") == 0) return PRESET_ULTRAFAST;
    if (strcasecmp(str, "superfast") == 0) return PRESET_SUPERFAST;
    if (strcasecmp(str, "veryfast") == 0)  return PRESET_VERYFAST;
    if (strcasecmp(str, "faster") == 0)    return PRESET_FASTER;
    if (strcasecmp(str, "fast") == 0)      return PRESET_FAST;
    if (strcasecmp(str, "medium") == 0)    return PRESET_MEDIUM;
    if (strcasecmp(str, "slow") == 0)      return PRESET_SLOW;
    if (strcasecmp(str, "slower") == 0)    return PRESET_SLOWER;
    if (strcasecmp(str, "veryslow") == 0)  return PRESET_VERYSLOW;
    return PRESET_MEDIUM;
}

static const char *preset_to_string(VideoPreset preset) {
    switch (preset) {
        case PRESET_ULTRAFAST: return "ultrafast";
        case PRESET_SUPERFAST: return "superfast";
        case PRESET_VERYFAST:  return "veryfast";
        case PRESET_FASTER:    return "faster";
        case PRESET_FAST:      return "fast";
        case PRESET_MEDIUM:    return "medium";
        case PRESET_SLOW:      return "slow";
        case PRESET_SLOWER:    return "slower";
        case PRESET_VERYSLOW:  return "veryslow";
        default:               return "medium";
    }
}

/*
 * Timeout callback to quit main loop
 */
static gboolean quit_main_loop_cb(gpointer data) {
    GMainLoop *loop = (GMainLoop *)data;
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
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
            if (debug) {
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
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(g_ctx.pipeline)) {
                GstState old_state, new_state, pending_state;
                gst_message_parse_state_changed(msg, &old_state, &new_state, &pending_state);
                if (g_ctx.debug) {
                    fprintf(stderr, "Pipeline state: %s -> %s\n",
                            gst_element_state_get_name(old_state),
                            gst_element_state_get_name(new_state));
                }
            }
            break;

        default:
            break;
    }

    return TRUE;
}

/*
 * Helper to parse video caps into stream info
 */
static void parse_video_caps(GstCaps *caps) {
    if (!caps || gst_caps_is_empty(caps) || gst_caps_is_any(caps)) {
        return;
    }

    GstStructure *str = gst_caps_get_structure(caps, 0);
    const gchar *name = gst_structure_get_name(str);

    if (!g_str_has_prefix(name, "video/")) {
        return;
    }

    pthread_mutex_lock(&g_ctx.lock);

    /* Parse video codec */
    if (g_strcmp0(name, "video/x-h264") == 0) {
        g_ctx.stream_info.video_codec = VIDEO_CODEC_H264;
    } else if (g_strcmp0(name, "video/x-h265") == 0) {
        g_ctx.stream_info.video_codec = VIDEO_CODEC_H265;
    } else if (g_strcmp0(name, "video/mpeg") == 0) {
        gint mpegversion = 0;
        gst_structure_get_int(str, "mpegversion", &mpegversion);
        if (mpegversion == 2) {
            g_ctx.stream_info.video_codec = VIDEO_CODEC_MPEG2;
        }
    }

    gst_structure_get_int(str, "width", &g_ctx.stream_info.video_width);
    gst_structure_get_int(str, "height", &g_ctx.stream_info.video_height);

    gint fps_num = 0, fps_den = 1;
    if (gst_structure_get_fraction(str, "framerate", &fps_num, &fps_den)) {
        g_ctx.stream_info.video_fps_num = fps_num;
        g_ctx.stream_info.video_fps_den = fps_den;
    }

    const gchar *interlace_mode = gst_structure_get_string(str, "interlace-mode");
    g_ctx.stream_info.video_interlaced =
        (interlace_mode && g_strcmp0(interlace_mode, "progressive") != 0);

    const gchar *profile = gst_structure_get_string(str, "profile");
    if (profile) {
        strncpy(g_ctx.stream_info.video_profile, profile,
                sizeof(g_ctx.stream_info.video_profile) - 1);
    }

    g_ctx.stream_info.video_detected = TRUE;

    if (g_ctx.debug) {
        fprintf(stderr, "Video detected: %s %dx%d @ %d/%d fps%s\n",
                video_codec_to_string(g_ctx.stream_info.video_codec),
                g_ctx.stream_info.video_width,
                g_ctx.stream_info.video_height,
                g_ctx.stream_info.video_fps_num,
                g_ctx.stream_info.video_fps_den,
                g_ctx.stream_info.video_interlaced ? " (interlaced)" : "");
    }

    pthread_mutex_unlock(&g_ctx.lock);
}

/*
 * Helper to parse audio caps into stream info
 */
static void parse_audio_caps(GstCaps *caps) {
    if (!caps || gst_caps_is_empty(caps) || gst_caps_is_any(caps)) {
        return;
    }

    GstStructure *str = gst_caps_get_structure(caps, 0);
    const gchar *name = gst_structure_get_name(str);

    if (!g_str_has_prefix(name, "audio/")) {
        return;
    }

    pthread_mutex_lock(&g_ctx.lock);

    /* Parse audio codec */
    if (g_strcmp0(name, "audio/mpeg") == 0) {
        gint mpegversion = 0;
        gst_structure_get_int(str, "mpegversion", &mpegversion);
        if (mpegversion == 4 || mpegversion == 2) {
            g_ctx.stream_info.audio_codec = AUDIO_CODEC_AAC;
        } else if (mpegversion == 1) {
            gint layer = 0;
            gst_structure_get_int(str, "layer", &layer);
            if (layer == 2) {
                g_ctx.stream_info.audio_codec = AUDIO_CODEC_MP2;
            }
        }
    } else if (g_strcmp0(name, "audio/x-ac3") == 0) {
        g_ctx.stream_info.audio_codec = AUDIO_CODEC_AC3;
    } else if (g_strcmp0(name, "audio/x-eac3") == 0) {
        g_ctx.stream_info.audio_codec = AUDIO_CODEC_EAC3;
    }

    gst_structure_get_int(str, "channels", &g_ctx.stream_info.audio_channels);
    gst_structure_get_int(str, "rate", &g_ctx.stream_info.audio_sample_rate);

    g_ctx.stream_info.audio_detected = TRUE;

    if (g_ctx.debug) {
        fprintf(stderr, "Audio detected: %s %d ch @ %d Hz\n",
                audio_codec_to_string(g_ctx.stream_info.audio_codec),
                g_ctx.stream_info.audio_channels,
                g_ctx.stream_info.audio_sample_rate);
    }

    pthread_mutex_unlock(&g_ctx.lock);
}

/*
 * Pad probe to capture caps when they become available/change
 * This catches the full caps including width/height/framerate
 */
static GstPadProbeReturn on_caps_probe(GstPad *pad, GstPadProbeInfo *info, gpointer data) {
    gboolean is_video = GPOINTER_TO_INT(data);
    GstEvent *event;

    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
        event = GST_PAD_PROBE_INFO_EVENT(info);

        if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
            GstCaps *caps = NULL;
            gst_event_parse_caps(event, &caps);

            if (g_ctx.debug && caps) {
                gchar *caps_str = gst_caps_to_string(caps);
                fprintf(stderr, "Caps event on %s: %s\n", GST_PAD_NAME(pad), caps_str);
                g_free(caps_str);
            }

            if (is_video) {
                parse_video_caps(caps);
            } else {
                parse_audio_caps(caps);
            }

            /* Check if we have both streams with full info */
            if (g_ctx.detect_only &&
                g_ctx.stream_info.video_detected &&
                g_ctx.stream_info.audio_detected &&
                g_ctx.stream_info.video_width > 0 &&
                g_ctx.stream_info.audio_sample_rate > 0) {
                /* We have complete info, schedule quit */
                g_timeout_add(200, quit_main_loop_cb, g_ctx.main_loop);
            }
        }
    }

    return GST_PAD_PROBE_OK;
}

/*
 * Callback when tsdemux adds a new pad (video or audio stream found)
 */
static void on_demux_pad_added(GstElement *element, GstPad *pad, gpointer data) {
    (void)element;
    (void)data;

    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps) {
        caps = gst_pad_query_caps(pad, NULL);
    }

    if (!caps) {
        fprintf(stderr, "Warning: Could not get caps for pad %s\n", GST_PAD_NAME(pad));
        return;
    }

    GstStructure *str = gst_caps_get_structure(caps, 0);
    const gchar *name = gst_structure_get_name(str);

    if (g_ctx.debug) {
        gchar *caps_str = gst_caps_to_string(caps);
        fprintf(stderr, "Pad added: %s, caps: %s\n", GST_PAD_NAME(pad), caps_str);
        g_free(caps_str);
    }

    GstElement *sink = NULL;
    gboolean is_video = FALSE;

    if (g_str_has_prefix(name, "video/")) {
        sink = gst_bin_get_by_name(GST_BIN(g_ctx.pipeline), "fakesink_v");
        is_video = TRUE;
        /* Initial parse of video caps */
        parse_video_caps(caps);
    } else if (g_str_has_prefix(name, "audio/")) {
        sink = gst_bin_get_by_name(GST_BIN(g_ctx.pipeline), "fakesink_a");
        is_video = FALSE;
        /* Initial parse of audio caps */
        parse_audio_caps(caps);
    }

    if (sink) {
        GstPad *sink_pad = gst_element_get_static_pad(sink, "sink");

        if (sink_pad && !gst_pad_is_linked(sink_pad)) {
            /* Add a probe to capture caps events for full stream info */
            gst_pad_add_probe(pad,
                              GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                              on_caps_probe,
                              GINT_TO_POINTER(is_video),
                              NULL);

            /* Link the pad to the fakesink */
            GstPadLinkReturn link_ret = gst_pad_link(pad, sink_pad);
            if (link_ret != GST_PAD_LINK_OK) {
                fprintf(stderr, "Warning: Failed to link %s pad: %d\n",
                        is_video ? "video" : "audio", link_ret);
            } else if (g_ctx.debug) {
                fprintf(stderr, "Linked %s pad to fakesink\n", is_video ? "video" : "audio");
            }
        }

        if (sink_pad) {
            gst_object_unref(sink_pad);
        }
        gst_object_unref(sink);
    }

    gst_caps_unref(caps);
}

/*
 * Simple JSON string value extractor
 * Finds "key": "value" and returns the value (caller must free)
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
 * Simple JSON integer value extractor
 * Finds "key": 123 and returns the integer value
 */
static int json_get_int(const char *json, const char *key) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);

    const char *pos = strstr(json, pattern);
    if (!pos) return 0;

    pos += strlen(pattern);
    while (*pos == ' ' || *pos == '\t') pos++;

    return atoi(pos);
}

/*
 * Detect stream using ffprobe (more reliable than GStreamer for format detection)
 */
static int detect_stream_with_ffprobe(void) {
    char cmd[512];
    char url[256];
    FILE *fp;
    char *output = NULL;
    size_t output_size = 0;
    size_t output_capacity = 32768;
    int ret = -1;

    /* Build URL */
    snprintf(url, sizeof(url), "udp://@%s:%d", g_ctx.input_address, g_ctx.input_port);

    /* Build ffprobe command */
    snprintf(cmd, sizeof(cmd),
             "ffprobe -v quiet -print_format json -show_streams "
             "-analyzeduration 5000000 -probesize 5000000 -i \"%s\" 2>/dev/null",
             url);

    if (g_ctx.debug) {
        fprintf(stderr, "Running: %s\n", cmd);
    }

    fprintf(stderr, "Detecting stream at %s:%d using ffprobe...\n",
            g_ctx.input_address, g_ctx.input_port);

    fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "Error: Failed to run ffprobe\n");
        return -1;
    }

    /* Read all output */
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
        fprintf(stderr, "Error: ffprobe failed (status %d)\n", status);
        free(output);
        return -1;
    }

    if (g_ctx.debug) {
        fprintf(stderr, "FFprobe output (%zu bytes):\n%s\n", output_size, output);
    }

    /* Parse streams - look for video and audio codec_type sections */
    const char *stream_pos = output;
    while ((stream_pos = strstr(stream_pos, "\"codec_type\"")) != NULL) {
        /* Find the start of this stream object (search backwards for {) */
        const char *stream_start = stream_pos;
        int brace_count = 0;
        while (stream_start > output) {
            stream_start--;
            if (*stream_start == '{') {
                brace_count++;
                if (brace_count == 1) break;
            } else if (*stream_start == '}') {
                brace_count--;
            }
        }

        /* Find the end of this stream object */
        const char *stream_end = stream_pos;
        brace_count = 0;
        while (*stream_end) {
            if (*stream_end == '{') brace_count++;
            else if (*stream_end == '}') {
                brace_count--;
                if (brace_count < 0) break;
            }
            stream_end++;
        }

        /* Extract this stream's JSON */
        size_t stream_len = stream_end - stream_start + 1;
        char *stream_json = malloc(stream_len + 1);
        if (!stream_json) break;
        memcpy(stream_json, stream_start, stream_len);
        stream_json[stream_len] = '\0';

        /* Check codec type */
        char *codec_type = json_get_string(stream_json, "codec_type");
        if (codec_type) {
            if (strcmp(codec_type, "video") == 0 && !g_ctx.stream_info.video_detected) {
                /* Parse video info */
                char *codec_name = json_get_string(stream_json, "codec_name");
                if (codec_name) {
                    if (strcasecmp(codec_name, "h264") == 0 || strcasecmp(codec_name, "avc") == 0) {
                        g_ctx.stream_info.video_codec = VIDEO_CODEC_H264;
                    } else if (strcasecmp(codec_name, "hevc") == 0 || strcasecmp(codec_name, "h265") == 0) {
                        g_ctx.stream_info.video_codec = VIDEO_CODEC_H265;
                    } else if (strcasecmp(codec_name, "mpeg2video") == 0) {
                        g_ctx.stream_info.video_codec = VIDEO_CODEC_MPEG2;
                    }
                    free(codec_name);
                }

                g_ctx.stream_info.video_width = json_get_int(stream_json, "width");
                g_ctx.stream_info.video_height = json_get_int(stream_json, "height");

                /* Parse framerate (r_frame_rate is "num/den") */
                char *fps = json_get_string(stream_json, "r_frame_rate");
                if (fps) {
                    if (sscanf(fps, "%d/%d", &g_ctx.stream_info.video_fps_num,
                               &g_ctx.stream_info.video_fps_den) != 2) {
                        g_ctx.stream_info.video_fps_num = 0;
                        g_ctx.stream_info.video_fps_den = 1;
                    }
                    free(fps);
                }

                /* Check for interlaced (field_order != "progressive") */
                char *field_order = json_get_string(stream_json, "field_order");
                if (field_order) {
                    g_ctx.stream_info.video_interlaced =
                        (strcmp(field_order, "progressive") != 0 &&
                         strcmp(field_order, "unknown") != 0);
                    free(field_order);
                }

                /* Profile */
                char *profile = json_get_string(stream_json, "profile");
                if (profile) {
                    strncpy(g_ctx.stream_info.video_profile, profile,
                            sizeof(g_ctx.stream_info.video_profile) - 1);
                    free(profile);
                }

                g_ctx.stream_info.video_detected = TRUE;

                if (g_ctx.debug) {
                    fprintf(stderr, "Video detected: %s %dx%d @ %d/%d fps%s\n",
                            video_codec_to_string(g_ctx.stream_info.video_codec),
                            g_ctx.stream_info.video_width,
                            g_ctx.stream_info.video_height,
                            g_ctx.stream_info.video_fps_num,
                            g_ctx.stream_info.video_fps_den,
                            g_ctx.stream_info.video_interlaced ? " (interlaced)" : "");
                }

            } else if (strcmp(codec_type, "audio") == 0 && !g_ctx.stream_info.audio_detected) {
                /* Parse audio info */
                char *codec_name = json_get_string(stream_json, "codec_name");
                if (codec_name) {
                    if (strcasecmp(codec_name, "aac") == 0) {
                        g_ctx.stream_info.audio_codec = AUDIO_CODEC_AAC;
                    } else if (strcasecmp(codec_name, "ac3") == 0) {
                        g_ctx.stream_info.audio_codec = AUDIO_CODEC_AC3;
                    } else if (strcasecmp(codec_name, "eac3") == 0) {
                        g_ctx.stream_info.audio_codec = AUDIO_CODEC_EAC3;
                    } else if (strcasecmp(codec_name, "mp2") == 0) {
                        g_ctx.stream_info.audio_codec = AUDIO_CODEC_MP2;
                    }
                    free(codec_name);
                }

                g_ctx.stream_info.audio_channels = json_get_int(stream_json, "channels");

                /* Sample rate might be a string */
                char *sample_rate_str = json_get_string(stream_json, "sample_rate");
                if (sample_rate_str) {
                    g_ctx.stream_info.audio_sample_rate = atoi(sample_rate_str);
                    free(sample_rate_str);
                }
                if (g_ctx.stream_info.audio_sample_rate == 0) {
                    g_ctx.stream_info.audio_sample_rate = json_get_int(stream_json, "sample_rate");
                }

                g_ctx.stream_info.audio_detected = TRUE;

                if (g_ctx.debug) {
                    fprintf(stderr, "Audio detected: %s %d ch @ %d Hz\n",
                            audio_codec_to_string(g_ctx.stream_info.audio_codec),
                            g_ctx.stream_info.audio_channels,
                            g_ctx.stream_info.audio_sample_rate);
                }
            }
            free(codec_type);
        }

        free(stream_json);
        stream_pos++;  /* Move past current match */
    }

    if (g_ctx.stream_info.video_detected || g_ctx.stream_info.audio_detected) {
        ret = 0;
    } else {
        fprintf(stderr, "Error: No streams detected\n");
    }

    free(output);
    return ret;
}

/*
 * Create detection-only pipeline
 * udpsrc -> queue -> tsparse -> tsdemux -> (pads inspected via callback)
 */
static int create_detection_pipeline(void) {
    GstElement *udpsrc, *queue, *tsparse, *tsdemux, *fakesink_v, *fakesink_a;
    char uri[256];

    g_ctx.pipeline = gst_pipeline_new("detection-pipeline");
    if (!g_ctx.pipeline) {
        fprintf(stderr, "Error: Failed to create pipeline\n");
        return -1;
    }

    /* Create elements */
    udpsrc = gst_element_factory_make("udpsrc", "udpsrc");
    queue = gst_element_factory_make("queue", "queue");
    tsparse = gst_element_factory_make("tsparse", "tsparse");
    tsdemux = gst_element_factory_make("tsdemux", "tsdemux");
    fakesink_v = gst_element_factory_make("fakesink", "fakesink_v");
    fakesink_a = gst_element_factory_make("fakesink", "fakesink_a");

    if (!udpsrc || !queue || !tsparse || !tsdemux || !fakesink_v || !fakesink_a) {
        fprintf(stderr, "Error: Failed to create GStreamer elements\n");
        fprintf(stderr, "  udpsrc=%p queue=%p tsparse=%p tsdemux=%p\n",
                (void*)udpsrc, (void*)queue, (void*)tsparse, (void*)tsdemux);
        return -1;
    }

    /* Configure udpsrc */
    snprintf(uri, sizeof(uri), "udp://%s:%d", g_ctx.input_address, g_ctx.input_port);
    g_object_set(udpsrc, "uri", uri, NULL);
    g_object_set(udpsrc, "buffer-size", 2097152, NULL);  /* 2MB buffer */

    if (g_ctx.input_interface[0]) {
        g_object_set(udpsrc, "multicast-iface", g_ctx.input_interface, NULL);
    }

    /* Configure queue */
    g_object_set(queue,
                 "leaky", 1,  /* downstream */
                 "max-size-buffers", 0,
                 "max-size-time", (guint64)3000000000,  /* 3 seconds */
                 "max-size-bytes", 0,
                 NULL);

    /* Add all elements to pipeline */
    gst_bin_add_many(GST_BIN(g_ctx.pipeline),
                     udpsrc, queue, tsparse, tsdemux, fakesink_v, fakesink_a, NULL);

    /* Link static elements: udpsrc -> queue -> tsparse -> tsdemux */
    if (!gst_element_link_many(udpsrc, queue, tsparse, tsdemux, NULL)) {
        fprintf(stderr, "Error: Failed to link elements\n");
        return -1;
    }

    /* Connect to pad-added signal for dynamic linking */
    g_signal_connect(tsdemux, "pad-added", G_CALLBACK(on_demux_pad_added), NULL);

    /* Set up bus watch */
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(g_ctx.pipeline));
    gst_bus_add_watch(bus, on_bus_message, NULL);
    gst_object_unref(bus);

    fprintf(stderr, "Detecting stream at %s:%d...\n",
            g_ctx.input_address, g_ctx.input_port);

    return 0;
}

/*
 * Print detected stream info as JSON
 */
static void print_detected_info(void) {
    printf("{\n");
    printf("  \"video\": {\n");
    printf("    \"codec\": \"%s\",\n", video_codec_to_string(g_ctx.stream_info.video_codec));
    printf("    \"width\": %d,\n", g_ctx.stream_info.video_width);
    printf("    \"height\": %d,\n", g_ctx.stream_info.video_height);
    printf("    \"framerate\": \"%d/%d\",\n",
           g_ctx.stream_info.video_fps_num, g_ctx.stream_info.video_fps_den);
    printf("    \"interlaced\": %s,\n", g_ctx.stream_info.video_interlaced ? "true" : "false");
    printf("    \"profile\": \"%s\"\n", g_ctx.stream_info.video_profile);
    printf("  },\n");
    printf("  \"audio\": {\n");
    printf("    \"codec\": \"%s\",\n", audio_codec_to_string(g_ctx.stream_info.audio_codec));
    printf("    \"channels\": %d,\n", g_ctx.stream_info.audio_channels);
    printf("    \"sample_rate\": %d\n", g_ctx.stream_info.audio_sample_rate);
    printf("  }\n");
    printf("}\n");
}

/*
 * Create full transcoding pipeline (placeholder for Phase 2)
 */
static int create_transcode_pipeline(void) {
    fprintf(stderr, "Full transcoding pipeline not yet implemented\n");
    fprintf(stderr, "Use --detect-only to verify stream detection works\n");
    return -1;
}

/*
 * Cleanup resources
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

    pthread_mutex_destroy(&g_ctx.lock);
}

/*
 * Main entry point
 */
int main(int argc, char *argv[]) {
    int ret = 0;

    /* Initialize context */
    init_context();

    /* Parse arguments */
    if (parse_args(argc, argv) != 0) {
        print_help(argv[0]);
        return 1;
    }

    /* Set up signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    /* For detect-only mode, use ffprobe (more reliable) */
    if (g_ctx.detect_only) {
        ret = detect_stream_with_ffprobe();
        if (ret == 0) {
            print_detected_info();
        }
        pthread_mutex_destroy(&g_ctx.lock);
        return ret != 0 ? 1 : 0;
    }

    /* Initialize GStreamer for transcoding */
    gst_init(&argc, &argv);

    /* Create main loop */
    g_ctx.main_loop = g_main_loop_new(NULL, FALSE);

    /* Create transcoding pipeline */
    ret = create_transcode_pipeline();

    if (ret != 0) {
        cleanup();
        return 1;
    }

    /* Start pipeline */
    GstStateChangeReturn state_ret = gst_element_set_state(g_ctx.pipeline, GST_STATE_PLAYING);
    if (state_ret == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "Error: Failed to start pipeline\n");
        cleanup();
        return 1;
    }

    /* Record start time */
    gettimeofday(&g_ctx.start_time, NULL);

    /* Run main loop */
    g_main_loop_run(g_ctx.main_loop);

    /* Cleanup */
    cleanup();

    return ret;
}
