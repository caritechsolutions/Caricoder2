/*
 * HLS Output Server
 *
 * Receives UDP MPEG-TS input, generates HLS segments via FFmpeg,
 * and serves them over HTTP with client tracking.
 * Supports both single-stream and ABR (multi-variant) inputs.
 *
 * Usage: hls_output -i <udp_address>:<port> -p <http_port> -o <output_dir> [options]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <getopt.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <microhttpd.h>

#define MAX_CLIENTS 256
#define MAX_VARIANTS 8
#define MAX_PATH_LEN 512
#define DEFAULT_HTTP_PORT 8080
#define DEFAULT_SEGMENT_DURATION 2
#define DEFAULT_SEGMENT_COUNT 5
#define CLIENT_TIMEOUT 60  /* seconds of inactivity before removing client */

/* Client structure */
typedef struct {
    int active;
    char ip[INET6_ADDRSTRLEN];
    time_t first_seen;
    time_t last_seen;
    uint64_t requests;
    uint64_t bytes_sent;
    uint64_t manifest_requests;
    uint64_t segment_requests;
    pthread_mutex_t lock;
} Client;

/* Application context */
typedef struct {
    /* Configuration */
    char input_address[256];
    uint16_t input_port;
    uint16_t http_port;
    char output_dir[MAX_PATH_LEN];
    char base_url[256];
    int segment_duration;
    int segment_count;
    int variants;
    int verbose;

    /* State */
    volatile int running;
    pid_t ffmpeg_pid;
    Client clients[MAX_CLIENTS];
    int client_count;
    pthread_mutex_t clients_lock;

    /* Statistics */
    uint64_t total_requests;
    uint64_t total_bytes_sent;
    time_t start_time;

    /* HTTP server */
    struct MHD_Daemon *http_daemon;
} AppContext;

static AppContext ctx;

/* Forward declarations */
static void cleanup_old_clients(void);

/* Find or create client by IP */
static int find_or_create_client(const char *ip) {
    pthread_mutex_lock(&ctx.clients_lock);
    time_t now = time(NULL);

    /* Look for existing client */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ctx.clients[i].active && strcmp(ctx.clients[i].ip, ip) == 0) {
            pthread_mutex_unlock(&ctx.clients_lock);
            return i;
        }
    }

    /* Find empty slot */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!ctx.clients[i].active) {
            ctx.clients[i].active = 1;
            snprintf(ctx.clients[i].ip, sizeof(ctx.clients[i].ip), "%s", ip);
            ctx.clients[i].first_seen = now;
            ctx.clients[i].last_seen = now;
            ctx.clients[i].requests = 0;
            ctx.clients[i].bytes_sent = 0;
            ctx.clients[i].manifest_requests = 0;
            ctx.clients[i].segment_requests = 0;
            pthread_mutex_init(&ctx.clients[i].lock, NULL);
            ctx.client_count++;

            if (ctx.verbose) {
                printf("[HLS] New client: %s (slot %d)\n", ip, i);
            }

            pthread_mutex_unlock(&ctx.clients_lock);
            return i;
        }
    }

    pthread_mutex_unlock(&ctx.clients_lock);
    return -1;
}

/* Update client stats */
static void update_client_stats(int client_idx, size_t bytes, int is_manifest) {
    if (client_idx < 0 || client_idx >= MAX_CLIENTS) return;

    Client *client = &ctx.clients[client_idx];
    pthread_mutex_lock(&client->lock);

    client->last_seen = time(NULL);
    client->requests++;
    client->bytes_sent += bytes;
    if (is_manifest) {
        client->manifest_requests++;
    } else {
        client->segment_requests++;
    }

    pthread_mutex_unlock(&client->lock);

    /* Update global stats */
    __sync_fetch_and_add(&ctx.total_requests, 1);
    __sync_fetch_and_add(&ctx.total_bytes_sent, bytes);
}

/* Cleanup inactive clients */
static void cleanup_old_clients(void) {
    pthread_mutex_lock(&ctx.clients_lock);
    time_t now = time(NULL);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ctx.clients[i].active) {
            if (now - ctx.clients[i].last_seen > CLIENT_TIMEOUT) {
                if (ctx.verbose) {
                    printf("[HLS] Client timeout: %s\n", ctx.clients[i].ip);
                }
                ctx.clients[i].active = 0;
                pthread_mutex_destroy(&ctx.clients[i].lock);
                ctx.client_count--;
            }
        }
    }

    pthread_mutex_unlock(&ctx.clients_lock);
}

