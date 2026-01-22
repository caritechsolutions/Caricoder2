/*
 * HLS Output Generator
 *
 * Receives UDP MPEG-TS input and generates HLS segments via FFmpeg.
 * Files are served by nginx - no embedded HTTP server needed.
 * Writes stats to a JSON file for API consumption.
 *
 * Usage: hls_output -i <udp_address>:<port> -o <output_dir> [options]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>

#define MAX_VARIANTS 8
#define MAX_PATH_LEN 512
#define MAX_FULL_PATH_LEN 768  /* MAX_PATH_LEN + space for filename */
#define DEFAULT_SEGMENT_DURATION 2
#define DEFAULT_SEGMENT_COUNT 5
#define STATS_UPDATE_INTERVAL 5  /* seconds between stats updates */

/* Application context */
typedef struct {
    /* Configuration */
    char input_address[256];
    uint16_t input_port;
    char output_dir[MAX_PATH_LEN];
    int segment_duration;
    int segment_count;
    int variants;
    int verbose;

    /* State */
    volatile int running;
    pid_t ffmpeg_pid;
    time_t start_time;
    int restart_count;
} AppContext;

static AppContext ctx;

/* Write stats JSON file */
static void write_stats_file(void) {
    char stats_path[MAX_FULL_PATH_LEN];
    char tmp_path[MAX_FULL_PATH_LEN];

    snprintf(stats_path, sizeof(stats_path), "%s/stats.json", ctx.output_dir);
    snprintf(tmp_path, sizeof(tmp_path), "%s/stats.json.tmp", ctx.output_dir);

    FILE *f = fopen(tmp_path, "w");
    if (!f) {
        if (ctx.verbose) {
            fprintf(stderr, "[HLS] Failed to write stats file: %s (%s)\n", tmp_path, strerror(errno));
        }
        return;
    }

    time_t now = time(NULL);
    int ffmpeg_running = (ctx.ffmpeg_pid > 0 && kill(ctx.ffmpeg_pid, 0) == 0);

    /* Count segments in output directory */
    int segment_count = 0;
    DIR *dir = opendir(ctx.output_dir);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strstr(entry->d_name, ".ts") != NULL) {
                segment_count++;
            }
        }
        closedir(dir);
    }

    /* Check if playlist exists */
    char playlist_path[MAX_FULL_PATH_LEN];
    snprintf(playlist_path, sizeof(playlist_path), "%s/playlist.m3u8", ctx.output_dir);
    int playlist_ready = (access(playlist_path, F_OK) == 0);

    fprintf(f,
        "{\n"
        "  \"uptime\": %ld,\n"
        "  \"ffmpeg_running\": %s,\n"
        "  \"ffmpeg_pid\": %d,\n"
        "  \"restart_count\": %d,\n"
        "  \"udp_input\": \"%s:%d\",\n"
        "  \"output_dir\": \"%s\",\n"
        "  \"variants\": %d,\n"
        "  \"segment_duration\": %d,\n"
        "  \"segment_count\": %d,\n"
        "  \"segments_on_disk\": %d,\n"
        "  \"playlist_ready\": %s,\n"
        "  \"playlist\": \"playlist.m3u8\",\n"
        "  \"timestamp\": %ld\n"
        "}\n",
        (long)(now - ctx.start_time),
        ffmpeg_running ? "true" : "false",
        ctx.ffmpeg_pid,
        ctx.restart_count,
        ctx.input_address,
        ctx.input_port,
        ctx.output_dir,
        ctx.variants,
        ctx.segment_duration,
        ctx.segment_count,
        segment_count,
        playlist_ready ? "true" : "false",
        (long)now
    );

    fclose(f);
    if (rename(tmp_path, stats_path) != 0) {
        if (ctx.verbose) {
            fprintf(stderr, "[HLS] Failed to rename stats file: %s\n", strerror(errno));
        }
    }
}

