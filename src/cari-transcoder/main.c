/*
 * CariTranscoder - GStreamer-based Video/Audio Transcoder
 *
 * Uses gst_parse_launch() to build pipelines - much simpler and more reliable.
 * Receives MPEG-TS via UDP, transcodes video/audio using GStreamer,
 * and outputs to stdout or TCP for piping to tsp.
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
#define VERSION "2.0.0"

/* Defaults */
#define DEFAULT_VIDEO_BITRATE 5000000
#define DEFAULT_AUDIO_BITRATE 128000
#define DEFAULT_KEYFRAME_INTERVAL 60
#define DEFAULT_AUDIO_SAMPLERATE 48000
#define DEFAULT_TCP_PORT 8888

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
    VideoCodec video_codec;
    int video_width;
    int video_height;
    int video_fps_num;
    int video_fps_den;
    gboolean video_interlaced;

    AudioCodec audio_codec;
    int audio_channels;
    int audio_sample_rate;

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
    int keyframe_interval;

    /* Scaling settings */
    int scale_width;
    int scale_height;
    gboolean deinterlace;

    /* Audio output settings */
    ProcessingMode audio_mode;
    AudioCodec audio_out_codec;
    int audio_bitrate;
    int audio_channels;
    int audio_samplerate;

    /* Output settings */
    gboolean use_stdout;
    int tcp_port;

    /* General settings */
    gboolean debug;
    gboolean detect_only;

    /* Runtime state */
    volatile int running;
    StreamInfo stream_info;

    /* GStreamer */
    GstElement *pipeline;
    GMainLoop *main_loop;

    /* Threading */
    pthread_mutex_t lock;

    /* Statistics */
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
static int detect_stream_with_ffprobe(void);
static void print_detected_info(void);
static char *build_pipeline_string(void);
static int create_pipeline(void);
static gboolean on_bus_message(GstBus *bus, GstMessage *msg, gpointer data);

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
    printf("                             medium|slow|slower|veryslow (default: superfast)\n");
    printf("  --keyframe-interval FRAMES GOP size in frames (default: 60)\n");
    printf("\n");

    printf("SCALING OPTIONS:\n");
    printf("  --scale WIDTHxHEIGHT       Output resolution (e.g., 1280x720)\n");
    printf("  --deinterlace              Enable deinterlacing\n");
    printf("\n");

    printf("AUDIO OPTIONS:\n");
    printf("  --audio-mode MODE          transcode|passthrough|drop (default: transcode)\n");
    printf("  --audio-codec CODEC        aac|ac3|mp2 (default: aac)\n");
    printf("  --audio-bitrate BPS        Audio bitrate in bps (default: 128000)\n");
    printf("  --audio-channels COUNT     1|2|6 (default: 2/stereo)\n");
    printf("  --audio-samplerate HZ      Sample rate (default: 48000)\n");
    printf("\n");

    printf("OUTPUT OPTIONS:\n");
    printf("  --stdout                   Output to stdout (for piping to tsp)\n");
    printf("  --tcp-port PORT            TCP server port (default: 8888)\n");
    printf("\n");

    printf("GENERAL OPTIONS:\n");
    printf("  --debug                    Enable debug logging\n");
    printf("  --detect-only              Detect stream info and exit (JSON output)\n");
    printf("  --help                     Show this help\n");
    printf("\n");

    printf("EXAMPLES:\n");
    printf("  Detect stream format:\n");
    printf("    %s --input 239.100.0.1:5000 --detect-only\n\n", prog);

    printf("  Transcode to H.264 at 2 Mbps (TCP output for testing):\n");
    printf("    %s --input 239.100.0.1:5000 --video-bitrate 2000000 --tcp-port 8888\n\n", prog);

    printf("  Transcode and pipe to tsp:\n");
    printf("    %s --input 239.100.0.1:5000 --video-bitrate 2000000 --stdout \\\n", prog);
    printf("        | tsp -I file - -P regulate --bitrate 6000000 -O ip 239.100.0.2:5000\n\n");
}

/*
 * Initialize context with defaults
 */
