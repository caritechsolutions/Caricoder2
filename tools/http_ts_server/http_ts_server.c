/*
 * HTTP MPEG-TS Server
 *
 * Receives UDP MPEG-TS input and serves it over HTTP for pull-based clients.
 * Provides client tracking and statistics via JSON API.
 *
 * Usage: http_ts_server -i <udp_address>:<port> -p <http_port> [options]
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
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <microhttpd.h>

#define TS_PACKET_SIZE 188
#define UDP_BUFFER_SIZE (7 * TS_PACKET_SIZE)
#define RING_BUFFER_SIZE (1024 * 1024 * 4)  /* 4MB ring buffer */
#define MAX_CLIENTS 100
#define DEFAULT_HTTP_PORT 8888
#define DEFAULT_STREAM_PATH "/stream"
#define DEFAULT_STATS_PATH "/stats"
#define DEFAULT_MIME_TYPE "video/mp2t"

/* Client structure */
typedef struct {
    int active;
    char ip[INET6_ADDRSTRLEN];
    uint16_t port;
    time_t connect_time;
    uint64_t bytes_sent;
    uint64_t packets_sent;
    size_t read_pos;
    pthread_mutex_t lock;
} Client;

/* Ring buffer structure */
typedef struct {
    uint8_t *data;
    size_t size;
    size_t write_pos;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} RingBuffer;

/* Application context */
typedef struct {
    /* Configuration */
    char udp_address[256];
    uint16_t udp_port;
    uint16_t http_port;
    char stream_path[256];
    char stats_path[256];
    char mime_type[128];
    int chunked_encoding;
    int verbose;

    /* State */
    volatile int running;
    RingBuffer ring;
    Client clients[MAX_CLIENTS];
    int client_count;
    pthread_mutex_t clients_lock;

    /* Statistics */
    uint64_t total_bytes_received;
    uint64_t total_packets_received;
    time_t start_time;

    /* Threads */
    pthread_t udp_thread;
    struct MHD_Daemon *http_daemon;
} AppContext;

static AppContext ctx;

/* Initialize ring buffer */
static int ring_buffer_init(RingBuffer *rb, size_t size) {
    rb->data = malloc(size);
    if (!rb->data) return -1;
    rb->size = size;
    rb->write_pos = 0;
    pthread_mutex_init(&rb->lock, NULL);
    pthread_cond_init(&rb->cond, NULL);
    return 0;
}

/* Cleanup ring buffer */
static void ring_buffer_cleanup(RingBuffer *rb) {
    if (rb->data) {
        free(rb->data);
        rb->data = NULL;
    }
    pthread_mutex_destroy(&rb->lock);
    pthread_cond_destroy(&rb->cond);
}

/* Write to ring buffer */
static void ring_buffer_write(RingBuffer *rb, const uint8_t *data, size_t len) {
    pthread_mutex_lock(&rb->lock);

    size_t space_to_end = rb->size - rb->write_pos;
    if (len <= space_to_end) {
        memcpy(rb->data + rb->write_pos, data, len);
    } else {
        memcpy(rb->data + rb->write_pos, data, space_to_end);
        memcpy(rb->data, data + space_to_end, len - space_to_end);
    }
    rb->write_pos = (rb->write_pos + len) % rb->size;

    pthread_cond_broadcast(&rb->cond);
    pthread_mutex_unlock(&rb->lock);
}

/* Get current write position */
static size_t ring_buffer_get_write_pos(RingBuffer *rb) {
    pthread_mutex_lock(&rb->lock);
    size_t pos = rb->write_pos;
    pthread_mutex_unlock(&rb->lock);
    return pos;
}

/* Read from ring buffer (non-blocking, returns available data) */
static size_t ring_buffer_read(RingBuffer *rb, size_t read_pos, uint8_t *buf, size_t max_len) {
    pthread_mutex_lock(&rb->lock);

    size_t write_pos = rb->write_pos;
    size_t available;

    if (write_pos >= read_pos) {
        available = write_pos - read_pos;
    } else {
        available = rb->size - read_pos + write_pos;
    }

    if (available == 0) {
        pthread_mutex_unlock(&rb->lock);
        return 0;
    }

    size_t to_read = (available < max_len) ? available : max_len;
    size_t space_to_end = rb->size - read_pos;

    if (to_read <= space_to_end) {
        memcpy(buf, rb->data + read_pos, to_read);
    } else {
        memcpy(buf, rb->data + read_pos, space_to_end);
        memcpy(buf + space_to_end, rb->data, to_read - space_to_end);
    }

    pthread_mutex_unlock(&rb->lock);
    return to_read;
}

