/*
 * CariTranscoder - Output Application
 * Copyright (c) 2024 CariTech Solutions
 *
 * Receives UDP input and outputs to SRT (one-to-many listener mode).
 * Designed to receive output from cari-mux, cari-transcoder, or any UDP source.
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
#include <srt/srt.h>

#include "config.h"
#include "logging.h"
#include "ts_packet.h"
#include "license.h"

#define MAX_SRT_CLIENTS 100
#define MAX_EPOLL_EVENTS 64
#define TS_PACKET_BATCH 7           /* 7 TS packets = 1316 bytes per SRT/UDP send */
#define SRT_PAYLOAD_SIZE (TS_PACKET_BATCH * TS_PACKET_SIZE)
#define UDP_BUFFER_SIZE 2097152     /* 2MB UDP receive buffer */

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
    char output_type[32];           /* srt */

    /* UDP input */
    int udp_input_socket;
    char udp_input_address[64];
    int udp_input_port;
    char udp_input_interface[64];

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

    /* SRT accept thread */
    pthread_t srt_accept_thread;
    volatile int srt_accept_running;

    /* Statistics */
    uint64_t packets_received;
    uint64_t packets_sent;
    uint64_t bytes_received;
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
            config_get_string(&state->config, "output", "type", "srt"),
            sizeof(state->output_type) - 1);

    /* UDP input configuration */
    strncpy(state->udp_input_address,
            config_get_string(&state->config, "input", "address", ""),
            sizeof(state->udp_input_address) - 1);

    state->udp_input_port = config_get_int(&state->config, "input", "port", 5000);

    strncpy(state->udp_input_interface,
            config_get_string(&state->config, "input", "interface", ""),
            sizeof(state->udp_input_interface) - 1);

    /* SRT output configuration */
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
    CARI_LOG_INFO("UDP Input: %s:%d", state->udp_input_address, state->udp_input_port);

    return 0;
}