/* Start FFmpeg process */
static int start_ffmpeg(void) {
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        return -1;
    }

    if (pid == 0) {
        /* Child process - exec ffmpeg */
        char input_url[512];
        char segment_pattern[MAX_FULL_PATH_LEN];
        char playlist_path[MAX_FULL_PATH_LEN];
        char duration_str[16];
        char list_size_str[16];

        snprintf(input_url, sizeof(input_url),
                 "udp://%s:%d?fifo_size=5000000&overrun_nonfatal=1",
                 strlen(ctx.input_address) > 0 ? ctx.input_address : "0.0.0.0",
                 ctx.input_port);

        snprintf(duration_str, sizeof(duration_str), "%d", ctx.segment_duration);
        snprintf(list_size_str, sizeof(list_size_str), "%d", ctx.segment_count);

        if (ctx.variants > 1) {
            /* Multi-variant ABR mode */
            char var_stream_map[512] = "";
            char master_pl_name[] = "playlist.m3u8";

            /* Build var_stream_map: "v:0,a:0 v:1,a:1 ..." */
            for (int i = 0; i < ctx.variants; i++) {
                char var_entry[32];
                snprintf(var_entry, sizeof(var_entry), "%sv:%d,a:%d",
                         i > 0 ? " " : "", i, i);
                strncat(var_stream_map, var_entry, sizeof(var_stream_map) - strlen(var_stream_map) - 1);

                /* Create variant directory */
                char var_dir[MAX_FULL_PATH_LEN];
                snprintf(var_dir, sizeof(var_dir), "%s/v%d", ctx.output_dir, i);
                mkdir(var_dir, 0755);
            }

            snprintf(segment_pattern, sizeof(segment_pattern),
                     "%s/v%%v/segment-%%06d.ts", ctx.output_dir);
            snprintf(playlist_path, sizeof(playlist_path),
                     "%s/v%%v/stream.m3u8", ctx.output_dir);

            /* Build ffmpeg args for multi-variant */
            char *argv[64];
            int argc = 0;

            argv[argc++] = "ffmpeg";
            argv[argc++] = "-hide_banner";
            argv[argc++] = "-loglevel";
            argv[argc++] = "warning";
            argv[argc++] = "-fflags";
            argv[argc++] = "+genpts";
            argv[argc++] = "-i";
            argv[argc++] = input_url;

            /* Map each video and audio stream */
            for (int i = 0; i < ctx.variants; i++) {
                char map_v[16], map_a[16];
                snprintf(map_v, sizeof(map_v), "0:v:%d", i);
                snprintf(map_a, sizeof(map_a), "0:a:0");
                argv[argc++] = "-map";
                argv[argc++] = strdup(map_v);
                argv[argc++] = "-map";
                argv[argc++] = strdup(map_a);
            }

            argv[argc++] = "-c";
            argv[argc++] = "copy";
            argv[argc++] = "-f";
            argv[argc++] = "hls";
            argv[argc++] = "-hls_time";
            argv[argc++] = duration_str;
            argv[argc++] = "-hls_list_size";
            argv[argc++] = list_size_str;
            argv[argc++] = "-hls_flags";
            argv[argc++] = "delete_segments+independent_segments";
            argv[argc++] = "-hls_segment_type";
            argv[argc++] = "mpegts";
            argv[argc++] = "-hls_segment_filename";
            argv[argc++] = segment_pattern;
            argv[argc++] = "-master_pl_name";
            argv[argc++] = master_pl_name;
            argv[argc++] = "-var_stream_map";
            argv[argc++] = var_stream_map;
            argv[argc++] = playlist_path;
            argv[argc] = NULL;

            execvp("ffmpeg", argv);
        } else {
            /* Single stream mode */
            snprintf(segment_pattern, sizeof(segment_pattern),
                     "%s/segment-%%06d.ts", ctx.output_dir);
            snprintf(playlist_path, sizeof(playlist_path),
                     "%s/playlist.m3u8", ctx.output_dir);

            execlp("ffmpeg", "ffmpeg",
                   "-hide_banner",
                   "-loglevel", "warning",
                   "-fflags", "+genpts",
                   "-i", input_url,
                   "-c:v", "copy",
                   "-c:a", "copy",
                   "-f", "hls",
                   "-hls_time", duration_str,
                   "-hls_list_size", list_size_str,
                   "-hls_flags", "delete_segments+independent_segments",
                   "-hls_segment_type", "mpegts",
                   "-hls_segment_filename", segment_pattern,
                   playlist_path,
                   NULL);
        }

        /* If we get here, exec failed */
        perror("exec ffmpeg");
        _exit(1);
    }

    /* Parent process */
    ctx.ffmpeg_pid = pid;
    printf("[HLS] Started FFmpeg (PID %d)\n", pid);
    return 0;
}