/* Wait for data in ring buffer */
static int ring_buffer_wait(RingBuffer *rb, int timeout_ms) {
    pthread_mutex_lock(&rb->lock);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }

    int ret = pthread_cond_timedwait(&rb->cond, &rb->lock, &ts);
    pthread_mutex_unlock(&rb->lock);

    return (ret == 0) ? 0 : -1;
}

/* Find or create client slot */
static int find_or_create_client(const char *ip, uint16_t port) {
    pthread_mutex_lock(&ctx.clients_lock);

    /* Look for existing client */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ctx.clients[i].active &&
            strcmp(ctx.clients[i].ip, ip) == 0 &&
            ctx.clients[i].port == port) {
            pthread_mutex_unlock(&ctx.clients_lock);
            return i;
        }
    }

    /* Find empty slot */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!ctx.clients[i].active) {
            ctx.clients[i].active = 1;
            snprintf(ctx.clients[i].ip, sizeof(ctx.clients[i].ip), "%s", ip);
            ctx.clients[i].port = port;
            ctx.clients[i].connect_time = time(NULL);
            ctx.clients[i].bytes_sent = 0;
            ctx.clients[i].packets_sent = 0;
            ctx.clients[i].read_pos = ring_buffer_get_write_pos(&ctx.ring);
            pthread_mutex_init(&ctx.clients[i].lock, NULL);
            ctx.client_count++;

            if (ctx.verbose) {
                printf("[HTTP] Client connected: %s:%d (slot %d)\n", ip, port, i);
            }

            pthread_mutex_unlock(&ctx.clients_lock);
            return i;
        }
    }

    pthread_mutex_unlock(&ctx.clients_lock);
    return -1;
}

/* Remove client */
static void remove_client(int index) {
    pthread_mutex_lock(&ctx.clients_lock);

    if (index >= 0 && index < MAX_CLIENTS && ctx.clients[index].active) {
        if (ctx.verbose) {
            printf("[HTTP] Client disconnected: %s:%d (sent %lu bytes)\n",
                   ctx.clients[index].ip, ctx.clients[index].port,
                   (unsigned long)ctx.clients[index].bytes_sent);
        }

        ctx.clients[index].active = 0;
        pthread_mutex_destroy(&ctx.clients[index].lock);
        ctx.client_count--;
    }

    pthread_mutex_unlock(&ctx.clients_lock);
}

/* UDP receiver thread */
static void *udp_receiver_thread(void *arg) {
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return NULL;
    }

    /* Allow reuse */
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    /* Set receive buffer size */
    int rcvbuf = 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ctx.udp_port);

    /* Check if multicast */
    struct in_addr mcast_addr;
    int is_multicast = 0;
    if (inet_aton(ctx.udp_address, &mcast_addr)) {
        uint32_t ip = ntohl(mcast_addr.s_addr);
        is_multicast = (ip >= 0xE0000000 && ip <= 0xEFFFFFFF);
    }

    if (is_multicast) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (strlen(ctx.udp_address) > 0) {
        inet_aton(ctx.udp_address, &addr.sin_addr);
    } else {
        addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock);
        return NULL;
    }

    /* Join multicast group if needed */
    if (is_multicast) {
        struct ip_mreq mreq;
        mreq.imr_multiaddr = mcast_addr;
        mreq.imr_interface.s_addr = INADDR_ANY;
        if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
            perror("IP_ADD_MEMBERSHIP");
        }
    }

    printf("[UDP] Listening on %s:%d%s\n",
           strlen(ctx.udp_address) ? ctx.udp_address : "0.0.0.0",
           ctx.udp_port,
           is_multicast ? " (multicast)" : "");

    uint8_t buffer[UDP_BUFFER_SIZE];

    while (ctx.running) {
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);

        int ret = select(sock + 1, &fds, NULL, NULL, &tv);
        if (ret <= 0) continue;

        ssize_t len = recv(sock, buffer, sizeof(buffer), 0);
        if (len > 0) {
            ring_buffer_write(&ctx.ring, buffer, len);
            ctx.total_bytes_received += len;
            ctx.total_packets_received++;
        }
    }

    close(sock);
    return NULL;
}

