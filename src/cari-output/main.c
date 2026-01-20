/*
 * CariTranscoder - Output Application
 * Copyright (c) 2024 CariTech Solutions
 *
 * Reads from ring buffer and outputs to various destinations.
 * Supports UDP multicast and SRT with one-to-many listener mode.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <gst/gst.h>
#include <srt/srt.h>

#include "config.h"
#include "logging.h"
#include "ring_buffer.h"
#include "ts_packet.h"
#include "license.h"

#define MAX_SRT_CLIENTS 100
#define MAX_EPOLL_EVENTS 64
#define TS_PACKET_BATCH 7           /* 7 TS packets = 1316 bytes per SRT/UDP send */
#define SRT_PAYLOAD_SIZE (TS_PACKET_BATCH * TS_PACKET_SIZE)

/* SRT Client state */
typedef struct {
    SRTSOCKET socket;
    struct sockaddr_storage addr;
    socklen_t addr_len;
    char addr_str[64];
    time_t connected_at;
    uint64_t bytes_sent;
    uint64_t packets_sent;
    uint64_t send_errors;
    bool active;
} srt_client_t;

/* Output state */
typedef struct {
    config_t config;
    char config_file[256];
    char id[64];
    char name[128];
    char output_type[32];           /* udp, srt, hls */

    /* Input buffer */
    char input_buffer_name[128];
    ring_buffer_t *input_buffer;

    /* UDP output */
    int udp_socket;
    struct sockaddr_in udp_dest;

    /* SRT output (one-to-many) */
    SRTSOCKET srt_listener;
    int srt_epoll;
    srt_client_t srt_clients[MAX_SRT_CLIENTS];
    int srt_client_count;
    pthread_mutex_t srt_clients_lock;
    char srt_listen_address[64];
    int srt_port;
    int srt_latency;
    char srt_passphrase[256];
    int srt_pbkeylen;
    char srt_streamid[512];
    int srt_max_clients;

    /* Reader thread */
    pthread_t reader_thread;
    volatile int reader_running;

    /* SRT accept thread */
    pthread_t srt_accept_thread;
    volatile int srt_accept_running;

    /* Statistics */
    uint64_t packets_sent;
    uint64_t bytes_sent;
    time_t stats_last_report;
    uint64_t stats_bytes_since_report;

    /* State */
    volatile int running;
    license_info_t license;
} output_state_t;

static output_state_t g_state = {0};

static void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        CARI_LOG_INFO("Received signal %d, shutting down...", signum);
        g_state.running = 0;
        g_state.reader_running = 0;
        g_state.srt_accept_running = 0;
    }
}

static int load_config(output_state_t *state) {
    if (config_load(&state->config, state->config_file) != 0) {
        return -1;
    }

    strncpy(state->id,
            config_get_string(&state->config, "output", "id", "output-001"),
            sizeof(state->id) - 1);

    strncpy(state->name,
            config_get_string(&state->config, "output", "name", "Unnamed Output"),
            sizeof(state->name) - 1);

    strncpy(state->output_type,
            config_get_string(&state->config, "output", "type", "udp"),
            sizeof(state->output_type) - 1);

    strncpy(state->input_buffer_name,
            config_get_string(&state->config, "input", "buffer_name", ""),
            sizeof(state->input_buffer_name) - 1);

    /* SRT configuration */
    strncpy(state->srt_listen_address,
            config_get_string(&state->config, "destination_srt", "listen_address", "0.0.0.0"),
            sizeof(state->srt_listen_address) - 1);

    state->srt_port = config_get_int(&state->config, "destination_srt", "listen_port", 4900);
    state->srt_latency = config_get_int(&state->config, "destination_srt", "latency", 120);
    state->srt_max_clients = config_get_int(&state->config, "destination_srt", "max_clients", 10);
    state->srt_pbkeylen = config_get_int(&state->config, "destination_srt", "pbkeylen", 0);

    strncpy(state->srt_passphrase,
            config_get_string(&state->config, "destination_srt", "passphrase", ""),
            sizeof(state->srt_passphrase) - 1);

    strncpy(state->srt_streamid,
            config_get_string(&state->config, "destination_srt", "streamid", ""),
            sizeof(state->srt_streamid) - 1);

    CARI_LOG_INFO("Configured: %s (%s) - Type: %s", state->name, state->id, state->output_type);

    return 0;
}

/* ============================================
 * UDP Output Functions
 * ============================================ */