/* Get MIME type for file */
static const char *get_mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";

    if (strcasecmp(ext, ".m3u8") == 0) {
        return "application/vnd.apple.mpegurl";
    } else if (strcasecmp(ext, ".ts") == 0) {
        return "video/mp2t";
    } else if (strcasecmp(ext, ".m4s") == 0) {
        return "video/iso.segment";
    } else if (strcasecmp(ext, ".mp4") == 0) {
        return "video/mp4";
    }

    return "application/octet-stream";
}

/* Check if path is safe (no directory traversal) */
static int is_safe_path(const char *path) {
    if (strstr(path, "..") != NULL) return 0;
    if (path[0] == '/') path++;  /* skip leading slash */
    if (strstr(path, "//") != NULL) return 0;
    return 1;
}

/* Generate stats JSON */
static char *generate_stats_json(void) {
    char *json = malloc(65536);
    if (!json) return NULL;

    time_t now = time(NULL);
    int offset = 0;

    /* Cleanup old clients first */
    cleanup_old_clients();

    offset += snprintf(json + offset, 65536 - offset,
        "{\n"
        "  \"server\": {\n"
        "    \"uptime\": %ld,\n"
        "    \"http_port\": %d,\n"
        "    \"udp_input\": \"%s:%d\",\n"
        "    \"output_dir\": \"%s\",\n"
        "    \"variants\": %d,\n"
        "    \"segment_duration\": %d,\n"
        "    \"segment_count\": %d,\n"
        "    \"total_requests\": %lu,\n"
        "    \"total_bytes_sent\": %lu\n"
        "  },\n"
        "  \"ffmpeg_running\": %s,\n"
        "  \"client_count\": %d,\n"
        "  \"clients\": [\n",
        (long)(now - ctx.start_time),
        ctx.http_port,
        ctx.input_address,
        ctx.input_port,
        ctx.output_dir,
        ctx.variants,
        ctx.segment_duration,
        ctx.segment_count,
        (unsigned long)ctx.total_requests,
        (unsigned long)ctx.total_bytes_sent,
        (ctx.ffmpeg_pid > 0 && kill(ctx.ffmpeg_pid, 0) == 0) ? "true" : "false",
        ctx.client_count
    );

    pthread_mutex_lock(&ctx.clients_lock);
    int first = 1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ctx.clients[i].active) {
            pthread_mutex_lock(&ctx.clients[i].lock);

            if (!first) {
                offset += snprintf(json + offset, 65536 - offset, ",\n");
            }
            first = 0;

            offset += snprintf(json + offset, 65536 - offset,
                "    {\n"
                "      \"ip\": \"%s\",\n"
                "      \"connected_duration\": %ld,\n"
                "      \"last_seen\": %ld,\n"
                "      \"requests\": %lu,\n"
                "      \"manifest_requests\": %lu,\n"
                "      \"segment_requests\": %lu,\n"
                "      \"bytes_sent\": %lu\n"
                "    }",
                ctx.clients[i].ip,
                (long)(now - ctx.clients[i].first_seen),
                (long)(now - ctx.clients[i].last_seen),
                (unsigned long)ctx.clients[i].requests,
                (unsigned long)ctx.clients[i].manifest_requests,
                (unsigned long)ctx.clients[i].segment_requests,
                (unsigned long)ctx.clients[i].bytes_sent
            );

            pthread_mutex_unlock(&ctx.clients[i].lock);
        }
    }
    pthread_mutex_unlock(&ctx.clients_lock);

    offset += snprintf(json + offset, 65536 - offset, "\n  ]\n}\n");

    return json;
}

