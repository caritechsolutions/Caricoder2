/*
 * player_preview.c - HLS Preview Generator using FFmpeg
 *
 * Creates HLS stream for web preview with proper keyframe alignment.
 * Supports multi-variant HLS for ABR streams with multiple video qualities.
 * Uses FFmpeg instead of tsp for better codec compatibility.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <errno.h>
#include <microhttpd.h>
#include <pthread.h>

#define KEEPALIVE_TIMEOUT 60  // Seconds before shutdown if no keepalive
#define DEFAULT_DURATION 2    // Segment duration in seconds
#define DEFAULT_LIVE_SEGMENTS 5
#define MAX_VIDEO_STREAMS 8   // Maximum number of video variants supported

typedef struct {
    char input_addr[128];     // Full input URL (udp://addr:port)
    char output_dir[256];
    char playlist_path[280];  // output_dir + "/playlist.m3u8"
    int api_port;
    int duration;
    int live_segments;
    int segment_count;
    int ready;                // 1 when >= 3 segments available
    time_t last_keepalive;
    volatile int running;
    pid_t ffmpeg_child;
    pthread_mutex_t lock;
    int video_stream_count;   // Number of video streams detected
    int probed;               // 1 if stream has been probed
} AppContext;

AppContext g_ctx;

void print_help(const char *prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("HLS Preview Generator (FFmpeg) - Creates HLS stream for web preview\n\n");
    printf("Options:\n");
    printf("  --input ADDRESS:PORT         Input UDP multicast address (required)\n");
    printf("  --output-dir PATH            Output directory for HLS files (required)\n");
    printf("  --api-port PORT              REST API port (required)\n");
    printf("  --duration SECONDS           Segment duration (default: %d)\n", DEFAULT_DURATION);
    printf("  --live-segments COUNT        Number of live segments (default: %d)\n", DEFAULT_LIVE_SEGMENTS);
    printf("  --help                       Show this help\n");
    printf("\nAPI Endpoints:\n");
    printf("  GET  /health     - Health check\n");
    printf("  POST /keepalive  - Keep the preview alive (call every 30s)\n");
    printf("  GET  /status     - Get status {segments, ready, playlist, variants}\n");
    printf("\nFeatures:\n");
    printf("  - Auto-detects ABR streams with multiple video qualities\n");
    printf("  - Generates multi-variant HLS master playlist for quality switching\n");
}

void init_context() {
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.duration = DEFAULT_DURATION;
    g_ctx.live_segments = DEFAULT_LIVE_SEGMENTS;
    g_ctx.last_keepalive = time(NULL);
    g_ctx.running = 1;
    g_ctx.video_stream_count = 1;  // Default to 1
    g_ctx.probed = 0;
    pthread_mutex_init(&g_ctx.lock, NULL);
}

// Probe stream to detect number of video streams
int probe_stream() {
    char cmd[512];
    FILE *fp;
    int count = 0;

    // Use ffprobe to count video streams
    // -v quiet suppresses decode errors, analyzeduration/probesize help with stream detection
    snprintf(cmd, sizeof(cmd),
        "timeout 10 ffprobe -v quiet -select_streams v "
        "-analyzeduration 3000000 -probesize 3000000 "
        "-show_entries stream=index -of csv=p=0 '%s'",
        g_ctx.input_addr);

    fprintf(stderr, "Probing stream for video tracks: %s\n", g_ctx.input_addr);

    fp = popen(cmd, "r");
    if (fp) {
        char line[64];
        while (fgets(line, sizeof(line), fp) != NULL) {
            // Each line contains stream info - count non-empty lines
            // Output may have "stream,N" or "program,stream,N" format
            if (line[0] != '\0' && line[0] != '\n') {
                count++;
            }
        }
        pclose(fp);
    }

    // Divide by 2 if we got duplicate entries (sometimes ffprobe outputs twice)
    // Check if count is even and > 2, which suggests duplicates
    if (count > 2 && count % 2 == 0) {
        // Verify by checking if we got exactly double
        count = count / 2;
    }

    if (count == 0) {
        fprintf(stderr, "Warning: No video streams detected, defaulting to 1\n");
        count = 1;
    } else if (count > MAX_VIDEO_STREAMS) {
        fprintf(stderr, "Warning: Found %d video streams, limiting to %d\n", count, MAX_VIDEO_STREAMS);
        count = MAX_VIDEO_STREAMS;
    }

    fprintf(stderr, "Detected %d video stream(s)\n", count);
    return count;
}

// Count .ts segment files in output directory (recursive for variants)
int count_segments() {
    DIR *dir = opendir(g_ctx.output_dir);
    if (!dir) return 0;

    int count = 0;
    struct dirent *entry;
    char subdir_path[512];

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            const char *ext = strrchr(entry->d_name, '.');
            if (ext && strcmp(ext, ".ts") == 0) {
                count++;
            }
        } else if (entry->d_type == DT_DIR && entry->d_name[0] == 'v') {
            // Check variant subdirectories (v0, v1, v2, etc.)
            snprintf(subdir_path, sizeof(subdir_path), "%s/%s", g_ctx.output_dir, entry->d_name);
            DIR *subdir = opendir(subdir_path);
            if (subdir) {
                struct dirent *subentry;
                while ((subentry = readdir(subdir)) != NULL) {
                    if (subentry->d_type == DT_REG) {
                        const char *ext = strrchr(subentry->d_name, '.');
                        if (ext && strcmp(ext, ".ts") == 0) {
                            count++;
                        }
                    }
                }
                closedir(subdir);
            }
        }
    }
    closedir(dir);
    return count;
}

// Clear output directory including variant subdirectories
int clear_output_dir() {
    DIR *dir = opendir(g_ctx.output_dir);
    if (!dir) {
        // Try to create it
        if (mkdir(g_ctx.output_dir, 0755) != 0) {
            fprintf(stderr, "ERROR: Cannot create output directory: %s\n", g_ctx.output_dir);
            return -1;
        }
        return 0;
    }

    struct dirent *entry;
    char filepath[512];  // output_dir (256) + "/" + d_name (255)

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        snprintf(filepath, sizeof(filepath), "%s/%s", g_ctx.output_dir, entry->d_name);

        if (entry->d_type == DT_DIR) {
            // Recursively clear subdirectory
            DIR *subdir = opendir(filepath);
            if (subdir) {
                struct dirent *subentry;
                char subpath[768];  // filepath (512) + "/" + d_name (255)
                while ((subentry = readdir(subdir)) != NULL) {
                    if (subentry->d_type == DT_REG) {
                        snprintf(subpath, sizeof(subpath), "%s/%s", filepath, subentry->d_name);
                        unlink(subpath);
                    }
                }
                closedir(subdir);
            }
            rmdir(filepath);
        } else if (entry->d_type == DT_REG) {
            unlink(filepath);
        }
    }
    closedir(dir);
    fprintf(stderr, "Cleared output directory: %s\n", g_ctx.output_dir);
    return 0;
}

// Create variant subdirectories for multi-variant HLS
// count = number of video streams, but we need count+1 dirs for audio (v0, v1, ..., vN for audio)
int create_variant_dirs(int video_count) {
    char dirpath[512];
    // Create video_count + 1 directories: v0..v(N-1) for video, vN for audio
    for (int i = 0; i <= video_count; i++) {
        snprintf(dirpath, sizeof(dirpath), "%s/v%d", g_ctx.output_dir, i);
        if (mkdir(dirpath, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "ERROR: Cannot create variant directory: %s\n", dirpath);
            return -1;
        }
    }
    fprintf(stderr, "Created %d variant directories (v0..v%d)\n", video_count + 1, video_count);
    return 0;
}

void kill_ffmpeg_child() {
    if (g_ctx.ffmpeg_child > 0) {
        fprintf(stderr, "Killing ffmpeg child process %d\n", g_ctx.ffmpeg_child);
        kill(g_ctx.ffmpeg_child, SIGTERM);
        usleep(100000);  // 100ms grace period
        kill(g_ctx.ffmpeg_child, SIGKILL);
        waitpid(g_ctx.ffmpeg_child, NULL, WNOHANG);
        g_ctx.ffmpeg_child = 0;
    }
}

void cleanup_and_exit() {
    fprintf(stderr, "Cleaning up...\n");
    kill_ffmpeg_child();

    // Clear HLS files on exit (including subdirectories)
    clear_output_dir();
    fprintf(stderr, "Cleaned up HLS files\n");
}

void signal_handler(int sig) {
    fprintf(stderr, "Received signal %d, shutting down...\n", sig);
    g_ctx.running = 0;
}

// Build and run single-stream ffmpeg command
void run_single_stream_ffmpeg() {
    char duration_str[16], list_size_str[16];
    char segment_pattern[512];

    snprintf(duration_str, sizeof(duration_str), "%d", g_ctx.duration);
    snprintf(list_size_str, sizeof(list_size_str), "%d", g_ctx.live_segments);
    snprintf(segment_pattern, sizeof(segment_pattern), "%s/segment-%%06d.ts", g_ctx.output_dir);

    char *argv[] = {
        "ffmpeg",
        "-hide_banner",
        "-loglevel", "warning",
        "-fflags", "+genpts",
        "-i", g_ctx.input_addr,
        "-c:v", "copy",
        "-c:a", "copy",
        "-f", "hls",
        "-hls_time", duration_str,
        "-hls_list_size", list_size_str,
        "-hls_flags", "delete_segments+independent_segments",
        "-hls_segment_type", "mpegts",
        "-hls_segment_filename", segment_pattern,
        g_ctx.playlist_path,
        NULL
    };

    fprintf(stderr, "Starting ffmpeg (single-stream):");
    for (int i = 0; argv[i]; i++) {
        fprintf(stderr, " %s", argv[i]);
    }
    fprintf(stderr, "\n");

    execvp("ffmpeg", argv);
    perror("execvp ffmpeg failed");
    _exit(1);
}

// Build and run multi-variant ffmpeg command for ABR
void run_multi_variant_ffmpeg(int video_count) {
    // We need to build a dynamic command with variable number of -map options
    char *argv[128];
    int argc = 0;

    char duration_str[16], list_size_str[16];
    char segment_pattern[512];
    char output_pattern[512];
    char var_stream_map[512];
    char input_url[256];
    char map_args[MAX_VIDEO_STREAMS][16];  // video map args

    snprintf(duration_str, sizeof(duration_str), "%d", g_ctx.duration);
    snprintf(list_size_str, sizeof(list_size_str), "%d", g_ctx.live_segments);
    snprintf(segment_pattern, sizeof(segment_pattern), "%s/v%%v/segment-%%06d.ts", g_ctx.output_dir);
    snprintf(output_pattern, sizeof(output_pattern), "%s/v%%v/stream.m3u8", g_ctx.output_dir);

    // Add UDP buffer settings to input URL
    snprintf(input_url, sizeof(input_url), "%s?fifo_size=5000000&overrun_nonfatal=1", g_ctx.input_addr);

    // Build var_stream_map using audio groups:
    // "v:0,agroup:audio v:1,agroup:audio ... a:0,agroup:audio"
    // This allows multiple video variants to share one audio rendition
    var_stream_map[0] = '\0';
    for (int i = 0; i < video_count; i++) {
        char entry[64];
        snprintf(entry, sizeof(entry), "%sv:%d,agroup:audio", (i > 0 ? " " : ""), i);
        strcat(var_stream_map, entry);
    }
    strcat(var_stream_map, " a:0,agroup:audio");

    // Build argument list
    argv[argc++] = "ffmpeg";
    argv[argc++] = "-hide_banner";
    argv[argc++] = "-loglevel";
    argv[argc++] = "warning";
    argv[argc++] = "-fflags";
    argv[argc++] = "+genpts";
    argv[argc++] = "-i";
    argv[argc++] = input_url;

    // Map all video streams first
    for (int i = 0; i < video_count; i++) {
        snprintf(map_args[i], sizeof(map_args[i]), "0:v:%d", i);
        argv[argc++] = "-map";
        argv[argc++] = map_args[i];
    }

    // Map audio once (will be shared via audio group)
    argv[argc++] = "-map";
    argv[argc++] = "0:a:0";

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
    argv[argc++] = "playlist.m3u8";
    argv[argc++] = "-var_stream_map";
    argv[argc++] = var_stream_map;
    argv[argc++] = output_pattern;
    argv[argc] = NULL;

    fprintf(stderr, "Starting ffmpeg (multi-variant, %d streams):", video_count);
    for (int i = 0; argv[i]; i++) {
        fprintf(stderr, " %s", argv[i]);
    }
    fprintf(stderr, "\n");

    execvp("ffmpeg", argv);
    perror("execvp ffmpeg failed");
    _exit(1);
}

void* ffmpeg_manager_thread(void *arg) {
    (void)arg;

    // Probe stream once at startup
    if (!g_ctx.probed) {
        g_ctx.video_stream_count = probe_stream();
        g_ctx.probed = 1;

        // Create variant directories if multi-stream
        if (g_ctx.video_stream_count > 1) {
            if (create_variant_dirs(g_ctx.video_stream_count) != 0) {
                fprintf(stderr, "ERROR: Failed to create variant directories\n");
                g_ctx.running = 0;
                return NULL;
            }
        }
    }

    while (g_ctx.running) {
        // Fork and exec
        pid_t pid = fork();
        if (pid == 0) {
            // Child process
            prctl(PR_SET_PDEATHSIG, SIGKILL);

            if (g_ctx.video_stream_count > 1) {
                run_multi_variant_ffmpeg(g_ctx.video_stream_count);
            } else {
                run_single_stream_ffmpeg();
            }
            // Should never reach here
            _exit(1);
        } else if (pid > 0) {
            g_ctx.ffmpeg_child = pid;
            fprintf(stderr, "ffmpeg started with PID %d\n", pid);

            int status;
            waitpid(pid, &status, 0);
            g_ctx.ffmpeg_child = 0;

            if (g_ctx.running) {
                if (WIFEXITED(status)) {
                    fprintf(stderr, "ffmpeg exited with code %d, restarting...\n", WEXITSTATUS(status));
                } else if (WIFSIGNALED(status)) {
                    fprintf(stderr, "ffmpeg killed by signal %d, restarting...\n", WTERMSIG(status));
                }
                sleep(2);
            }
        } else {
            perror("fork failed");
            sleep(2);
        }
    }

    return NULL;
}

void* monitor_thread(void *arg) {
    (void)arg;

    while (g_ctx.running) {
        pthread_mutex_lock(&g_ctx.lock);
        g_ctx.segment_count = count_segments();
        // For multi-variant, need segments from all streams (video + audio)
        // With N video streams, we have N+1 total streams (including audio)
        int stream_count = (g_ctx.video_stream_count > 1) ? g_ctx.video_stream_count + 1 : 1;
        int min_segments = 3 * stream_count;
        g_ctx.ready = (g_ctx.segment_count >= min_segments) ? 1 : 0;
        pthread_mutex_unlock(&g_ctx.lock);

        usleep(500000);  // Check every 500ms
    }

    return NULL;
}

static enum MHD_Result api_handler(void *cls, struct MHD_Connection *connection,
                      const char *url, const char *method,
                      const char *version, const char *upload_data,
                      size_t *upload_data_size, void **con_cls) {
    (void)cls;
    (void)version;
    (void)upload_data;
    (void)upload_data_size;
    (void)con_cls;

    struct MHD_Response *mhd_response;
    int ret;

    // CORS preflight
    if (strcmp(method, "OPTIONS") == 0) {
        mhd_response = MHD_create_response_from_buffer(0, "", MHD_RESPMEM_PERSISTENT);
        MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");
        MHD_add_response_header(mhd_response, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        MHD_add_response_header(mhd_response, "Access-Control-Allow-Headers", "Content-Type");
        ret = MHD_queue_response(connection, MHD_HTTP_OK, mhd_response);
        MHD_destroy_response(mhd_response);
        return ret;
    }

    if (strcmp(url, "/health") == 0 && strcmp(method, "GET") == 0) {
        const char *response = "{\"status\":\"ok\"}";
        mhd_response = MHD_create_response_from_buffer(
            strlen(response), (void *)response, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(mhd_response, "Content-Type", "application/json");
        MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");
        ret = MHD_queue_response(connection, MHD_HTTP_OK, mhd_response);
        MHD_destroy_response(mhd_response);
        return ret;
    }

    if (strcmp(url, "/keepalive") == 0 && strcmp(method, "POST") == 0) {
        pthread_mutex_lock(&g_ctx.lock);
        g_ctx.last_keepalive = time(NULL);
        pthread_mutex_unlock(&g_ctx.lock);

        const char *response = "{\"status\":\"ok\",\"timeout\":60}";
        mhd_response = MHD_create_response_from_buffer(
            strlen(response), (void *)response, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(mhd_response, "Content-Type", "application/json");
        MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");
        ret = MHD_queue_response(connection, MHD_HTTP_OK, mhd_response);
        MHD_destroy_response(mhd_response);
        return ret;
    }

    if (strcmp(url, "/status") == 0 && strcmp(method, "GET") == 0) {
        pthread_mutex_lock(&g_ctx.lock);
        int segments = g_ctx.segment_count;
        int ready = g_ctx.ready;
        time_t last_ka = g_ctx.last_keepalive;
        int variants = g_ctx.video_stream_count;
        pthread_mutex_unlock(&g_ctx.lock);

        time_t now = time(NULL);
        int ttl = KEEPALIVE_TIMEOUT - (int)(now - last_ka);
        if (ttl < 0) ttl = 0;

        char response[512];
        snprintf(response, sizeof(response),
                "{\"segments\":%d,\"ready\":%s,\"playlist\":\"playlist.m3u8\",\"ttl\":%d,\"variants\":%d}",
                segments, ready ? "true" : "false", ttl, variants);

        mhd_response = MHD_create_response_from_buffer(
            strlen(response), (void *)response, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(mhd_response, "Content-Type", "application/json");
        MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");
        ret = MHD_queue_response(connection, MHD_HTTP_OK, mhd_response);
        MHD_destroy_response(mhd_response);
        return ret;
    }

    const char *response = "{\"error\":\"not found\"}";
    mhd_response = MHD_create_response_from_buffer(
        strlen(response), (void *)response, MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");
    ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, mhd_response);
    MHD_destroy_response(mhd_response);
    return ret;
}

int main(int argc, char *argv[]) {
    init_context();

    int has_input = 0, has_output = 0, has_port = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            // Build UDP URL from address:port
            const char *addr_port = argv[++i];
            snprintf(g_ctx.input_addr, sizeof(g_ctx.input_addr), "udp://%s", addr_port);
            has_input = 1;
        } else if (strcmp(argv[i], "--output-dir") == 0 && i + 1 < argc) {
            snprintf(g_ctx.output_dir, sizeof(g_ctx.output_dir), "%s", argv[++i]);
            has_output = 1;
        } else if (strcmp(argv[i], "--api-port") == 0 && i + 1 < argc) {
            g_ctx.api_port = atoi(argv[++i]);
            has_port = 1;
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            g_ctx.duration = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--live-segments") == 0 && i + 1 < argc) {
            g_ctx.live_segments = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        }
    }

    // Validate required arguments
    if (!has_input || !has_output || !has_port) {
        fprintf(stderr, "ERROR: Missing required arguments\n");
        fprintf(stderr, "  --input ADDRESS:PORT (required)\n");
        fprintf(stderr, "  --output-dir PATH (required)\n");
        fprintf(stderr, "  --api-port PORT (required)\n\n");
        print_help(argv[0]);
        return 1;
    }

    // Build playlist path
    snprintf(g_ctx.playlist_path, sizeof(g_ctx.playlist_path), "%s/playlist.m3u8", g_ctx.output_dir);

    // Clear/create output directory
    if (clear_output_dir() != 0) {
        return 1;
    }

    // Set up signal handlers
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    fprintf(stderr, "Player Preview Starting (FFmpeg)\n");
    fprintf(stderr, "Input:  %s\n", g_ctx.input_addr);
    fprintf(stderr, "Output: %s\n", g_ctx.output_dir);
    fprintf(stderr, "API Port: %d\n", g_ctx.api_port);
    fprintf(stderr, "Segment Duration: %d seconds\n", g_ctx.duration);
    fprintf(stderr, "Live Segments: %d\n", g_ctx.live_segments);
    fprintf(stderr, "Keepalive Timeout: %d seconds\n", KEEPALIVE_TIMEOUT);

    // Start ffmpeg manager thread
    pthread_t ffmpeg_thread;
    pthread_create(&ffmpeg_thread, NULL, ffmpeg_manager_thread, NULL);

    // Start monitor thread
    pthread_t mon_thread;
    pthread_create(&mon_thread, NULL, monitor_thread, NULL);

    // Start HTTP server
    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_SELECT_INTERNALLY,
        g_ctx.api_port,
        NULL, NULL,
        &api_handler, NULL,
        MHD_OPTION_END);

    if (!daemon) {
        fprintf(stderr, "ERROR: Failed to start HTTP server on port %d\n", g_ctx.api_port);
        g_ctx.running = 0;
        pthread_join(ffmpeg_thread, NULL);
        return 1;
    }

    fprintf(stderr, "HTTP server started on port %d\n", g_ctx.api_port);
    fprintf(stderr, "Endpoints: GET /health, POST /keepalive, GET /status\n");

    // Main loop - check keepalive timeout
    while (g_ctx.running) {
        sleep(1);

        pthread_mutex_lock(&g_ctx.lock);
        time_t last_ka = g_ctx.last_keepalive;
        pthread_mutex_unlock(&g_ctx.lock);

        time_t now = time(NULL);
        if (now - last_ka > KEEPALIVE_TIMEOUT) {
            fprintf(stderr, "Keepalive timeout (%ld seconds since last keepalive). Shutting down.\n",
                    now - last_ka);
            g_ctx.running = 0;
        }
    }

    // Cleanup
    fprintf(stderr, "Shutting down...\n");
    MHD_stop_daemon(daemon);
    kill_ffmpeg_child();
    pthread_cancel(mon_thread);
    pthread_join(ffmpeg_thread, NULL);
    pthread_join(mon_thread, NULL);
    cleanup_and_exit();

    fprintf(stderr, "Cleanup complete\n");
    return 0;
}