/* Stop FFmpeg process */
static void stop_ffmpeg(void) {
    if (ctx.ffmpeg_pid > 0) {
        printf("[HLS] Stopping FFmpeg (PID %d)...\n", ctx.ffmpeg_pid);
        kill(ctx.ffmpeg_pid, SIGTERM);

        /* Wait for graceful shutdown */
        int status;
        int waited = 0;
        while (waited < 5) {
            if (waitpid(ctx.ffmpeg_pid, &status, WNOHANG) != 0) {
                break;
            }
            sleep(1);
            waited++;
        }

        /* Force kill if still running */
        if (kill(ctx.ffmpeg_pid, 0) == 0) {
            kill(ctx.ffmpeg_pid, SIGKILL);
            waitpid(ctx.ffmpeg_pid, &status, 0);
        }

        ctx.ffmpeg_pid = 0;
    }
}

/* Signal handler */
static void signal_handler(int sig) {
    (void)sig;
    printf("\nShutting down...\n");
    ctx.running = 0;
}

/* Print usage */
static void print_usage(const char *prog) {
    printf("HLS Output Generator\n");
    printf("Usage: %s -i <udp_input> -o <output_dir> [options]\n\n", prog);
    printf("Generates HLS segments from UDP input. Files are served by nginx.\n\n");
    printf("Required:\n");
    printf("  -i, --input <addr:port>   UDP input address and port (e.g., 239.1.1.1:5000 or :5000)\n");
    printf("  -o, --output <dir>        Output directory for HLS files (must be served by nginx)\n");
    printf("\nOptional:\n");
    printf("  -d, --duration <sec>      Segment duration in seconds (default: %d)\n", DEFAULT_SEGMENT_DURATION);
    printf("  -n, --segments <num>      Number of segments to keep (default: %d)\n", DEFAULT_SEGMENT_COUNT);
    printf("  -V, --variants <num>      Number of variants for ABR (default: 1 = single stream)\n");
    printf("  -v, --verbose             Verbose output\n");
    printf("  -h, --help                Show this help\n");
    printf("\nOutput files:\n");
    printf("  playlist.m3u8             Main HLS playlist\n");
    printf("  segment-NNNNNN.ts         HLS segments\n");
    printf("  stats.json                Status file (updated every %d seconds)\n", STATS_UPDATE_INTERVAL);
    printf("\nExamples:\n");
    printf("  %s -i 239.1.1.1:5000 -o /var/www/caritrans/public/hls/channel1\n", prog);
    printf("  %s -i :5000 -o /var/www/hls/stream -V 3   # ABR with 3 variants\n", prog);
}

/* Create output directory recursively */
static int create_output_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return 0;  /* Already exists */
        }
        return -1;  /* Exists but not a directory */
    }

    /* Create directory with parents */
    char tmp[MAX_PATH_LEN];
    snprintf(tmp, sizeof(tmp), "%s", path);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755);
}

/* Clean output directory */
static void clean_output_dir(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return;

    struct dirent *entry;
    char filepath[MAX_PATH_LEN * 2];

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        snprintf(filepath, sizeof(filepath), "%s/%s", path, entry->d_name);

        struct stat st;
        if (stat(filepath, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                /* Recursively clean subdirectory */
                clean_output_dir(filepath);
                rmdir(filepath);
            } else {
                unlink(filepath);
            }
        }
    }

    closedir(dir);
}