static void init_context(void) {
    memset(&g_ctx, 0, sizeof(g_ctx));

    /* Video defaults */
    g_ctx.video_mode = MODE_TRANSCODE;
    g_ctx.video_out_codec = VIDEO_CODEC_H264;
    g_ctx.video_bitrate = DEFAULT_VIDEO_BITRATE;
    g_ctx.video_preset = PRESET_SUPERFAST;
    g_ctx.keyframe_interval = DEFAULT_KEYFRAME_INTERVAL;

    /* Audio defaults */
    g_ctx.audio_mode = MODE_TRANSCODE;
    g_ctx.audio_out_codec = AUDIO_CODEC_AAC;
    g_ctx.audio_bitrate = DEFAULT_AUDIO_BITRATE;
    g_ctx.audio_channels = 2;
    g_ctx.audio_samplerate = DEFAULT_AUDIO_SAMPLERATE;

    /* Output defaults */
    g_ctx.use_stdout = FALSE;
    g_ctx.tcp_port = DEFAULT_TCP_PORT;

    /* General defaults */
    g_ctx.debug = FALSE;
    g_ctx.detect_only = FALSE;
    g_ctx.running = 1;

    pthread_mutex_init(&g_ctx.lock, NULL);
}

/*
 * Parse command line arguments
 */