/* Initialize UDP input socket */
static int init_udp_input(output_state_t *state) {
    state->udp_input_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (state->udp_input_socket < 0) {
        CARI_LOG_ERROR("Failed to create UDP socket: %s", strerror(errno));
        return -1;
    }

    /* Allow address reuse */
    int reuse = 1;
    setsockopt(state->udp_input_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    /* Set receive buffer size */
    int bufsize = UDP_BUFFER_SIZE;
    setsockopt(state->udp_input_socket, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));

    /* Bind to address */
    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(state->udp_input_port);

    /* Check if multicast */
    struct in_addr mcast_addr;
    int is_multicast = 0;
    if (state->udp_input_address[0] != '\0') {
        inet_aton(state->udp_input_address, &mcast_addr);
        is_multicast = IN_MULTICAST(ntohl(mcast_addr.s_addr));
    }

    if (is_multicast) {
        /* Multicast - bind to INADDR_ANY */
        bind_addr.sin_addr.s_addr = INADDR_ANY;
    } else if (state->udp_input_address[0] != '\0') {
        /* Unicast - bind to specific address */
        inet_aton(state->udp_input_address, &bind_addr.sin_addr);
    } else {
        /* No address specified - bind to any */
        bind_addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(state->udp_input_socket, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        CARI_LOG_ERROR("Failed to bind UDP socket to port %d: %s",
                      state->udp_input_port, strerror(errno));
        close(state->udp_input_socket);
        return -1;
    }

    /* Join multicast group if needed */
    if (is_multicast) {
        struct ip_mreq mreq;
        mreq.imr_multiaddr = mcast_addr;

        if (state->udp_input_interface[0] != '\0') {
            inet_aton(state->udp_input_interface, &mreq.imr_interface);
        } else {
            mreq.imr_interface.s_addr = INADDR_ANY;
        }

        if (setsockopt(state->udp_input_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       &mreq, sizeof(mreq)) < 0) {
            CARI_LOG_ERROR("Failed to join multicast group %s: %s",
                          state->udp_input_address, strerror(errno));
            close(state->udp_input_socket);
            return -1;
        }
        CARI_LOG_INFO("Joined multicast group: %s:%d",
                      state->udp_input_address, state->udp_input_port);
    } else {
        CARI_LOG_INFO("UDP input listening on port %d", state->udp_input_port);
    }

    return 0;
}

/* Initialize SRT listener */
static int init_srt_output(output_state_t *state) {
    state->srt_listener = srt_create_socket();
    if (state->srt_listener == SRT_INVALID_SOCK) {
        CARI_LOG_ERROR("Failed to create SRT socket: %s", srt_getlasterror_str());
        return -1;
    }

    /* Configure SRT options */
    int yes = 1;
    srt_setsockflag(state->srt_listener, SRTO_RCVSYN, &yes, sizeof(yes));

    /* Set latency */
    srt_setsockflag(state->srt_listener, SRTO_LATENCY, &state->srt_latency, sizeof(state->srt_latency));
    srt_setsockflag(state->srt_listener, SRTO_PEERLATENCY, &state->srt_latency, sizeof(state->srt_latency));

    /* Set payload size for MPEG-TS */
    int payloadsize = SRT_PAYLOAD_SIZE;
    srt_setsockflag(state->srt_listener, SRTO_PAYLOADSIZE, &payloadsize, sizeof(payloadsize));

    /* Set encryption if configured */
    if (state->srt_passphrase[0] != '\0' && state->srt_pbkeylen > 0) {
        srt_setsockflag(state->srt_listener, SRTO_PASSPHRASE,
                       state->srt_passphrase, strlen(state->srt_passphrase));
        srt_setsockflag(state->srt_listener, SRTO_PBKEYLEN,
                       &state->srt_pbkeylen, sizeof(state->srt_pbkeylen));
        CARI_LOG_INFO("SRT encryption enabled (key length: %d)", state->srt_pbkeylen);
    }

    /* Set stream ID if configured */
    if (state->srt_streamid[0] != '\0') {
        srt_setsockflag(state->srt_listener, SRTO_STREAMID,
                       state->srt_streamid, strlen(state->srt_streamid));
    }

    /* Bind to listen address */
    struct sockaddr_in listen_addr;
    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(state->srt_port);
    inet_aton(state->srt_listen_address, &listen_addr.sin_addr);

    if (srt_bind(state->srt_listener, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) == SRT_ERROR) {
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

    /* Create epoll for accepting connections */
    state->srt_epoll = srt_epoll_create();
    if (state->srt_epoll < 0) {
        CARI_LOG_ERROR("Failed to create SRT epoll: %s", srt_getlasterror_str());
        srt_close(state->srt_listener);
        return -1;
    }

    int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    srt_epoll_add_usock(state->srt_epoll, state->srt_listener, &events);

    /* Initialize client list */
    pthread_mutex_init(&state->srt_clients_lock, NULL);
    memset(state->srt_clients, 0, sizeof(state->srt_clients));
    state->srt_client_count = 0;

    CARI_LOG_INFO("SRT listener started on %s:%d (max clients: %d, latency: %dms)",
                  state->srt_listen_address, state->srt_port,
                  state->srt_max_clients, state->srt_latency);

    return 0;
}

/* Add new SRT client */
static int add_srt_client(output_state_t *state, SRTSOCKET client_sock,
                          struct sockaddr_storage *addr, socklen_t addr_len) {
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
        pthread_mutex_unlock(&state->srt_clients_lock);
        CARI_LOG_WARNING("Maximum clients reached, rejecting connection");
        srt_close(client_sock);
        return -1;
    }

    /* Setup client */
    srt_client_t *client = &state->srt_clients[slot];
    client->socket = client_sock;
    memcpy(&client->addr, addr, addr_len);
    client->addr_len = addr_len;
    client->connected_at = time(NULL);
    client->bytes_sent = 0;
    client->packets_sent = 0;
    client->send_errors = 0;
    client->active = true;

    /* Get address string */
    if (addr->ss_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)addr;
        snprintf(client->addr_str, sizeof(client->addr_str), "%s:%d",
                inet_ntoa(sin->sin_addr), ntohs(sin->sin_port));
    } else {
        snprintf(client->addr_str, sizeof(client->addr_str), "unknown");
    }

    state->srt_client_count++;

    CARI_LOG_INFO("SRT client connected: %s (slot %d, total: %d)",
                  client->addr_str, slot, state->srt_client_count);

    pthread_mutex_unlock(&state->srt_clients_lock);
    return 0;
}

/* Remove SRT client */
static void remove_srt_client(output_state_t *state, int slot) {
    pthread_mutex_lock(&state->srt_clients_lock);

    if (slot >= 0 && slot < MAX_SRT_CLIENTS && state->srt_clients[slot].active) {
        srt_client_t *client = &state->srt_clients[slot];

        CARI_LOG_INFO("SRT client disconnected: %s (sent %lu packets, %lu errors)",
                      client->addr_str, client->packets_sent, client->send_errors);

        srt_close(client->socket);
        client->active = false;
        state->srt_client_count--;
    }

    pthread_mutex_unlock(&state->srt_clients_lock);
}