static int init_udp_output(output_state_t *state) {
    const char *address = config_get_string(&state->config, "destination", "address", "239.1.1.1");
    int port = config_get_int(&state->config, "destination", "port", 5000);
    int ttl = config_get_int(&state->config, "destination", "ttl", 64);

    state->udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (state->udp_socket < 0) {
        CARI_LOG_ERROR("Failed to create UDP socket: %s", strerror(errno));
        return -1;
    }

    /* Set TTL for multicast */
    if (IN_MULTICAST(ntohl(inet_addr(address)))) {
        unsigned char mc_ttl = ttl;
        setsockopt(state->udp_socket, IPPROTO_IP, IP_MULTICAST_TTL, &mc_ttl, sizeof(mc_ttl));
    }

    /* Set socket buffer size */
    int bufsize = config_get_int(&state->config, "destination", "buffer_size", 2097152);
    setsockopt(state->udp_socket, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));

    memset(&state->udp_dest, 0, sizeof(state->udp_dest));
    state->udp_dest.sin_family = AF_INET;
    state->udp_dest.sin_addr.s_addr = inet_addr(address);
    state->udp_dest.sin_port = htons(port);

    CARI_LOG_INFO("UDP output configured: %s:%d (TTL=%d)", address, port, ttl);

    return 0;
}

static int send_udp_packets(output_state_t *state, ts_packet_raw_t *packets, int count) {
    /* Send as 7 TS packets per UDP datagram (1316 bytes) */
    int sent = 0;
    int i = 0;

    while (i < count) {
        int batch = (count - i > TS_PACKET_BATCH) ? TS_PACKET_BATCH : (count - i);
        ssize_t ret = sendto(state->udp_socket,
                              &packets[i],
                              batch * TS_PACKET_SIZE,
                              0,
                              (struct sockaddr *)&state->udp_dest,
                              sizeof(state->udp_dest));
        if (ret > 0) {
            sent += batch;
            state->bytes_sent += ret;
            state->stats_bytes_since_report += ret;
        }
        i += batch;
    }

    return sent;
}

/* ============================================
 * SRT One-to-Many Output Functions
 * ============================================ */

static int init_srt_output(output_state_t *state) {
    /* Initialize SRT library */
    srt_startup();

    /* Create listener socket */
    state->srt_listener = srt_create_socket();
    if (state->srt_listener == SRT_INVALID_SOCK) {
        CARI_LOG_ERROR("Failed to create SRT socket: %s", srt_getlasterror_str());
        return -1;
    }

    /* Set socket options */
    int yes = 1;
    srt_setsockflag(state->srt_listener, SRTO_SENDER, &yes, sizeof(yes));

    /* Set latency */
    srt_setsockflag(state->srt_listener, SRTO_LATENCY, &state->srt_latency, sizeof(state->srt_latency));

    /* Set payload size */
    int payload_size = SRT_PAYLOAD_SIZE;
    srt_setsockflag(state->srt_listener, SRTO_PAYLOADSIZE, &payload_size, sizeof(payload_size));

    /* Live transmission mode */
    int transtype = SRTT_LIVE;
    srt_setsockflag(state->srt_listener, SRTO_TRANSTYPE, &transtype, sizeof(transtype));

    /* Set encryption if passphrase is provided */
    if (state->srt_passphrase[0] && state->srt_pbkeylen > 0) {
        srt_setsockflag(state->srt_listener, SRTO_PASSPHRASE, state->srt_passphrase, strlen(state->srt_passphrase));
        srt_setsockflag(state->srt_listener, SRTO_PBKEYLEN, &state->srt_pbkeylen, sizeof(state->srt_pbkeylen));
        CARI_LOG_INFO("SRT encryption enabled (keylen=%d)", state->srt_pbkeylen);
    }

    /* Set stream ID if provided */
    if (state->srt_streamid[0]) {
        srt_setsockflag(state->srt_listener, SRTO_STREAMID, state->srt_streamid, strlen(state->srt_streamid));
    }

    /* Bind to address */
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(state->srt_port);
    sa.sin_addr.s_addr = inet_addr(state->srt_listen_address);

    if (srt_bind(state->srt_listener, (struct sockaddr*)&sa, sizeof(sa)) == SRT_ERROR) {
        CARI_LOG_ERROR("Failed to bind SRT socket: %s", srt_getlasterror_str());
        srt_close(state->srt_listener);
        return -1;
    }

    /* Start listening */
    if (srt_listen(state->srt_listener, state->srt_max_clients) == SRT_ERROR) {
        CARI_LOG_ERROR("Failed to listen on SRT socket: %s", srt_getlasterror_str());
        srt_close(state->srt_listener);
        return -1;
    }

    /* Create epoll for connection monitoring */
    state->srt_epoll = srt_epoll_create();
    if (state->srt_epoll < 0) {
        CARI_LOG_ERROR("Failed to create SRT epoll: %s", srt_getlasterror_str());
        srt_close(state->srt_listener);
        return -1;
    }

    /* Add listener to epoll */
    int modes = SRT_EPOLL_IN;
    srt_epoll_add_usock(state->srt_epoll, state->srt_listener, &modes);

    /* Initialize client list */
    pthread_mutex_init(&state->srt_clients_lock, NULL);
    memset(state->srt_clients, 0, sizeof(state->srt_clients));
    state->srt_client_count = 0;

    CARI_LOG_INFO("SRT listener started on %s:%d (max clients: %d, latency: %dms)",
                  state->srt_listen_address, state->srt_port,
                  state->srt_max_clients, state->srt_latency);

    return 0;
}