static int parse_args(int argc, char *argv[]) {
    static struct option long_options[] = {
        {"input",              required_argument, 0, 'i'},
        {"input-interface",    required_argument, 0, 'I'},
        {"video-mode",         required_argument, 0, 'V'},
        {"video-codec",        required_argument, 0, 'c'},
        {"video-bitrate",      required_argument, 0, 'b'},
        {"video-preset",       required_argument, 0, 'p'},
        {"keyframe-interval",  required_argument, 0, 'k'},
        {"scale",              required_argument, 0, 's'},
        {"deinterlace",        no_argument,       0, 'D'},
        {"audio-mode",         required_argument, 0, 'A'},
        {"audio-codec",        required_argument, 0, 'C'},
        {"audio-bitrate",      required_argument, 0, 'a'},
        {"audio-channels",     required_argument, 0, 'n'},
        {"audio-samplerate",   required_argument, 0, 'r'},
        {"stdout",             no_argument,       0, 'o'},
        {"tcp-port",           required_argument, 0, 't'},
        {"debug",              no_argument,       0, 'd'},
        {"detect-only",        no_argument,       0, 'O'},
        {"help",               no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;
    char *colon;

    while ((opt = getopt_long(argc, argv, "i:I:V:c:b:p:k:s:DA:C:a:n:r:ot:dOh",
                              long_options, &option_index)) != -1) {
        switch (opt) {
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

            case 'k':  /* --keyframe-interval */
                g_ctx.keyframe_interval = atoi(optarg);
                break;

            case 's':  /* --scale WIDTHxHEIGHT */
                if (sscanf(optarg, "%dx%d", &g_ctx.scale_width, &g_ctx.scale_height) != 2) {
                    fprintf(stderr, "Error: Scale must be in WIDTHxHEIGHT format\n");
                    return -1;
                }
                break;

            case 'D':  /* --deinterlace */
                g_ctx.deinterlace = TRUE;
                break;

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
                g_ctx.audio_channels = atoi(optarg);
                break;

            case 'r':  /* --audio-samplerate */
                g_ctx.audio_samplerate = atoi(optarg);
                break;

            case 'o':  /* --stdout */
                g_ctx.use_stdout = TRUE;
                break;

            case 't':  /* --tcp-port */
                g_ctx.tcp_port = atoi(optarg);
                break;

            case 'd':  /* --debug */
                g_ctx.debug = TRUE;
                break;

            case 'O':  /* --detect-only */
                g_ctx.detect_only = TRUE;
                break;

            case 'h':  /* --help */
                print_help(argv[0]);
                exit(0);

            default:
                return -1;
        }
    }

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
    return PRESET_SUPERFAST;
}

static const char *preset_to_gst_string(VideoPreset preset) {
    /* GStreamer x264enc uses these preset names */
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
        default:               return "superfast";
    }
}

/*
 * Simple JSON string value extractor
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
 * Detect stream using ffprobe
 */
static int detect_stream_with_ffprobe(void) {
    char cmd[512];
    char url[256];
    FILE *fp;
    char *output = NULL;
    size_t output_size = 0;
    size_t output_capacity = 32768;
    int ret = -1;

    snprintf(url, sizeof(url), "udp://@%s:%d", g_ctx.input_address, g_ctx.input_port);

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

    /* Parse streams */
    const char *stream_pos = output;
    while ((stream_pos = strstr(stream_pos, "\"codec_type\"")) != NULL) {
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

        size_t stream_len = stream_end - stream_start + 1;
        char *stream_json = malloc(stream_len + 1);
        if (!stream_json) break;
        memcpy(stream_json, stream_start, stream_len);
        stream_json[stream_len] = '\0';

        char *codec_type = json_get_string(stream_json, "codec_type");
        char *codec_name = json_get_string(stream_json, "codec_name");

        if (codec_type && codec_name) {
            if (strcmp(codec_type, "video") == 0 && !g_ctx.stream_info.video_detected) {
                if (strcmp(codec_name, "h264") == 0) {
                    g_ctx.stream_info.video_codec = VIDEO_CODEC_H264;
                } else if (strcmp(codec_name, "hevc") == 0 || strcmp(codec_name, "h265") == 0) {
                    g_ctx.stream_info.video_codec = VIDEO_CODEC_H265;
                } else if (strcmp(codec_name, "mpeg2video") == 0) {
                    g_ctx.stream_info.video_codec = VIDEO_CODEC_MPEG2;
                }

                g_ctx.stream_info.video_width = json_get_int(stream_json, "width");
                g_ctx.stream_info.video_height = json_get_int(stream_json, "height");
                g_ctx.stream_info.video_detected = TRUE;

                fprintf(stderr, "Video detected: %s %dx%d\n",
                        video_codec_to_string(g_ctx.stream_info.video_codec),
                        g_ctx.stream_info.video_width,
                        g_ctx.stream_info.video_height);
                ret = 0;
            } else if (strcmp(codec_type, "audio") == 0 && !g_ctx.stream_info.audio_detected) {
                if (strcmp(codec_name, "aac") == 0) {
                    g_ctx.stream_info.audio_codec = AUDIO_CODEC_AAC;
                } else if (strcmp(codec_name, "ac3") == 0) {
                    g_ctx.stream_info.audio_codec = AUDIO_CODEC_AC3;
                } else if (strcmp(codec_name, "eac3") == 0) {
                    g_ctx.stream_info.audio_codec = AUDIO_CODEC_EAC3;
                } else if (strcmp(codec_name, "mp2") == 0) {
                    g_ctx.stream_info.audio_codec = AUDIO_CODEC_MP2;
                }

                g_ctx.stream_info.audio_channels = json_get_int(stream_json, "channels");
                g_ctx.stream_info.audio_sample_rate = json_get_int(stream_json, "sample_rate");
                g_ctx.stream_info.audio_detected = TRUE;

                fprintf(stderr, "Audio detected: %s %d ch @ %d Hz\n",
                        audio_codec_to_string(g_ctx.stream_info.audio_codec),
                        g_ctx.stream_info.audio_channels,
                        g_ctx.stream_info.audio_sample_rate);
                ret = 0;
            }
        }

        free(codec_type);
        free(codec_name);
        free(stream_json);

        stream_pos++;
    }

    free(output);
    return ret;
}

/*
 * Print detected stream info (JSON format)
 */
static void print_detected_info(void) {
    printf("{\n");
    printf("  \"video\": {\n");
    printf("    \"detected\": %s,\n", g_ctx.stream_info.video_detected ? "true" : "false");
    printf("    \"codec\": \"%s\",\n", video_codec_to_string(g_ctx.stream_info.video_codec));
    printf("    \"width\": %d,\n", g_ctx.stream_info.video_width);
    printf("    \"height\": %d\n", g_ctx.stream_info.video_height);
    printf("  },\n");
    printf("  \"audio\": {\n");
    printf("    \"detected\": %s,\n", g_ctx.stream_info.audio_detected ? "true" : "false");
    printf("    \"codec\": \"%s\",\n", audio_codec_to_string(g_ctx.stream_info.audio_codec));
    printf("    \"channels\": %d,\n", g_ctx.stream_info.audio_channels);
    printf("    \"sample_rate\": %d\n", g_ctx.stream_info.audio_sample_rate);
    printf("  }\n");
    printf("}\n");
}

/*
 * Get video parser element name for input codec
 */
static const char *get_video_parser(VideoCodec codec) {
    switch (codec) {
        case VIDEO_CODEC_H264:  return "h264parse";
        case VIDEO_CODEC_H265:  return "h265parse";
        case VIDEO_CODEC_MPEG2: return "mpegvideoparse";
        default:                return NULL;
    }
}

/*
 * Get video decoder element name for input codec
 */
static const char *get_video_decoder(VideoCodec codec) {
    switch (codec) {
        case VIDEO_CODEC_H264:  return "avdec_h264";
        case VIDEO_CODEC_H265:  return "avdec_h265";
        case VIDEO_CODEC_MPEG2: return "avdec_mpeg2video";
        default:                return NULL;
    }
}

/*
 * Get audio parser element name for input codec
 */
static const char *get_audio_parser(AudioCodec codec) {
    switch (codec) {
        case AUDIO_CODEC_AAC:   return "aacparse";
        case AUDIO_CODEC_AC3:   return "ac3parse";
        case AUDIO_CODEC_EAC3:  return "ac3parse";
        case AUDIO_CODEC_MP2:   return "mpegaudioparse";
        default:                return NULL;
    }
}

/*
 * Get audio decoder element name for input codec
 */
static const char *get_audio_decoder(AudioCodec codec) {
    switch (codec) {
        case AUDIO_CODEC_AAC:   return "avdec_aac";
        case AUDIO_CODEC_AC3:   return "avdec_ac3";
        case AUDIO_CODEC_EAC3:  return "avdec_eac3";
        case AUDIO_CODEC_MP2:   return "avdec_mp2float";
        default:                return NULL;
    }
}

/*
 * Get audio encoder element name for output codec
 */
static const char *get_audio_encoder(AudioCodec codec) {
    switch (codec) {
        case AUDIO_CODEC_AAC:   return "avenc_aac";
        case AUDIO_CODEC_AC3:   return "avenc_ac3";
        case AUDIO_CODEC_MP2:   return "avenc_mp2";
        default:                return NULL;
    }
}

/*
 * Build the pipeline string based on detected input and output settings
 */
static char *build_pipeline_string(void) {
    char *pipeline = malloc(4096);
    if (!pipeline) return NULL;

    char *p = pipeline;
    int remaining = 4096;
    int n;

    /* Queue settings to match Python code */
    const char *queue_settings = "leaky=1 max-size-buffers=0 max-size-time=3000000000 max-size-bytes=0";

    /* Input: udpsrc -> queue -> tsparse -> tsdemux */
    n = snprintf(p, remaining,
        "udpsrc uri=udp://%s:%d buffer-size=2097152 ! "
        "queue %s ! "
        "tsparse ! tsdemux name=demux ",
        g_ctx.input_address, g_ctx.input_port, queue_settings);
    p += n; remaining -= n;

    /* Video branch */
    if (g_ctx.video_mode == MODE_TRANSCODE && g_ctx.stream_info.video_detected) {
        const char *parser = get_video_parser(g_ctx.stream_info.video_codec);
        const char *decoder = get_video_decoder(g_ctx.stream_info.video_codec);

        if (parser && decoder) {
            n = snprintf(p, remaining,
                "demux. ! queue %s ! %s ! %s ! videoconvert ! ",
                queue_settings, parser, decoder);
            p += n; remaining -= n;

            /* Optional deinterlace */
            if (g_ctx.deinterlace) {
                n = snprintf(p, remaining, "deinterlace ! ");
                p += n; remaining -= n;
            }

            /* Optional scaling */
            if (g_ctx.scale_width > 0 && g_ctx.scale_height > 0) {
                n = snprintf(p, remaining,
                    "videoscale ! video/x-raw,width=%d,height=%d ! ",
                    g_ctx.scale_width, g_ctx.scale_height);
                p += n; remaining -= n;
            }

            /* Video encoder based on output codec */
            switch (g_ctx.video_out_codec) {
                case VIDEO_CODEC_H264:
                    n = snprintf(p, remaining,
                        "x264enc tune=zerolatency speed-preset=%s bitrate=%d key-int-max=%d ! mux. ",
                        preset_to_gst_string(g_ctx.video_preset),
                        g_ctx.video_bitrate / 1000,
                        g_ctx.keyframe_interval);
                    break;
                case VIDEO_CODEC_H265:
                    n = snprintf(p, remaining,
                        "x265enc tune=zerolatency speed-preset=%s bitrate=%d key-int-max=%d ! mux. ",
                        preset_to_gst_string(g_ctx.video_preset),
                        g_ctx.video_bitrate / 1000,
                        g_ctx.keyframe_interval);
                    break;
                case VIDEO_CODEC_MPEG2:
                    n = snprintf(p, remaining,
                        "avenc_mpeg2video bitrate=%d gop-size=%d ! mux. ",
                        g_ctx.video_bitrate,
                        g_ctx.keyframe_interval);
                    break;
                default:
                    n = 0;
            }
            p += n; remaining -= n;
        }
    } else if (g_ctx.video_mode == MODE_DROP) {
        n = snprintf(p, remaining, "demux. ! queue %s ! fakesink ", queue_settings);
        p += n; remaining -= n;
    }
    /* TODO: passthrough mode */

    /* Audio branch - matches working gst-launch pipeline exactly */
    if (g_ctx.audio_mode == MODE_TRANSCODE && g_ctx.stream_info.audio_detected) {
        const char *parser = get_audio_parser(g_ctx.stream_info.audio_codec);
        const char *decoder = get_audio_decoder(g_ctx.stream_info.audio_codec);
        const char *encoder = get_audio_encoder(g_ctx.audio_out_codec);

        if (parser && decoder && encoder) {
            n = snprintf(p, remaining,
                "demux. ! queue %s ! %s ! %s ! audioconvert ! audioresample ! "
                "%s bitrate=%d ! mux. ",
                queue_settings, parser, decoder,
                encoder, g_ctx.audio_bitrate);
            p += n; remaining -= n;
        }
    } else if (g_ctx.audio_mode == MODE_DROP) {
        n = snprintf(p, remaining, "demux. ! queue %s ! fakesink ", queue_settings);
        p += n; remaining -= n;
    }
    /* TODO: passthrough mode */

    /* Muxer and output */
    n = snprintf(p, remaining, "mpegtsmux name=mux ! queue %s ! queue ! ", queue_settings);
    p += n; remaining -= n;

    if (g_ctx.use_stdout) {
        n = snprintf(p, remaining, "fdsink fd=1");
    } else {
        n = snprintf(p, remaining, "tcpserversink host=0.0.0.0 port=%d", g_ctx.tcp_port);
    }
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
 * Create pipeline using gst_parse_launch
 */
static int create_pipeline(void) {
    GError *error = NULL;

    /* Build the pipeline string */
    char *pipeline_str = build_pipeline_string();
    if (!pipeline_str) {
        fprintf(stderr, "Error: Failed to build pipeline string\n");
        return -1;
    }

    fprintf(stderr, "\n=== Pipeline ===\n%s\n================\n\n", pipeline_str);

    /* Create pipeline from string */
    g_ctx.pipeline = gst_parse_launch(pipeline_str, &error);
    free(pipeline_str);

    if (error) {
        fprintf(stderr, "Pipeline parse error: %s\n", error->message);
        g_error_free(error);
        return -1;
    }

    if (!g_ctx.pipeline) {
        fprintf(stderr, "Error: Failed to create pipeline\n");
        return -1;
    }

    /* Set up bus watch */
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(g_ctx.pipeline));
    gst_bus_add_watch(bus, on_bus_message, NULL);
    gst_object_unref(bus);

    return 0;
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

    /* For detect-only mode, use ffprobe */
    if (g_ctx.detect_only) {
        ret = detect_stream_with_ffprobe();
        if (ret == 0) {
            print_detected_info();
        }
        pthread_mutex_destroy(&g_ctx.lock);
        return ret != 0 ? 1 : 0;
    }

    /* Always detect stream first to determine input codecs */
    fprintf(stderr, "Step 1: Detecting input stream...\n");
    ret = detect_stream_with_ffprobe();
    if (ret != 0) {
        fprintf(stderr, "Error: Could not detect stream\n");
        return 1;
    }

    if (!g_ctx.stream_info.video_detected && !g_ctx.stream_info.audio_detected) {
        fprintf(stderr, "Error: No video or audio streams detected\n");
        return 1;
    }

    /* Initialize GStreamer */
    fprintf(stderr, "Step 2: Initializing GStreamer...\n");
    gst_init(&argc, &argv);

    /* Create main loop */
    g_ctx.main_loop = g_main_loop_new(NULL, FALSE);

    /* Create pipeline using gst_parse_launch */
    fprintf(stderr, "Step 3: Building pipeline...\n");
    ret = create_pipeline();
    if (ret != 0) {
        cleanup();
        return 1;
    }

    /* Start pipeline */
    fprintf(stderr, "Step 4: Starting transcoder...\n");
    GstStateChangeReturn state_ret = gst_element_set_state(g_ctx.pipeline, GST_STATE_PLAYING);
    if (state_ret == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "Error: Failed to start pipeline\n");
        cleanup();
        return 1;
    }

    gettimeofday(&g_ctx.start_time, NULL);

    if (g_ctx.use_stdout) {
        fprintf(stderr, "Transcoding to stdout. Press Ctrl+C to stop.\n");
    } else {
        fprintf(stderr, "Transcoding to TCP port %d. Press Ctrl+C to stop.\n", g_ctx.tcp_port);
        fprintf(stderr, "Test with: ffplay tcp://localhost:%d\n", g_ctx.tcp_port);
    }

    /* Run main loop */
    g_main_loop_run(g_ctx.main_loop);

    /* Cleanup */
    cleanup();

    return ret;
}