/* Content reader callback for streaming response */
struct StreamContext {
    int client_index;
    uint8_t buffer[UDP_BUFFER_SIZE * 10];
};

static ssize_t stream_reader_callback(void *cls, uint64_t pos, char *buf, size_t max) {
    (void)pos;
    struct StreamContext *sctx = (struct StreamContext *)cls;

    if (!ctx.running || sctx->client_index < 0 || !ctx.clients[sctx->client_index].active) {
        return MHD_CONTENT_READER_END_OF_STREAM;
    }

    Client *client = &ctx.clients[sctx->client_index];

    /* Wait for data with timeout */
    int waited = 0;
    size_t len = 0;
    while (ctx.running && client->active && len == 0 && waited < 10) {
        len = ring_buffer_read(&ctx.ring, client->read_pos, (uint8_t *)buf, max);
        if (len == 0) {
            ring_buffer_wait(&ctx.ring, 100);
            waited++;
        }
    }

    if (len > 0) {
        pthread_mutex_lock(&client->lock);
        client->read_pos = (client->read_pos + len) % ctx.ring.size;
        client->bytes_sent += len;
        client->packets_sent++;
        pthread_mutex_unlock(&client->lock);
        return len;
    }

    if (!ctx.running || !client->active) {
        return MHD_CONTENT_READER_END_OF_STREAM;
    }

    /* Return 0 to indicate no data yet but stream continues */
    return 0;
}

static void stream_free_callback(void *cls) {
    struct StreamContext *sctx = (struct StreamContext *)cls;
    if (sctx) {
        remove_client(sctx->client_index);
        free(sctx);
    }
}