int main(int argc, char *argv[]) {
    /* Initialize context */
    memset(&ctx, 0, sizeof(ctx));
    ctx.segment_duration = DEFAULT_SEGMENT_DURATION;
    ctx.segment_count = DEFAULT_SEGMENT_COUNT;
    ctx.variants = 1;
    ctx.verbose = 0;

    /* Parse command line */
    static struct option long_options[] = {
        {"input", required_argument, 0, 'i'},
        {"output", required_argument, 0, 'o'},
        {"duration", required_argument, 0, 'd'},
        {"segments", required_argument, 0, 'n'},
        {"variants", required_argument, 0, 'V'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int input_specified = 0;
    int output_specified = 0;

    int opt;
    while ((opt = getopt_long(argc, argv, "i:o:d:n:V:vh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'i': {
                char *colon = strrchr(optarg, ':');
                if (colon) {
                    *colon = '\0';
                    snprintf(ctx.input_address, sizeof(ctx.input_address), "%s", optarg);
                    ctx.input_port = atoi(colon + 1);
                } else {
                    ctx.input_port = atoi(optarg);
                }
                input_specified = 1;
                break;
            }
            case 'o':
                snprintf(ctx.output_dir, sizeof(ctx.output_dir), "%s", optarg);
                output_specified = 1;
                break;
            case 'd':
                ctx.segment_duration = atoi(optarg);
                if (ctx.segment_duration < 1) ctx.segment_duration = 1;
                break;
            case 'n':
                ctx.segment_count = atoi(optarg);
                if (ctx.segment_count < 1) ctx.segment_count = 1;
                break;
            case 'V':
                ctx.variants = atoi(optarg);
                if (ctx.variants < 1) ctx.variants = 1;
                if (ctx.variants > MAX_VARIANTS) ctx.variants = MAX_VARIANTS;
                break;
            case 'v':
                ctx.verbose = 1;
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return (opt == 'h') ? 0 : 1;
        }
    }

    if (!input_specified || !output_specified) {
        fprintf(stderr, "Error: Input (-i) and output directory (-o) are required\n\n");
        print_usage(argv[0]);
        return 1;
    }

    if (ctx.input_port == 0) {
        fprintf(stderr, "Error: Invalid UDP port\n");
        return 1;
    }

    /* Create and clean output directory */
    if (create_output_dir(ctx.output_dir) != 0 && errno != EEXIST) {
        fprintf(stderr, "Failed to create output directory: %s\n", ctx.output_dir);
        return 1;
    }
    clean_output_dir(ctx.output_dir);

    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    ctx.running = 1;
    ctx.start_time = time(NULL);
    ctx.restart_count = 0;

    printf("[HLS] HLS Output Generator starting\n");
    printf("[HLS] UDP Input:    %s:%d\n",
           strlen(ctx.input_address) > 0 ? ctx.input_address : "0.0.0.0",
           ctx.input_port);
    printf("[HLS] Output Dir:   %s\n", ctx.output_dir);
    printf("[HLS] Variants:     %d\n", ctx.variants);
    printf("[HLS] Segment:      %d sec, keep %d\n", ctx.segment_duration, ctx.segment_count);
    printf("[HLS] Stats file:   %s/stats.json\n", ctx.output_dir);

    /* Start FFmpeg */
    if (start_ffmpeg() != 0) {
        fprintf(stderr, "Failed to start FFmpeg\n");
        return 1;
    }

    /* Main loop */
    time_t last_stats_update = 0;

    while (ctx.running) {
        /* Check if FFmpeg is still running */
        if (ctx.ffmpeg_pid > 0) {
            int status;
            pid_t result = waitpid(ctx.ffmpeg_pid, &status, WNOHANG);
            if (result == ctx.ffmpeg_pid) {
                printf("[HLS] FFmpeg exited, restarting...\n");
                ctx.restart_count++;
                sleep(2);
                start_ffmpeg();
            }
        }

        /* Update stats file periodically */
        time_t now = time(NULL);
        if (now - last_stats_update >= STATS_UPDATE_INTERVAL) {
            write_stats_file();
            last_stats_update = now;

            if (ctx.verbose) {
                printf("[HLS] Stats updated\n");
            }
        }

        sleep(1);
    }

    /* Cleanup */
    printf("[HLS] Stopping FFmpeg...\n");
    stop_ffmpeg();

    /* Write final stats */
    write_stats_file();

    /* Clean up HLS files on exit */
    printf("[HLS] Cleaning output directory...\n");
    clean_output_dir(ctx.output_dir);

    printf("[HLS] Shutdown complete.\n");
    return 0;
}