static void add_srt_client(output_state_t *state, SRTSOCKET sock, struct sockaddr_storage *addr, socklen_t addr_len) {
    pthread_mutex_lock(&state->srt_clients_lock);

    /* Find empty slot */
    int slot = -1;
    for (int i = 0; i < MAX_SRT_CLIENTS; i++) {
        if (!state->srt_clients[i].active) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        CARI_LOG_WARN("No available client slots, rejecting connection");
        srt_close(sock);
        pthread_mutex_unlock(&state->srt_clients_lock);
        return;
    }

    /* Set socket options for sender */
    int yes = 1;
    srt_setsockflag(sock, SRTO_SENDER, &yes, sizeof(yes));

    /* Non-blocking mode */
    int no_block = 0;  /* blocking */
    srt_setsockflag(sock, SRTO_RCVSYN, &no_block, sizeof(no_block));
    srt_setsockflag(sock, SRTO_SNDSYN, &yes, sizeof(yes));

    /* Store client info */
    srt_client_t *client = &state->srt_clients[slot];
    client->socket = sock;
    memcpy(&client->addr, addr, addr_len);
    client->addr_len = addr_len;
    client->connected_at = time(NULL);
    client->bytes_sent = 0;
    client->packets_sent = 0;
    client->send_errors = 0;
    client->active = true;

    /* Format address string */
    if (addr->ss_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in*)addr;
        snprintf(client->addr_str, sizeof(client->addr_str), "%s:%d",
                 inet_ntoa(sin->sin_addr), ntohs(sin->sin_port));
    } else {
        snprintf(client->addr_str, sizeof(client->addr_str), "unknown");
    }

    state->srt_client_count++;

    CARI_LOG_INFO("SRT client connected: %s (slot %d, total: %d)",
                  client->addr_str, slot, state->srt_client_count);

    pthread_mutex_unlock(&state->srt_clients_lock);
}

static void remove_srt_client(output_state_t *state, int slot) {
    pthread_mutex_lock(&state->srt_clients_lock);

    srt_client_t *client = &state->srt_clients[slot];
    if (client->active) {
        CARI_LOG_INFO("SRT client disconnected: %s (sent: %lu packets, %lu bytes, errors: %lu)",
                      client->addr_str, client->packets_sent, client->bytes_sent, client->send_errors);

        srt_close(client->socket);
        client->active = false;
        state->srt_client_count--;
    }

    pthread_mutex_unlock(&state->srt_clients_lock);
}

static void* srt_accept_thread_func(void *arg) {
    output_state_t *state = (output_state_t *)arg;
    SRTSOCKET ready[MAX_EPOLL_EVENTS];
    int rnum = MAX_EPOLL_EVENTS;

    CARI_LOG_DEBUG("SRT accept thread started");

    while (state->srt_accept_running) {
        /* Wait for incoming connections with timeout */
        int n = srt_epoll_wait(state->srt_epoll, ready, &rnum, NULL, NULL, 1000, NULL, NULL, NULL, NULL);

        if (n < 0) {
            if (srt_getlasterror(NULL) == SRT_ETIMEOUT) {
                continue;  /* Timeout, loop again */
            }
            CARI_LOG_ERROR("SRT epoll error: %s", srt_getlasterror_str());
            break;
        }

        for (int i = 0; i < n; i++) {
            if (ready[i] == state->srt_listener) {
                /* New connection */
                struct sockaddr_storage client_addr;
                int addr_len = sizeof(client_addr);

                SRTSOCKET client_sock = srt_accept(state->srt_listener,
                                                    (struct sockaddr*)&client_addr,
                                                    &addr_len);

                if (client_sock != SRT_INVALID_SOCK) {
                    if (state->srt_client_count >= state->srt_max_clients) {
                        CARI_LOG_WARN("Max clients reached (%d), rejecting connection",
                                      state->srt_max_clients);
                        srt_close(client_sock);
                    } else {
                        add_srt_client(state, client_sock, &client_addr, addr_len);
                    }
                }
            }
        }
    }

    CARI_LOG_DEBUG("SRT accept thread stopped");
    return NULL;
}