/* HTTP request handler */
static enum MHD_Result request_handler(void *cls,
                                        struct MHD_Connection *connection,
                                        const char *url,
                                        const char *method,
                                        const char *version,
                                        const char *upload_data,
                                        size_t *upload_data_size,
                                        void **con_cls) {
    (void)cls;
    (void)version;
    (void)upload_data;
    (void)upload_data_size;
    (void)con_cls;

    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        return MHD_NO;
    }

    /* Get client IP */
    const union MHD_ConnectionInfo *info = MHD_get_connection_info(connection, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
    char client_ip[INET6_ADDRSTRLEN] = "unknown";

    if (info && info->client_addr) {
        if (info->client_addr->sa_family == AF_INET) {
            struct sockaddr_in *addr = (struct sockaddr_in *)info->client_addr;
            inet_ntop(AF_INET, &addr->sin_addr, client_ip, sizeof(client_ip));
        } else if (info->client_addr->sa_family == AF_INET6) {
            struct sockaddr_in6 *addr = (struct sockaddr_in6 *)info->client_addr;
            inet_ntop(AF_INET6, &addr->sin6_addr, client_ip, sizeof(client_ip));
        }
    }

    int client_idx = find_or_create_client(client_ip);

    /* Stats endpoint */
    if (strcmp(url, "/stats") == 0) {
        char *json = generate_stats_json();
        if (!json) {
            return MHD_NO;
        }

        struct MHD_Response *response = MHD_create_response_from_buffer(
            strlen(json), json, MHD_RESPMEM_MUST_FREE);
        MHD_add_response_header(response, "Content-Type", "application/json");
        MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
        enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
        MHD_destroy_response(response);
        return ret;
    }

    /* Root - show info page */
    if (strcmp(url, "/") == 0) {
        char info_page[4096];
        snprintf(info_page, sizeof(info_page),
            "HLS Output Server\n"
            "-----------------\n"
            "Playlist URL: http://<server>:%d/playlist.m3u8\n"
            "Stats URL:    http://<server>:%d/stats\n"
            "Variants:     %d\n"
            "Clients:      %d\n"
            "Uptime:       %ld seconds\n",
            ctx.http_port,
            ctx.http_port,
            ctx.variants,
            ctx.client_count,
            (long)(time(NULL) - ctx.start_time)
        );

        struct MHD_Response *response = MHD_create_response_from_buffer(
            strlen(info_page), info_page, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(response, "Content-Type", "text/plain");
        enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
        MHD_destroy_response(response);
        return ret;
    }

    /* Validate path */
    if (!is_safe_path(url)) {
        const char *msg = "Forbidden";
        struct MHD_Response *response = MHD_create_response_from_buffer(
            strlen(msg), (void *)msg, MHD_RESPMEM_PERSISTENT);
        enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_FORBIDDEN, response);
        MHD_destroy_response(response);
        return ret;
    }

    /* Build file path */
    char filepath[MAX_PATH_LEN * 2];
    const char *request_path = url;
    if (request_path[0] == '/') request_path++;

    snprintf(filepath, sizeof(filepath), "%s/%s", ctx.output_dir, request_path);

    /* Check if file exists */
    struct stat st;
    if (stat(filepath, &st) != 0 || !S_ISREG(st.st_mode)) {
        const char *msg = "Not Found";
        struct MHD_Response *response = MHD_create_response_from_buffer(
            strlen(msg), (void *)msg, MHD_RESPMEM_PERSISTENT);
        enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, response);
        MHD_destroy_response(response);
        return ret;
    }

    /* Open and read file */
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        const char *msg = "Internal Error";
        struct MHD_Response *response = MHD_create_response_from_buffer(
            strlen(msg), (void *)msg, MHD_RESPMEM_PERSISTENT);
        enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, response);
        MHD_destroy_response(response);
        return ret;
    }

    /* Create response from file descriptor */
    struct MHD_Response *response = MHD_create_response_from_fd(st.st_size, fd);
    if (!response) {
        close(fd);
        return MHD_NO;
    }

    /* Set headers */
    const char *mime = get_mime_type(filepath);
    MHD_add_response_header(response, "Content-Type", mime);
    MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
    MHD_add_response_header(response, "Cache-Control", "no-cache, no-store, must-revalidate");

    /* Update client stats */
    int is_manifest = (strstr(filepath, ".m3u8") != NULL);
    update_client_stats(client_idx, st.st_size, is_manifest);

    if (ctx.verbose) {
        printf("[HLS] %s -> %s (%ld bytes)\n", client_ip, url, (long)st.st_size);
    }

    enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);
    return ret;
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
        char segment_pattern[MAX_PATH_LEN];
        char playlist_path[MAX_PATH_LEN];
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
                char var_dir[MAX_PATH_LEN];
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
    printf("HLS Output Server\n");
    printf("Usage: %s -i <udp_input> -p <http_port> -o <output_dir> [options]\n\n", prog);
    printf("Required:\n");
    printf("  -i, --input <addr:port>   UDP input address and port (e.g., 239.1.1.1:5000 or :5000)\n");
    printf("  -p, --port <port>         HTTP server port\n");
    printf("  -o, --output <dir>        Output directory for HLS files\n");
    printf("\nOptional:\n");
    printf("  -d, --duration <sec>      Segment duration in seconds (default: %d)\n", DEFAULT_SEGMENT_DURATION);
    printf("  -n, --segments <num>      Number of segments to keep (default: %d)\n", DEFAULT_SEGMENT_COUNT);
    printf("  -V, --variants <num>      Number of variants for ABR (default: 1 = single stream)\n");
    printf("  -v, --verbose             Verbose output\n");
    printf("  -h, --help                Show this help\n");
    printf("\nExamples:\n");
    printf("  %s -i 239.1.1.1:5000 -p 8080 -o /var/www/hls/channel1\n", prog);
    printf("  %s -i :5000 -p 8080 -o /tmp/hls -V 3   # ABR with 3 variants\n", prog);
}