/* Generate stats JSON */
static char *generate_stats_json(void) {
    char *json = malloc(65536);
    if (!json) return NULL;

    time_t now = time(NULL);
    int offset = 0;

    offset += snprintf(json + offset, 65536 - offset,
        "{\n"
        "  \"server\": {\n"
        "    \"uptime\": %ld,\n"
        "    \"http_port\": %d,\n"
        "    \"udp_input\": \"%s:%d\",\n"
        "    \"stream_path\": \"%s\",\n"
        "    \"bytes_received\": %lu,\n"
        "    \"packets_received\": %lu\n"
        "  },\n"
        "  \"client_count\": %d,\n"
        "  \"clients\": [\n",
        (long)(now - ctx.start_time),
        ctx.http_port,
        ctx.udp_address,
        ctx.udp_port,
        ctx.stream_path,
        (unsigned long)ctx.total_bytes_received,
        (unsigned long)ctx.total_packets_received,
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
                "      \"port\": %d,\n"
                "      \"connected_duration\": %ld,\n"
                "      \"bytes_sent\": %lu,\n"
                "      \"packets_sent\": %lu\n"
                "    }",
                ctx.clients[i].ip,
                ctx.clients[i].port,
                (long)(now - ctx.clients[i].connect_time),
                (unsigned long)ctx.clients[i].bytes_sent,
                (unsigned long)ctx.clients[i].packets_sent
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

    if (strcmp(method, "GET") != 0) {
        return MHD_NO;
    }

    /* Get client info */
    const union MHD_ConnectionInfo *info = MHD_get_connection_info(connection, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
    char client_ip[INET6_ADDRSTRLEN] = "unknown";
    uint16_t client_port = 0;

    if (info && info->client_addr) {
        if (info->client_addr->sa_family == AF_INET) {
            struct sockaddr_in *addr = (struct sockaddr_in *)info->client_addr;
            inet_ntop(AF_INET, &addr->sin_addr, client_ip, sizeof(client_ip));
            client_port = ntohs(addr->sin_port);
        } else if (info->client_addr->sa_family == AF_INET6) {
            struct sockaddr_in6 *addr = (struct sockaddr_in6 *)info->client_addr;
            inet_ntop(AF_INET6, &addr->sin6_addr, client_ip, sizeof(client_ip));
            client_port = ntohs(addr->sin6_port);
        }
    }

    /* Stats endpoint */
    if (strcmp(url, ctx.stats_path) == 0) {
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

    /* Stream endpoint */
    if (strcmp(url, ctx.stream_path) == 0) {
        int client_index = find_or_create_client(client_ip, client_port);
        if (client_index < 0) {
            const char *msg = "Too many clients";
            struct MHD_Response *response = MHD_create_response_from_buffer(
                strlen(msg), (void *)msg, MHD_RESPMEM_PERSISTENT);
            enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_SERVICE_UNAVAILABLE, response);
            MHD_destroy_response(response);
            return ret;
        }

        struct StreamContext *sctx = malloc(sizeof(struct StreamContext));
        if (!sctx) {
            remove_client(client_index);
            return MHD_NO;
        }
        sctx->client_index = client_index;

        struct MHD_Response *response = MHD_create_response_from_callback(
            MHD_SIZE_UNKNOWN,
            UDP_BUFFER_SIZE * 10,
            stream_reader_callback,
            sctx,
            stream_free_callback
        );

        MHD_add_response_header(response, "Content-Type", ctx.mime_type);
        MHD_add_response_header(response, "Cache-Control", "no-cache, no-store");
        MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");

        if (ctx.chunked_encoding) {
            MHD_add_response_header(response, "Transfer-Encoding", "chunked");
        }

        enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
        MHD_destroy_response(response);
        return ret;
    }

    /* Root - show basic info */
    if (strcmp(url, "/") == 0) {
        char info_page[4096];
        snprintf(info_page, sizeof(info_page),
            "HTTP MPEG-TS Server\n"
            "-------------------\n"
            "Stream URL: http://<server>:%d%s\n"
            "Stats URL:  http://<server>:%d%s\n"
            "Connected clients: %d\n",
            ctx.http_port, ctx.stream_path,
            ctx.http_port, ctx.stats_path,
            ctx.client_count
        );

        struct MHD_Response *response = MHD_create_response_from_buffer(
            strlen(info_page), info_page, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(response, "Content-Type", "text/plain");
        enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
        MHD_destroy_response(response);
        return ret;
    }

    /* 404 for unknown URLs */
    const char *not_found = "Not Found";
    struct MHD_Response *response = MHD_create_response_from_buffer(
        strlen(not_found), (void *)not_found, MHD_RESPMEM_PERSISTENT);
    enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, response);
    MHD_destroy_response(response);
    return ret;
}

/* Signal handler */
static void signal_handler(int sig) {
    (void)sig;
    printf("\nShutting down...\n");
    ctx.running = 0;
}

/* Print usage */
static void print_usage(const char *prog) {
    printf("HTTP MPEG-TS Server\n");
    printf("Usage: %s -i <udp_input> -p <http_port> [options]\n\n", prog);
    printf("Required:\n");
    printf("  -i, --input <addr:port>   UDP input address and port (e.g., 239.1.1.1:5000 or :5000)\n");
    printf("  -p, --port <port>         HTTP server port\n");
    printf("\nOptional:\n");
    printf("  -s, --stream-path <path>  Stream URL path (default: %s)\n", DEFAULT_STREAM_PATH);
    printf("  -a, --stats-path <path>   Stats URL path (default: %s)\n", DEFAULT_STATS_PATH);
    printf("  -m, --mime-type <type>    MIME type for stream (default: %s)\n", DEFAULT_MIME_TYPE);
    printf("  -c, --chunked             Use chunked transfer encoding\n");
    printf("  -v, --verbose             Verbose output\n");
    printf("  -h, --help                Show this help\n");
    printf("\nExamples:\n");
    printf("  %s -i 239.1.1.1:5000 -p 8080\n", prog);
    printf("  %s -i :5000 -p 8080 -s /live/stream.ts\n", prog);
}

int main(int argc, char *argv[]) {
    /* Initialize context */
    memset(&ctx, 0, sizeof(ctx));
    ctx.http_port = DEFAULT_HTTP_PORT;
    strncpy(ctx.stream_path, DEFAULT_STREAM_PATH, sizeof(ctx.stream_path) - 1);
    strncpy(ctx.stats_path, DEFAULT_STATS_PATH, sizeof(ctx.stats_path) - 1);
    strncpy(ctx.mime_type, DEFAULT_MIME_TYPE, sizeof(ctx.mime_type) - 1);
    ctx.chunked_encoding = 0;
    ctx.verbose = 0;

    /* Parse command line */
    static struct option long_options[] = {
        {"input", required_argument, 0, 'i'},
        {"port", required_argument, 0, 'p'},
        {"stream-path", required_argument, 0, 's'},
        {"stats-path", required_argument, 0, 'a'},
        {"mime-type", required_argument, 0, 'm'},
        {"chunked", no_argument, 0, 'c'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int udp_specified = 0;
    int http_port_specified = 0;

    int opt;
    while ((opt = getopt_long(argc, argv, "i:p:s:a:m:cvh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'i': {
                /* Parse UDP input address:port */
                char *colon = strrchr(optarg, ':');
                if (colon) {
                    *colon = '\0';
                    strncpy(ctx.udp_address, optarg, sizeof(ctx.udp_address) - 1);
                    ctx.udp_port = atoi(colon + 1);
                } else {
                    ctx.udp_port = atoi(optarg);
                }
                udp_specified = 1;
                break;
            }
            case 'p':
                ctx.http_port = atoi(optarg);
                http_port_specified = 1;
                break;
            case 's':
                strncpy(ctx.stream_path, optarg, sizeof(ctx.stream_path) - 1);
                break;
            case 'a':
                strncpy(ctx.stats_path, optarg, sizeof(ctx.stats_path) - 1);
                break;
            case 'm':
                strncpy(ctx.mime_type, optarg, sizeof(ctx.mime_type) - 1);
                break;
            case 'c':
                ctx.chunked_encoding = 1;
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

    if (!udp_specified || !http_port_specified) {
        fprintf(stderr, "Error: Both UDP input (-i) and HTTP port (-p) are required\n\n");
        print_usage(argv[0]);
        return 1;
    }

    if (ctx.udp_port == 0) {
        fprintf(stderr, "Error: Invalid UDP port\n");
        return 1;
    }

    /* Initialize ring buffer */
    if (ring_buffer_init(&ctx.ring, RING_BUFFER_SIZE) < 0) {
        fprintf(stderr, "Failed to allocate ring buffer\n");
        return 1;
    }

    /* Initialize clients lock */
    pthread_mutex_init(&ctx.clients_lock, NULL);

    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    ctx.running = 1;
    ctx.start_time = time(NULL);

    /* Start UDP receiver thread */
    if (pthread_create(&ctx.udp_thread, NULL, udp_receiver_thread, NULL) != 0) {
        fprintf(stderr, "Failed to create UDP receiver thread\n");
        ring_buffer_cleanup(&ctx.ring);
        return 1;
    }

    /* Start HTTP server */
    ctx.http_daemon = MHD_start_daemon(
        MHD_USE_THREAD_PER_CONNECTION | MHD_USE_INTERNAL_POLLING_THREAD,
        ctx.http_port,
        NULL, NULL,
        request_handler, NULL,
        MHD_OPTION_END
    );

    if (!ctx.http_daemon) {
        fprintf(stderr, "Failed to start HTTP server on port %d\n", ctx.http_port);
        ctx.running = 0;
        pthread_join(ctx.udp_thread, NULL);
        ring_buffer_cleanup(&ctx.ring);
        return 1;
    }

    printf("[HTTP] Server listening on port %d\n", ctx.http_port);
    printf("[HTTP] Stream URL: http://0.0.0.0:%d%s\n", ctx.http_port, ctx.stream_path);
    printf("[HTTP] Stats URL:  http://0.0.0.0:%d%s\n", ctx.http_port, ctx.stats_path);

    /* Main loop - just wait for shutdown */
    while (ctx.running) {
        sleep(1);
    }

    /* Cleanup */
    printf("Stopping HTTP server...\n");
    MHD_stop_daemon(ctx.http_daemon);

    printf("Stopping UDP receiver...\n");
    pthread_join(ctx.udp_thread, NULL);

    /* Cleanup clients */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ctx.clients[i].active) {
            pthread_mutex_destroy(&ctx.clients[i].lock);
        }
    }
    pthread_mutex_destroy(&ctx.clients_lock);

    ring_buffer_cleanup(&ctx.ring);

    printf("Shutdown complete.\n");
    return 0;
}
