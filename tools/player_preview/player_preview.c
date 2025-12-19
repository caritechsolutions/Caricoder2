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

typedef struct {
    char input_addr[64];
    int input_port;
    char output_dir[512];
    char playlist_path[512];
    char segment_template[512];
    int api_port;
    int duration;
    int live_segments;
    int segment_count;
    int ready;  // 1 when >= 3 segments available
    time_t last_keepalive;
    volatile int running;
    pid_t tsp_child;
    pthread_mutex_t lock;
} AppContext;

AppContext g_ctx;

void print_help(const char *prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("HLS Preview Generator - Creates HLS stream for web preview\n\n");
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
    printf("  GET  /status     - Get status {segments, ready, playlist}\n");
}

void init_context() {
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.duration = DEFAULT_DURATION;
    g_ctx.live_segments = DEFAULT_LIVE_SEGMENTS;
    g_ctx.last_keepalive = time(NULL);
    g_ctx.running = 1;
    pthread_mutex_init(&g_ctx.lock, NULL);
}

// Count .ts segment files in output directory
int count_segments() {
    DIR *dir = opendir(g_ctx.output_dir);
    if (!dir) return 0;

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            const char *ext = strrchr(entry->d_name, '.');
            if (ext && strcmp(ext, ".ts") == 0) {
                count++;
            }
        }
    }
    closedir(dir);
    return count;
}

// Clear all files in output directory
int clear_output_dir() {
    DIR *dir = opendir(g_ctx.output_dir);
    if (!dir) {
        // Directory doesn't exist, try to create it
        if (mkdir(g_ctx.output_dir, 0755) != 0) {
            fprintf(stderr, "ERROR: Cannot create output directory: %s\n", g_ctx.output_dir);
            return -1;
        }
        fprintf(stderr, "Created output directory: %s\n", g_ctx.output_dir);
        return 0;
    }

    struct dirent *entry;
    char filepath[1024];
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            snprintf(filepath, sizeof(filepath), "%s/%s", g_ctx.output_dir, entry->d_name);
            if (unlink(filepath) != 0) {
                fprintf(stderr, "Warning: Could not delete %s: %s\n", filepath, strerror(errno));
            }
        }
    }
    closedir(dir);
    fprintf(stderr, "Cleared output directory: %s\n", g_ctx.output_dir);
    return 0;
}

void kill_tsp_child() {
    if (g_ctx.tsp_child > 0) {
        fprintf(stderr, "Killing tsp child process %d\n", g_ctx.tsp_child);
        kill(g_ctx.tsp_child, SIGTERM);
        usleep(100000);  // 100ms grace period
        kill(g_ctx.tsp_child, SIGKILL);
        waitpid(g_ctx.tsp_child, NULL, WNOHANG);
        g_ctx.tsp_child = 0;
    }
}

void cleanup_and_exit() {
    fprintf(stderr, "Cleaning up...\n");
    kill_tsp_child();

    // Clear HLS files on exit
    DIR *dir = opendir(g_ctx.output_dir);
    if (dir) {
        struct dirent *entry;
        char filepath[1024];
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type == DT_REG) {
                snprintf(filepath, sizeof(filepath), "%s/%s", g_ctx.output_dir, entry->d_name);
                unlink(filepath);
            }
        }
        closedir(dir);
        fprintf(stderr, "Cleaned up HLS files\n");
    }
}

void signal_handler(int sig) {
    fprintf(stderr, "Received signal %d, shutting down...\n", sig);
    g_ctx.running = 0;
}