static int send_srt_packets(output_state_t *state, ts_packet_raw_t *packets, int count) {
    if (state->srt_client_count == 0) {
        return 0;  /* No clients connected */
    }

    int total_sent = 0;

    pthread_mutex_lock(&state->srt_clients_lock);

    /* Send to each client in batches of 7 TS packets */
    int i = 0;
    while (i < count) {
        int batch = (count - i > TS_PACKET_BATCH) ? TS_PACKET_BATCH : (count - i);
        int send_size = batch * TS_PACKET_SIZE;

        for (int c = 0; c < MAX_SRT_CLIENTS; c++) {
            srt_client_t *client = &state->srt_clients[c];
            if (!client->active) continue;

            int ret = srt_send(client->socket, (char*)&packets[i], send_size);

            if (ret == SRT_ERROR) {
                int err = srt_getlasterror(NULL);
                client->send_errors++;

                /* Check for connection errors */
                if (err == SRT_ECONNLOST || err == SRT_EINVSOCK ||
                    err == SRT_ENOCONN || client->send_errors > 100) {
                    /* Mark for removal */
                    pthread_mutex_unlock(&state->srt_clients_lock);
                    remove_srt_client(state, c);
                    pthread_mutex_lock(&state->srt_clients_lock);
                }
            } else {
                client->bytes_sent += ret;
                client->packets_sent += batch;
            }
        }

        state->bytes_sent += send_size;
        state->stats_bytes_since_report += send_size;
        total_sent += batch;
        i += batch;
    }

    pthread_mutex_unlock(&state->srt_clients_lock);

    return total_sent;
}

static void cleanup_srt_output(output_state_t *state) {
    /* Close all clients */
    for (int i = 0; i < MAX_SRT_CLIENTS; i++) {
        if (state->srt_clients[i].active) {
            srt_close(state->srt_clients[i].socket);
            state->srt_clients[i].active = false;
        }
    }

    /* Close listener */
    if (state->srt_listener != SRT_INVALID_SOCK) {
        srt_epoll_remove_usock(state->srt_epoll, state->srt_listener);
        srt_close(state->srt_listener);
        state->srt_listener = SRT_INVALID_SOCK;
    }

    /* Cleanup epoll */
    if (state->srt_epoll >= 0) {
        srt_epoll_release(state->srt_epoll);
        state->srt_epoll = -1;
    }

    pthread_mutex_destroy(&state->srt_clients_lock);

    /* Cleanup SRT library */
    srt_cleanup();
}

/* ============================================
 * Reader Thread (common)
 * ============================================ */

static void* reader_thread_func(void *arg) {
    output_state_t *state = (output_state_t *)arg;
    ts_packet_raw_t packets[TS_PACKET_BATCH];

    CARI_LOG_DEBUG("Reader thread started");

    while (state->reader_running) {
        int count = ring_buffer_read_batch(state->input_buffer, packets, TS_PACKET_BATCH);

        if (count > 0) {
            if (strcmp(state->output_type, "udp") == 0) {
                int sent = send_udp_packets(state, packets, count);
                state->packets_sent += sent;
            } else if (strcmp(state->output_type, "srt") == 0) {
                int sent = send_srt_packets(state, packets, count);
                state->packets_sent += sent;
            }

            ring_buffer_heartbeat(state->input_buffer);
        } else {
            usleep(500);  /* 0.5ms when no data */
        }
    }

    CARI_LOG_DEBUG("Reader thread stopped");
    return NULL;
}

