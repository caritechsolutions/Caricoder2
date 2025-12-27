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
#define VERSION "2.2.0"

/* Defaults */
#define DEFAULT_VIDEO_BITRATE 5000000
#define DEFAULT_AUDIO_BITRATE 128000
#define DEFAULT_KEYFRAME_INTERVAL 60
#define DEFAULT_AUDIO_SAMPLERATE 48000
#define DEFAULT_TCP_PORT 8888

/* x264 defaults for low-latency streaming */
#define DEFAULT_X264_BFRAMES 0
#define DEFAULT_X264_REF 1
#define DEFAULT_X264_QP_MIN 10
#define DEFAULT_X264_QP_MAX 51
#define DEFAULT_X264_VBV_BUF 600
#define DEFAULT_X264_THREADS 0

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

    /* x264 encoder settings */
    int x264_bframes;           /* B-frames between I and P (0-16) */
    int x264_ref;               /* Reference frames (1-12) */
    int x264_qp_min;            /* Minimum quantizer (0-51) */
    int x264_qp_max;            /* Maximum quantizer (0-51) */
    int x264_vbv_buf;           /* VBV buffer size in ms (0-10000) */
    int x264_threads;           /* Encoding threads (0=auto) */
    int x264_sliced_threads;    /* Low latency sliced threading */
    int x264_b_adapt;           /* Adaptive B-frame decision */
    int x264_cabac;             /* CABAC entropy coding */
    int x264_intra_refresh;     /* Periodic intra refresh */
    int x264_interlaced;        /* Interlaced encoding */
    int x264_aud;               /* Access Unit delimiters */
    int x264_trellis;           /* Trellis quantization */
    int x264_weightp;           /* Weighted P-frames (0-2) */
    int x264_rc_lookahead;      /* Rate control lookahead frames */
    char x264_profile[32];      /* H.264 profile */
    char x264_psy_tune[32];     /* Psychovisual tuning */
    char x264_option_string[256]; /* Custom x264 options */

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

    /* AAC encoder settings (avenc_aac) */
    char aac_coder[16];         /* Coding algorithm: anmr, twoloop, fast */
    int aac_is;                 /* Intensity stereo coding */
    int aac_ms;                 /* Force M/S stereo coding */
    int aac_pns;                /* Perceptual noise substitution */
    int aac_tns;                /* Temporal noise shaping */
    int aac_ltp;                /* Long term prediction */
    int aac_pred;               /* AAC-Main prediction */
    int aac_cutoff;             /* Audio cutoff bandwidth (0=auto) */
    int aac_strict;             /* Standards compliance (-2 to 2) */

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

    printf("X264 ENCODER OPTIONS:\n");
    printf("  --profile PROFILE          baseline|main|high (default: main)\n");
    printf("  --bframes N                B-frames between I and P, 0-16 (default: 0)\n");
    printf("  --ref N                    Reference frames, 1-12 (default: 1)\n");
    printf("  --qp-min N                 Minimum quantizer, 0-51 (default: 10)\n");
    printf("  --qp-max N                 Maximum quantizer, 0-51 (default: 51)\n");
    printf("  --vbv-bufsize MS           VBV buffer size in ms, 0-10000 (default: 600)\n");
    printf("  --rc-lookahead N           Lookahead frames for ratecontrol (default: 0)\n");
    printf("  --threads N                Encoding threads, 0=auto (default: 0)\n");
    printf("  --sliced-threads           Enable low-latency sliced threading\n");
    printf("  --cabac / --no-cabac       Enable/disable CABAC entropy coding (default: on)\n");
    printf("  --trellis                  Enable trellis quantization\n");
    printf("  --b-adapt                  Enable adaptive B-frame decision\n");
    printf("  --weightp N                Weighted P-frames, 0-2 (default: 0)\n");
    printf("  --intra-refresh            Use periodic intra refresh instead of IDR\n");
    printf("  --interlaced               Enable interlaced encoding\n");
    printf("  --aud / --no-aud           Enable/disable Access Unit delimiters (default: on)\n");
    printf("  --psy-tune TUNE            none|film|animation|grain|psnr|ssim\n");
    printf("  --x264-opts STRING         Custom x264 options (key1=val1:key2=val2)\n");
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

    printf("AAC ENCODER OPTIONS:\n");
    printf("  --aac-coder CODER          anmr|twoloop|fast (default: fast)\n");
    printf("  --aac-is / --no-aac-is     Enable/disable intensity stereo (default: on)\n");
    printf("  --aac-ms / --no-aac-ms     Enable/disable M/S stereo coding (default: on)\n");
    printf("  --aac-pns / --no-aac-pns   Enable/disable perceptual noise sub (default: on)\n");
    printf("  --aac-tns / --no-aac-tns   Enable/disable temporal noise shaping (default: on)\n");
    printf("  --aac-ltp / --no-aac-ltp   Enable/disable long term prediction (default: off)\n");
    printf("  --aac-pred / --no-aac-pred Enable/disable AAC-Main prediction (default: off)\n");
    printf("  --aac-cutoff HZ            Audio cutoff bandwidth, 0=auto (default: 0)\n");
    printf("  --aac-strict N             Strictness, -2 to 2 (default: 0)\n");
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

    /* x264 defaults for low-latency streaming */
    g_ctx.x264_bframes = DEFAULT_X264_BFRAMES;
    g_ctx.x264_ref = DEFAULT_X264_REF;
    g_ctx.x264_qp_min = DEFAULT_X264_QP_MIN;
    g_ctx.x264_qp_max = DEFAULT_X264_QP_MAX;
    g_ctx.x264_vbv_buf = DEFAULT_X264_VBV_BUF;
    g_ctx.x264_threads = DEFAULT_X264_THREADS;
    g_ctx.x264_sliced_threads = 1;   /* Enable for low latency */
    g_ctx.x264_b_adapt = 0;          /* Disable for low latency */
    g_ctx.x264_cabac = 1;            /* Enable by default */
    g_ctx.x264_intra_refresh = 0;    /* Disabled */
    g_ctx.x264_interlaced = 0;       /* Progressive */
    g_ctx.x264_aud = 1;              /* Enable AUD */
    g_ctx.x264_trellis = 0;          /* Disable for speed */
    g_ctx.x264_weightp = 0;          /* Disable for low latency */
    g_ctx.x264_rc_lookahead = 0;     /* Zero for low latency */
    strcpy(g_ctx.x264_profile, "main");
    g_ctx.x264_psy_tune[0] = '\0';   /* No psy-tune by default */
    g_ctx.x264_option_string[0] = '\0';

    /* Audio defaults */
    g_ctx.audio_mode = MODE_TRANSCODE;
    g_ctx.audio_out_codec = AUDIO_CODEC_AAC;
    g_ctx.audio_bitrate = DEFAULT_AUDIO_BITRATE;
    g_ctx.audio_channels = 2;
    g_ctx.audio_samplerate = DEFAULT_AUDIO_SAMPLERATE;

    /* AAC encoder defaults (avenc_aac) */
    strcpy(g_ctx.aac_coder, "fast");  /* fast coding for low latency */
    g_ctx.aac_is = 1;                 /* intensity stereo enabled */
    g_ctx.aac_ms = 1;                 /* M/S stereo enabled */
    g_ctx.aac_pns = 1;                /* perceptual noise substitution enabled */
    g_ctx.aac_tns = 1;                /* temporal noise shaping enabled */
    g_ctx.aac_ltp = 0;                /* long term prediction disabled */
    g_ctx.aac_pred = 0;               /* AAC-Main prediction disabled */
    g_ctx.aac_cutoff = 0;             /* auto cutoff */
    g_ctx.aac_strict = 0;             /* normal compliance */

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
    /* Long option codes for x264 and AAC settings (no short options) */
    enum {
        /* x264 options */
        OPT_BFRAMES = 1000,
        OPT_REF,
        OPT_QP_MIN,
        OPT_QP_MAX,
        OPT_VBV_BUF,
        OPT_THREADS,
        OPT_SLICED_THREADS,
        OPT_B_ADAPT,
        OPT_CABAC,
        OPT_NO_CABAC,
        OPT_INTRA_REFRESH,
        OPT_INTERLACED,
        OPT_AUD,
        OPT_NO_AUD,
        OPT_TRELLIS,
        OPT_WEIGHTP,
        OPT_RC_LOOKAHEAD,
        OPT_PROFILE,
        OPT_PSY_TUNE,
        OPT_X264_OPTS,
        /* AAC options */
        OPT_AAC_CODER,
        OPT_AAC_IS,
        OPT_AAC_NO_IS,
        OPT_AAC_MS,
        OPT_AAC_NO_MS,
        OPT_AAC_PNS,
        OPT_AAC_NO_PNS,
        OPT_AAC_TNS,
        OPT_AAC_NO_TNS,
        OPT_AAC_LTP,
        OPT_AAC_NO_LTP,
        OPT_AAC_PRED,
        OPT_AAC_NO_PRED,
        OPT_AAC_CUTOFF,
        OPT_AAC_STRICT
    };

    static struct option long_options[] = {
        /* Input options */
        {"input",              required_argument, 0, 'i'},
        {"input-interface",    required_argument, 0, 'I'},
        /* Video options */
        {"video-mode",         required_argument, 0, 'V'},
        {"video-codec",        required_argument, 0, 'c'},
        {"video-bitrate",      required_argument, 0, 'b'},
        {"video-preset",       required_argument, 0, 'p'},
        {"keyframe-interval",  required_argument, 0, 'k'},
        /* x264 encoder options */
        {"bframes",            required_argument, 0, OPT_BFRAMES},
        {"ref",                required_argument, 0, OPT_REF},
        {"qp-min",             required_argument, 0, OPT_QP_MIN},
        {"qp-max",             required_argument, 0, OPT_QP_MAX},
        {"vbv-bufsize",        required_argument, 0, OPT_VBV_BUF},
        {"threads",            required_argument, 0, OPT_THREADS},
        {"sliced-threads",     no_argument,       0, OPT_SLICED_THREADS},
        {"b-adapt",            no_argument,       0, OPT_B_ADAPT},
        {"cabac",              no_argument,       0, OPT_CABAC},
        {"no-cabac",           no_argument,       0, OPT_NO_CABAC},
        {"intra-refresh",      no_argument,       0, OPT_INTRA_REFRESH},
        {"interlaced",         no_argument,       0, OPT_INTERLACED},
        {"aud",                no_argument,       0, OPT_AUD},
        {"no-aud",             no_argument,       0, OPT_NO_AUD},
        {"trellis",            no_argument,       0, OPT_TRELLIS},
        {"weightp",            required_argument, 0, OPT_WEIGHTP},
        {"rc-lookahead",       required_argument, 0, OPT_RC_LOOKAHEAD},
        {"profile",            required_argument, 0, OPT_PROFILE},
        {"psy-tune",           required_argument, 0, OPT_PSY_TUNE},
        {"x264-opts",          required_argument, 0, OPT_X264_OPTS},
        /* AAC encoder options */
        {"aac-coder",          required_argument, 0, OPT_AAC_CODER},
        {"aac-is",             no_argument,       0, OPT_AAC_IS},
        {"no-aac-is",          no_argument,       0, OPT_AAC_NO_IS},
        {"aac-ms",             no_argument,       0, OPT_AAC_MS},
        {"no-aac-ms",          no_argument,       0, OPT_AAC_NO_MS},
        {"aac-pns",            no_argument,       0, OPT_AAC_PNS},
        {"no-aac-pns",         no_argument,       0, OPT_AAC_NO_PNS},
        {"aac-tns",            no_argument,       0, OPT_AAC_TNS},
        {"no-aac-tns",         no_argument,       0, OPT_AAC_NO_TNS},
        {"aac-ltp",            no_argument,       0, OPT_AAC_LTP},
        {"no-aac-ltp",         no_argument,       0, OPT_AAC_NO_LTP},
        {"aac-pred",           no_argument,       0, OPT_AAC_PRED},
        {"no-aac-pred",        no_argument,       0, OPT_AAC_NO_PRED},
        {"aac-cutoff",         required_argument, 0, OPT_AAC_CUTOFF},
        {"aac-strict",         required_argument, 0, OPT_AAC_STRICT},
        /* Scaling options */
        {"scale",              required_argument, 0, 's'},
        {"deinterlace",        no_argument,       0, 'D'},
        /* Audio options */
        {"audio-mode",         required_argument, 0, 'A'},
        {"audio-codec",        required_argument, 0, 'C'},
        {"audio-bitrate",      required_argument, 0, 'a'},
        {"audio-channels",     required_argument, 0, 'n'},
        {"audio-samplerate",   required_argument, 0, 'r'},
        /* Output options */
        {"stdout",             no_argument,       0, 'o'},
        {"tcp-port",           required_argument, 0, 't'},
        /* General options */
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

            /* x264 encoder options */
            case OPT_BFRAMES:
                g_ctx.x264_bframes = atoi(optarg);
                break;
            case OPT_REF:
                g_ctx.x264_ref = atoi(optarg);
                break;
            case OPT_QP_MIN:
                g_ctx.x264_qp_min = atoi(optarg);
                break;
            case OPT_QP_MAX:
                g_ctx.x264_qp_max = atoi(optarg);
                break;
            case OPT_VBV_BUF:
                g_ctx.x264_vbv_buf = atoi(optarg);
                break;
            case OPT_THREADS:
                g_ctx.x264_threads = atoi(optarg);
                break;
            case OPT_SLICED_THREADS:
                g_ctx.x264_sliced_threads = 1;
                break;
            case OPT_B_ADAPT:
                g_ctx.x264_b_adapt = 1;
                break;
            case OPT_CABAC:
                g_ctx.x264_cabac = 1;
                break;
            case OPT_NO_CABAC:
                g_ctx.x264_cabac = 0;
                break;
            case OPT_INTRA_REFRESH:
                g_ctx.x264_intra_refresh = 1;
                break;
            case OPT_INTERLACED:
                g_ctx.x264_interlaced = 1;
                break;
            case OPT_AUD:
                g_ctx.x264_aud = 1;
                break;
            case OPT_NO_AUD:
                g_ctx.x264_aud = 0;
                break;
            case OPT_TRELLIS:
                g_ctx.x264_trellis = 1;
                break;
            case OPT_WEIGHTP:
                g_ctx.x264_weightp = atoi(optarg);
                break;
            case OPT_RC_LOOKAHEAD:
                g_ctx.x264_rc_lookahead = atoi(optarg);
                break;
            case OPT_PROFILE:
                strncpy(g_ctx.x264_profile, optarg, sizeof(g_ctx.x264_profile) - 1);
                break;
            case OPT_PSY_TUNE:
                strncpy(g_ctx.x264_psy_tune, optarg, sizeof(g_ctx.x264_psy_tune) - 1);
                break;
            case OPT_X264_OPTS:
                strncpy(g_ctx.x264_option_string, optarg, sizeof(g_ctx.x264_option_string) - 1);
                break;

            /* AAC encoder options */
            case OPT_AAC_CODER:
                strncpy(g_ctx.aac_coder, optarg, sizeof(g_ctx.aac_coder) - 1);
                break;
            case OPT_AAC_IS:
                g_ctx.aac_is = 1;
                break;
            case OPT_AAC_NO_IS:
                g_ctx.aac_is = 0;
                break;
            case OPT_AAC_MS:
                g_ctx.aac_ms = 1;
                break;
            case OPT_AAC_NO_MS:
                g_ctx.aac_ms = 0;
                break;
            case OPT_AAC_PNS:
                g_ctx.aac_pns = 1;
                break;
            case OPT_AAC_NO_PNS:
                g_ctx.aac_pns = 0;
                break;
            case OPT_AAC_TNS:
                g_ctx.aac_tns = 1;
                break;
            case OPT_AAC_NO_TNS:
                g_ctx.aac_tns = 0;
                break;
            case OPT_AAC_LTP:
                g_ctx.aac_ltp = 1;
                break;
            case OPT_AAC_NO_LTP:
                g_ctx.aac_ltp = 0;
                break;
            case OPT_AAC_PRED:
                g_ctx.aac_pred = 1;
                break;
            case OPT_AAC_NO_PRED:
                g_ctx.aac_pred = 0;
                break;
            case OPT_AAC_CUTOFF:
                g_ctx.aac_cutoff = atoi(optarg);
                break;
            case OPT_AAC_STRICT:
                g_ctx.aac_strict = atoi(optarg);
                break;

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
    char *pipeline = malloc(8192);
    if (!pipeline) return NULL;

    char *p = pipeline;
    int remaining = 8192;
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
                case VIDEO_CODEC_H264: {
                    /* Build x264enc with all options */
                    n = snprintf(p, remaining,
                        "x264enc tune=zerolatency speed-preset=%s bitrate=%d key-int-max=%d "
                        "bframes=%d ref=%d qp-min=%d qp-max=%d vbv-buf-capacity=%d "
                        "rc-lookahead=%d threads=%d sliced-threads=%s b-adapt=%s "
                        "cabac=%s trellis=%s aud=%s intra-refresh=%s interlaced=%s ",
                        preset_to_gst_string(g_ctx.video_preset),
                        g_ctx.video_bitrate / 1000,
                        g_ctx.keyframe_interval,
                        g_ctx.x264_bframes,
                        g_ctx.x264_ref,
                        g_ctx.x264_qp_min,
                        g_ctx.x264_qp_max,
                        g_ctx.x264_vbv_buf,
                        g_ctx.x264_rc_lookahead,
                        g_ctx.x264_threads,
                        g_ctx.x264_sliced_threads ? "true" : "false",
                        g_ctx.x264_b_adapt ? "true" : "false",
                        g_ctx.x264_cabac ? "true" : "false",
                        g_ctx.x264_trellis ? "true" : "false",
                        g_ctx.x264_aud ? "true" : "false",
                        g_ctx.x264_intra_refresh ? "true" : "false",
                        g_ctx.x264_interlaced ? "true" : "false");
                    p += n; remaining -= n;

                    /* Add optional psy-tune */
                    if (g_ctx.x264_psy_tune[0] != '\0') {
                        n = snprintf(p, remaining, "psy-tune=%s ", g_ctx.x264_psy_tune);
                        p += n; remaining -= n;
                    }

                    /* Add optional custom options */
                    if (g_ctx.x264_option_string[0] != '\0') {
                        n = snprintf(p, remaining, "option-string=\"%s\" ", g_ctx.x264_option_string);
                        p += n; remaining -= n;
                    }

                    /* Connect to muxer */
                    n = snprintf(p, remaining, "! mux. ");
                    break;
                }
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
            /* Base audio pipeline up to encoder */
            n = snprintf(p, remaining,
                "demux. ! queue %s ! %s ! %s ! audioconvert ! audioresample ! ",
                queue_settings, parser, decoder);
            p += n; remaining -= n;

            /* Audio encoder with codec-specific options */
            switch (g_ctx.audio_out_codec) {
                case AUDIO_CODEC_AAC:
                    /* avenc_aac with all options */
                    n = snprintf(p, remaining,
                        "avenc_aac bitrate=%d channels=%d sample-rate=%d aac-coder=%s "
                        "aac-is=%s aac-ms=%s aac-pns=%s aac-tns=%s aac-ltp=%s aac-pred=%s ",
                        g_ctx.audio_bitrate,
                        g_ctx.audio_channels,
                        g_ctx.audio_samplerate,
                        g_ctx.aac_coder,
                        g_ctx.aac_is ? "true" : "false",
                        g_ctx.aac_ms ? "true" : "false",
                        g_ctx.aac_pns ? "true" : "false",
                        g_ctx.aac_tns ? "true" : "false",
                        g_ctx.aac_ltp ? "true" : "false",
                        g_ctx.aac_pred ? "true" : "false");
                    p += n; remaining -= n;

                    /* Add optional cutoff */
                    if (g_ctx.aac_cutoff > 0) {
                        n = snprintf(p, remaining, "cutoff=%d ", g_ctx.aac_cutoff);
                        p += n; remaining -= n;
                    }

                    /* Add strict if non-default */
                    if (g_ctx.aac_strict != 0) {
                        n = snprintf(p, remaining, "strict=%d ", g_ctx.aac_strict);
                        p += n; remaining -= n;
                    }

                    n = snprintf(p, remaining, "! mux. ");
                    break;

                case AUDIO_CODEC_AC3:
                    n = snprintf(p, remaining,
                        "avenc_ac3 bitrate=%d ! mux. ", g_ctx.audio_bitrate);
                    break;

                case AUDIO_CODEC_MP2:
                    n = snprintf(p, remaining,
                        "avenc_mp2 bitrate=%d ! mux. ", g_ctx.audio_bitrate);
                    break;

                default:
                    n = snprintf(p, remaining, "%s bitrate=%d ! mux. ",
                        encoder, g_ctx.audio_bitrate);
            }
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