/* Send data to all SRT clients */
static int send_to_srt_clients(output_state_t *state, const uint8_t *data, int len) {
    if (state->srt_client_count == 0) {
        return 0;  /* No clients connected */
    }

    pthread_mutex_lock(&state->srt_clients_lock);

    int sent_count = 0;
    for (int i = 0; i < MAX_SRT_CLIENTS; i++) {
        if (!state->srt_clients[i].active) continue;

        srt_client_t *client = &state->srt_clients[i];
        int result = srt_send(client->socket, (const char *)data, len);

        if (result == SRT_ERROR) {
            int err = srt_getlasterror(NULL);
            client->send_errors++;

            /* Connection lost */
            if (err == SRT_ECONNLOST || err == SRT_EINVSOCK || err == SRT_ENOCONN) {
                pthread_mutex_unlock(&state->srt_clients_lock);
                remove_srt_client(state, i);
                pthread_mutex_lock(&state->srt_clients_lock);
            }
        } else {
            client->bytes_sent += len;
            client->packets_sent++;
            sent_count++;
        }
    }

    pthread_mutex_unlock(&state->srt_clients_lock);
    return sent_count;
}

/* SRT accept thread */
static void *srt_accept_thread_func(void *arg) {
    output_state_t *state = (output_state_t *)arg;
    SRTSOCKET ready[MAX_EPOLL_EVENTS];
    int ready_len;

    CARI_LOG_DEBUG("SRT accept thread started");

    while (state->srt_accept_running) {
        ready_len = MAX_EPOLL_EVENTS;
        int result = srt_epoll_wait(state->srt_epoll, ready, &ready_len, NULL, NULL, 100, NULL, NULL, NULL, NULL);

        if (result < 0) {
            if (srt_getlasterror(NULL) == SRT_ETIMEOUT) {
                continue;
            }
            CARI_LOG_ERROR("SRT epoll error: %s", srt_getlasterror_str());
            break;
        }

        for (int i = 0; i < ready_len; i++) {
            if (ready[i] == state->srt_listener) {
                /* New connection */
                struct sockaddr_storage client_addr;
                int addr_len = sizeof(client_addr);

                SRTSOCKET client_sock = srt_accept(state->srt_listener,
                                                   (struct sockaddr *)&client_addr, &addr_len);
                if (client_sock == SRT_INVALID_SOCK) {
                    CARI_LOG_WARNING("SRT accept failed: %s", srt_getlasterror_str());
                    continue;
                }

                /* Check if we can accept more clients */
                if (state->srt_client_count >= state->srt_max_clients) {
                    CARI_LOG_WARNING("Max clients reached, rejecting connection");
                    srt_close(client_sock);
                    continue;
                }

                add_srt_client(state, client_sock, &client_addr, addr_len);
            }
        }
    }

    CARI_LOG_DEBUG("SRT accept thread stopped");
    return NULL;
}

/* Cleanup SRT resources */
static void cleanup_srt_output(output_state_t *state) {
    /* Close all client connections */
    pthread_mutex_lock(&state->srt_clients_lock);
    for (int i = 0; i < MAX_SRT_CLIENTS; i++) {
        if (state->srt_clients[i].active) {
            srt_close(state->srt_clients[i].socket);
            state->srt_clients[i].active = false;
        }
    }
    state->srt_client_count = 0;
    pthread_mutex_unlock(&state->srt_clients_lock);

    /* Cleanup epoll and listener */
    if (state->srt_epoll >= 0) {
        srt_epoll_release(state->srt_epoll);
    }
    if (state->srt_listener != SRT_INVALID_SOCK) {
        srt_close(state->srt_listener);
    }

    pthread_mutex_destroy(&state->srt_clients_lock);
}

static void print_usage(const char *prog) {
    printf("CariTranscoder Output v2.0.0\n");
    printf("Copyright (c) 2024 CariTech Solutions\n\n");
    printf("Usage: %s [OPTIONS]\n\n", prog);
    printf("Options:\n");
    printf("  -c, --config FILE    Configuration file\n");
    printf("  -d, --debug          Enable debug logging\n");
    printf("  -h, --help           Show this help\n");
    printf("\n");
    printf("Receives UDP input (from mux/transcoder) and outputs to SRT (one-to-many).\n");
}