static void print_usage(const char *prog) {
    printf("CariTranscoder Output - v2.0.0\n");
    printf("Supports UDP multicast and SRT one-to-many output\n\n");
    printf("Usage: %s -c <config_file> [options]\n\n", prog);
    printf("Options:\n");
    printf("  -c, --config FILE    Configuration file (required)\n");
    printf("  -d, --debug          Enable debug logging\n");
    printf("  -h, --help           Show this help\n");
}

int main(int argc, char *argv[]) {
    int opt, debug = 0;
    const char *config_file = NULL;

    static struct option long_options[] = {
        {"config", required_argument, 0, 'c'},
        {"debug", no_argument, 0, 'd'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "c:dh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'c': config_file = optarg; break;
            case 'd': debug = 1; break;
            case 'h': print_usage(argv[0]); return 0;
            default: return 1;
        }
    }

    if (!config_file) {
        fprintf(stderr, "Configuration file required\n");
        print_usage(argv[0]);
        return 1;
    }

    log_config_t log_cfg = LOG_CONFIG_DEFAULT;
    strncpy(log_cfg.ident, "cari-output", sizeof(log_cfg.ident));
    if (debug) log_cfg.min_level = LOG_LEVEL_DEBUG;
    log_init(&log_cfg);

    CARI_LOG_INFO("CariTranscoder Output v2.0.0 starting...");

    strncpy(g_state.config_file, config_file, sizeof(g_state.config_file) - 1);
    if (load_config(&g_state) != 0) {
        CARI_LOG_ERROR("Failed to load configuration");
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    gst_init(&argc, &argv);

    /* Open input buffer */
    g_state.input_buffer = ring_buffer_open(g_state.input_buffer_name, NULL, false);
    if (!g_state.input_buffer) {
        CARI_LOG_ERROR("Failed to open input buffer: %s", g_state.input_buffer_name);
        return 1;
    }
    CARI_LOG_INFO("Connected to input buffer: %s", g_state.input_buffer_name);

    /* Initialize output based on type */
    if (strcmp(g_state.output_type, "udp") == 0) {
        if (init_udp_output(&g_state) != 0) {
            ring_buffer_close(g_state.input_buffer, false);
            return 1;
        }
    } else if (strcmp(g_state.output_type, "srt") == 0) {
        if (init_srt_output(&g_state) != 0) {
            ring_buffer_close(g_state.input_buffer, false);
            return 1;
        }
        /* Start SRT accept thread */
        g_state.srt_accept_running = 1;
        pthread_create(&g_state.srt_accept_thread, NULL, srt_accept_thread_func, &g_state);
    } else {
        CARI_LOG_ERROR("Unsupported output type: %s", g_state.output_type);
        ring_buffer_close(g_state.input_buffer, false);
        return 1;
    }

    /* Start reader thread */
    g_state.running = 1;
    g_state.reader_running = 1;
    g_state.stats_last_report = time(NULL);
    pthread_create(&g_state.reader_thread, NULL, reader_thread_func, &g_state);

    CARI_LOG_INFO("Output %s started, type: %s", g_state.id, g_state.output_type);

    /* Main loop - periodically log stats */
    while (g_state.running) {
        sleep(5);

        time_t now = time(NULL);
        double interval = difftime(now, g_state.stats_last_report);
        if (interval < 1.0) interval = 5.0;

        double bitrate_mbps = (g_state.stats_bytes_since_report * 8.0) / (interval * 1000000.0);

        if (strcmp(g_state.output_type, "srt") == 0) {
            CARI_LOG_INFO("Stats: %lu packets, %.2f Mbps, %d clients connected",
                     g_state.packets_sent, bitrate_mbps, g_state.srt_client_count);
        } else {
            CARI_LOG_INFO("Stats: %lu packets, %.2f Mbps",
                     g_state.packets_sent, bitrate_mbps);
        }

        g_state.stats_bytes_since_report = 0;
        g_state.stats_last_report = now;
    }

    /* Cleanup */
    CARI_LOG_INFO("Shutting down...");

    g_state.reader_running = 0;
    pthread_join(g_state.reader_thread, NULL);

    if (strcmp(g_state.output_type, "srt") == 0) {
        g_state.srt_accept_running = 0;
        pthread_join(g_state.srt_accept_thread, NULL);
        cleanup_srt_output(&g_state);
    }

    if (g_state.udp_socket > 0) {
        close(g_state.udp_socket);
    }

    ring_buffer_close(g_state.input_buffer, false);
    config_free(&g_state.config);
    log_shutdown();

    CARI_LOG_INFO("Shutdown complete");

    return 0;
}