/* Create output directory */
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
    ctx.http_port = DEFAULT_HTTP_PORT;
    ctx.segment_duration = DEFAULT_SEGMENT_DURATION;
    ctx.segment_count = DEFAULT_SEGMENT_COUNT;
    ctx.variants = 1;
    ctx.verbose = 0;

    /* Parse command line */
    static struct option long_options[] = {
        {"input", required_argument, 0, 'i'},
        {"port", required_argument, 0, 'p'},
        {"output", required_argument, 0, 'o'},
        {"duration", required_argument, 0, 'd'},
        {"segments", required_argument, 0, 'n'},
        {"variants", required_argument, 0, 'V'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int input_specified = 0;
    int port_specified = 0;
    int output_specified = 0;

    int opt;
    while ((opt = getopt_long(argc, argv, "i:p:o:d:n:V:vh", long_options, NULL)) != -1) {
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
            case 'p':
                ctx.http_port = atoi(optarg);
                port_specified = 1;
                break;
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

    if (!input_specified || !port_specified || !output_specified) {
        fprintf(stderr, "Error: Input (-i), port (-p), and output directory (-o) are required\n\n");
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

    /* Initialize clients lock */
    pthread_mutex_init(&ctx.clients_lock, NULL);

    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    ctx.running = 1;
    ctx.start_time = time(NULL);

    /* Start FFmpeg */
    if (start_ffmpeg() != 0) {
        fprintf(stderr, "Failed to start FFmpeg\n");
        return 1;
    }

    /* Start HTTP server with SO_REUSEADDR for quick restarts */
    ctx.http_daemon = MHD_start_daemon(
        MHD_USE_THREAD_PER_CONNECTION | MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_ERROR_LOG,
        ctx.http_port,
        NULL, NULL,
        request_handler, NULL,
        MHD_OPTION_LISTENING_ADDRESS_REUSE, 1,
        MHD_OPTION_END
    );

    if (!ctx.http_daemon) {
        fprintf(stderr, "Failed to start HTTP server on port %d\n", ctx.http_port);
        fprintf(stderr, "Possible causes:\n");
        fprintf(stderr, "  - Port %d is already in use by another process\n", ctx.http_port);
        fprintf(stderr, "  - Insufficient permissions to bind to port %d\n", ctx.http_port);
        fprintf(stderr, "  - libmicrohttpd error (check system logs)\n");
        stop_ffmpeg();
        return 1;
    }

    printf("[HLS] Server listening on port %d\n", ctx.http_port);
    printf("[HLS] Playlist URL: http://0.0.0.0:%d/playlist.m3u8\n", ctx.http_port);
    printf("[HLS] Stats URL:    http://0.0.0.0:%d/stats\n", ctx.http_port);
    printf("[HLS] UDP Input:    %s:%d\n",
           strlen(ctx.input_address) > 0 ? ctx.input_address : "0.0.0.0",
           ctx.input_port);
    printf("[HLS] Variants:     %d\n", ctx.variants);

    /* Main loop */
    while (ctx.running) {
        /* Check if FFmpeg is still running */
        if (ctx.ffmpeg_pid > 0) {
            int status;
            pid_t result = waitpid(ctx.ffmpeg_pid, &status, WNOHANG);
            if (result == ctx.ffmpeg_pid) {
                printf("[HLS] FFmpeg exited, restarting...\n");
                sleep(2);
                start_ffmpeg();
            }
        }

        /* Periodic client cleanup */
        cleanup_old_clients();

        sleep(5);
    }

    /* Cleanup */
    printf("Stopping HTTP server...\n");
    MHD_stop_daemon(ctx.http_daemon);

    printf("Stopping FFmpeg...\n");
    stop_ffmpeg();

    /* Cleanup clients */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ctx.clients[i].active) {
            pthread_mutex_destroy(&ctx.clients[i].lock);
        }
    }
    pthread_mutex_destroy(&ctx.clients_lock);

    printf("Shutdown complete.\n");
    return 0;
}