int main(int argc, char *argv[]) {
    char *config_file = NULL;
    int debug = 0;
    int opt;

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
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    if (!config_file) {
        fprintf(stderr, "Error: Configuration file required (-c)\n");
        print_usage(argv[0]);
        return 1;
    }

    /* Initialize logging */
    log_init(debug ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO, NULL);
    CARI_LOG_INFO("CariTranscoder Output v2.0.0 starting...");

    /* Load configuration */
    strncpy(g_state.config_file, config_file, sizeof(g_state.config_file) - 1);
    if (load_config(&g_state) != 0) {
        CARI_LOG_ERROR("Failed to load configuration");
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Initialize SRT library */
    srt_startup();

    /* Initialize UDP input */
    if (init_udp_input(&g_state) != 0) {
        CARI_LOG_ERROR("Failed to initialize UDP input");
        srt_cleanup();
        return 1;
    }

    /* Initialize SRT output */
    if (init_srt_output(&g_state) != 0) {
        CARI_LOG_ERROR("Failed to initialize SRT output");
        close(g_state.udp_input_socket);
        srt_cleanup();
        return 1;
    }

    /* Start SRT accept thread */
    g_state.srt_accept_running = 1;
    pthread_create(&g_state.srt_accept_thread, NULL, srt_accept_thread_func, &g_state);

    g_state.running = 1;
    g_state.stats_last_report = time(NULL);

    CARI_LOG_INFO("Output %s started - UDP %s:%d -> SRT %s:%d",
                  g_state.name,
                  g_state.udp_input_address[0] ? g_state.udp_input_address : "*",
                  g_state.udp_input_port,
                  g_state.srt_listen_address, g_state.srt_port);

    /* Main receive/forward loop */
    uint8_t recv_buffer[65536];
    uint8_t ts_buffer[SRT_PAYLOAD_SIZE];
    int ts_buffer_len = 0;

    while (g_state.running) {
        /* Receive UDP data */
        ssize_t recv_len = recv(g_state.udp_input_socket, recv_buffer, sizeof(recv_buffer), 0);

        if (recv_len < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            CARI_LOG_ERROR("UDP receive error: %s", strerror(errno));
            break;
        }

        if (recv_len == 0) continue;

        g_state.packets_received++;
        g_state.bytes_received += recv_len;

        /* Buffer TS packets and send in batches */
        int offset = 0;
        while (offset < recv_len) {
            int to_copy = recv_len - offset;
            int space_left = SRT_PAYLOAD_SIZE - ts_buffer_len;

            if (to_copy > space_left) {
                to_copy = space_left;
            }

            memcpy(ts_buffer + ts_buffer_len, recv_buffer + offset, to_copy);
            ts_buffer_len += to_copy;
            offset += to_copy;

            /* Send when buffer is full */
            if (ts_buffer_len >= SRT_PAYLOAD_SIZE) {
                int sent = send_to_srt_clients(&g_state, ts_buffer, ts_buffer_len);
                if (sent > 0) {
                    g_state.packets_sent++;
                    g_state.bytes_sent += ts_buffer_len;
                    g_state.stats_bytes_since_report += ts_buffer_len;
                }
                ts_buffer_len = 0;
            }
        }

        /* Stats reporting */
        time_t now = time(NULL);
        if (now - g_state.stats_last_report >= 5) {
            double mbps = (g_state.stats_bytes_since_report * 8.0) / (5.0 * 1000000.0);
            CARI_LOG_INFO("Stats: recv=%lu pkts, sent=%lu pkts, %.2f Mbps, %d clients",
                          g_state.packets_received, g_state.packets_sent,
                          mbps, g_state.srt_client_count);
            g_state.stats_bytes_since_report = 0;
            g_state.stats_last_report = now;
        }
    }

    /* Shutdown */
    CARI_LOG_INFO("Shutting down...");

    g_state.srt_accept_running = 0;
    pthread_join(g_state.srt_accept_thread, NULL);

    cleanup_srt_output(&g_state);
    close(g_state.udp_input_socket);

    config_free(&g_state.config);

    /* Cleanup SRT library */
    srt_cleanup();

    CARI_LOG_INFO("Shutdown complete");
    log_shutdown();

    return 0;
}