void* tsp_manager_thread(void *arg) {
    (void)arg;

    while (g_ctx.running) {
        char input_arg[128];
        snprintf(input_arg, sizeof(input_arg), "%s:%d", g_ctx.input_addr, g_ctx.input_port);

        char duration_str[16], live_str[16];
        snprintf(duration_str, sizeof(duration_str), "%d", g_ctx.duration);
        snprintf(live_str, sizeof(live_str), "%d", g_ctx.live_segments);

        // Build tsp command
        char *argv[] = {
            "tsp",
            "-I", "ip", input_arg,
            "-O", "hls",
            "--live", live_str,
            "--duration", duration_str,
            "--playlist", g_ctx.playlist_path,
            g_ctx.segment_template,
            NULL
        };

        // Log the command
        fprintf(stderr, "Starting tsp:");
        for (int i = 0; argv[i]; i++) {
            fprintf(stderr, " %s", argv[i]);
        }
        fprintf(stderr, "\n");

        // Fork and exec
        pid_t pid = fork();
        if (pid == 0) {
            // Child process
            prctl(PR_SET_PDEATHSIG, SIGKILL);

            // Redirect stderr to /dev/null to reduce noise
            freopen("/dev/null", "w", stderr);

            execvp("tsp", argv);
            perror("execvp tsp failed");
            _exit(1);
        } else if (pid > 0) {
            g_ctx.tsp_child = pid;
            fprintf(stderr, "tsp started with PID %d\n", pid);

            int status;
            waitpid(pid, &status, 0);
            g_ctx.tsp_child = 0;

            if (g_ctx.running) {
                if (WIFEXITED(status)) {
                    fprintf(stderr, "tsp exited with code %d, restarting...\n", WEXITSTATUS(status));
                } else if (WIFSIGNALED(status)) {
                    fprintf(stderr, "tsp killed by signal %d, restarting...\n", WTERMSIG(status));
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
        g_ctx.ready = (g_ctx.segment_count >= 3) ? 1 : 0;
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

    // Handle OPTIONS for CORS preflight
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
        pthread_mutex_unlock(&g_ctx.lock);

        time_t now = time(NULL);
        int ttl = KEEPALIVE_TIMEOUT - (int)(now - last_ka);
        if (ttl < 0) ttl = 0;

        char response[512];
        snprintf(response, sizeof(response),
                "{\"segments\":%d,\"ready\":%s,\"playlist\":\"playlist.m3u8\",\"ttl\":%d}",
                segments, ready ? "true" : "false", ttl);

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

    // Parse command line
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            char *addr_port = argv[++i];
            char *colon = strchr(addr_port, ':');
            if (colon) {
                strncpy(g_ctx.input_addr, addr_port, colon - addr_port);
                g_ctx.input_addr[colon - addr_port] = '\0';
                g_ctx.input_port = atoi(colon + 1);
                has_input = 1;
            }
        } else if (strcmp(argv[i], "--output-dir") == 0 && i + 1 < argc) {
            strncpy(g_ctx.output_dir, argv[++i], sizeof(g_ctx.output_dir) - 1);
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

    // Build paths
    snprintf(g_ctx.playlist_path, sizeof(g_ctx.playlist_path), "%s/playlist.m3u8", g_ctx.output_dir);
    snprintf(g_ctx.segment_template, sizeof(g_ctx.segment_template), "%s/segment.ts", g_ctx.output_dir);

    // Clear/create output directory
    if (clear_output_dir() != 0) {
        return 1;
    }

    // Set up signal handlers
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    fprintf(stderr, "Player Preview Starting\n");
    fprintf(stderr, "Input:  %s:%d\n", g_ctx.input_addr, g_ctx.input_port);
    fprintf(stderr, "Output: %s\n", g_ctx.output_dir);
    fprintf(stderr, "API Port: %d\n", g_ctx.api_port);
    fprintf(stderr, "Segment Duration: %d seconds\n", g_ctx.duration);
    fprintf(stderr, "Live Segments: %d\n", g_ctx.live_segments);
    fprintf(stderr, "Keepalive Timeout: %d seconds\n", KEEPALIVE_TIMEOUT);

    // Start tsp manager thread
    pthread_t tsp_thread;
    pthread_create(&tsp_thread, NULL, tsp_manager_thread, NULL);

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
        pthread_join(tsp_thread, NULL);
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
    kill_tsp_child();
    pthread_cancel(mon_thread);
    pthread_join(tsp_thread, NULL);
    pthread_join(mon_thread, NULL);
    cleanup_and_exit();

    fprintf(stderr, "Cleanup complete\n");
    return 0;
}
