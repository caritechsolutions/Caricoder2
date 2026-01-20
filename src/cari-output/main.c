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
#define API_BUFFER_SIZE 8192        /* HTTP API buffer size */

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

    /* SRT statistics (updated periodically) */
    double rtt_ms;              /* Round-trip time in ms */
    double bandwidth_mbps;      /* Estimated bandwidth in Mbps */
    double send_rate_mbps;      /* Actual send rate in Mbps */
    int negotiated_latency_ms;  /* Negotiated latency in ms */
    int64_t packets_lost;       /* Packets lost (reported by receiver) */
    int64_t packets_retrans;    /* Packets retransmitted */
    int64_t packets_dropped;    /* Packets dropped (too late) */
    int flight_size;            /* Packets in flight */
    int send_buffer_ms;         /* Send buffer level in ms */
    int congestion_window;      /* Congestion window size */
    int64_t bytes_acked;        /* Total bytes acknowledged */
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

    /* HTTP API server */
    int api_port;
    int api_socket;
    pthread_t api_thread;
    volatile int api_running;

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

    /* API configuration */
    state->api_port = config_get_int(&state->config, "output", "api_port", 0);

    CARI_LOG_INFO("Configured: %s (%s) - Type: %s", state->name, state->id, state->output_type);
    CARI_LOG_INFO("UDP Input: %s:%d", state->udp_input_address, state->udp_input_port);
    if (state->api_port > 0) {
        CARI_LOG_INFO("API Port: %d", state->api_port);
    }

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

    /* Add client socket to epoll for monitoring disconnects */
    int events = SRT_EPOLL_ERR;
    srt_epoll_add_usock(state->srt_epoll, client_sock, &events);

    state->srt_client_count++;

    CARI_LOG_INFO("SRT client connected: %s (slot %d, total: %d)",
                  client->addr_str, slot, state->srt_client_count);

    pthread_mutex_unlock(&state->srt_clients_lock);
    return slot;
}

/* Remove SRT client */
static void remove_srt_client(output_state_t *state, int slot) {
    pthread_mutex_lock(&state->srt_clients_lock);

    if (slot >= 0 && slot < MAX_SRT_CLIENTS && state->srt_clients[slot].active) {
        srt_client_t *client = &state->srt_clients[slot];

        /* Get final statistics before disconnecting */
        SRT_TRACEBSTATS stats;
        memset(&stats, 0, sizeof(stats));
        srt_bstats(client->socket, &stats, 0);

        /* Calculate connection duration */
        time_t duration = time(NULL) - client->connected_at;
        int hours = duration / 3600;
        int mins = (duration % 3600) / 60;
        int secs = duration % 60;

        CARI_LOG_INFO("SRT client disconnected: %s", client->addr_str);
        CARI_LOG_INFO("  Final stats: Duration=%02d:%02d:%02d, Sent=%lu pkts, Errs=%lu",
                      hours, mins, secs, client->packets_sent, client->send_errors);
        CARI_LOG_INFO("  SRT stats: RTT=%.1fms, Lost=%ld, Retrans=%ld, Dropped=%ld",
                      stats.msRTT, (long)stats.pktSndLossTotal,
                      (long)stats.pktRetransTotal, (long)stats.pktSndDropTotal);

        /* Remove from epoll before closing */
        srt_epoll_remove_usock(state->srt_epoll, client->socket);
        srt_close(client->socket);
        client->socket = SRT_INVALID_SOCK;
        client->active = false;
        state->srt_client_count--;
    }

    pthread_mutex_unlock(&state->srt_clients_lock);
}

/* Find client slot by socket */
static int find_client_by_socket(output_state_t *state, SRTSOCKET sock) {
    for (int i = 0; i < MAX_SRT_CLIENTS; i++) {
        if (state->srt_clients[i].active && state->srt_clients[i].socket == sock) {
            return i;
        }
    }
    return -1;
}

/* Check all client connections and remove disconnected ones */
static void check_client_connections(output_state_t *state) {
    pthread_mutex_lock(&state->srt_clients_lock);

    for (int i = 0; i < MAX_SRT_CLIENTS; i++) {
        if (!state->srt_clients[i].active) continue;

        SRT_SOCKSTATUS status = srt_getsockstate(state->srt_clients[i].socket);
        if (status == SRTS_BROKEN || status == SRTS_CLOSED || status == SRTS_NONEXIST) {
            pthread_mutex_unlock(&state->srt_clients_lock);
            remove_srt_client(state, i);
            pthread_mutex_lock(&state->srt_clients_lock);
        }
    }

    pthread_mutex_unlock(&state->srt_clients_lock);
}

/* Update SRT statistics for a client */
static void update_client_stats(srt_client_t *client) {
    SRT_TRACEBSTATS stats;
    memset(&stats, 0, sizeof(stats));

    /* Get statistics (clear=0 to keep accumulating, instantaneous=1 for current values) */
    if (srt_bstats(client->socket, &stats, 0) == 0) {
        client->rtt_ms = stats.msRTT;
        client->bandwidth_mbps = stats.mbpsBandwidth;
        client->send_rate_mbps = stats.mbpsSendRate;
        client->negotiated_latency_ms = stats.msSndTsbPdDelay;
        client->packets_lost = stats.pktSndLossTotal;
        client->packets_retrans = stats.pktRetransTotal;
        client->packets_dropped = stats.pktSndDropTotal;
        client->flight_size = stats.pktFlightSize;
        client->send_buffer_ms = stats.msSndBuf;
        client->congestion_window = stats.pktCongestionWindow;
        client->bytes_acked = stats.byteRecvTotal;  /* bytes received by peer (ACKed) */
    }
}

/* Log detailed statistics for all clients */
static void log_client_stats(output_state_t *state) {
    pthread_mutex_lock(&state->srt_clients_lock);

    for (int i = 0; i < MAX_SRT_CLIENTS; i++) {
        if (!state->srt_clients[i].active) continue;

        srt_client_t *client = &state->srt_clients[i];
        update_client_stats(client);

        /* Calculate connection duration */
        time_t duration = time(NULL) - client->connected_at;
        int hours = duration / 3600;
        int mins = (duration % 3600) / 60;
        int secs = duration % 60;

        CARI_LOG_INFO("  Client[%d] %s: RTT=%.1fms, Latency=%dms, BW=%.1f/%.1f Mbps, "
                      "Lost=%ld, Retrans=%ld, Drop=%ld, Flight=%d, Buf=%dms, "
                      "Sent=%lu pkts, Errs=%lu, Time=%02d:%02d:%02d",
                      i, client->addr_str,
                      client->rtt_ms,
                      client->negotiated_latency_ms,
                      client->send_rate_mbps,
                      client->bandwidth_mbps,
                      (long)client->packets_lost,
                      (long)client->packets_retrans,
                      (long)client->packets_dropped,
                      client->flight_size,
                      client->send_buffer_ms,
                      client->packets_sent,
                      client->send_errors,
                      hours, mins, secs);
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

    time_t last_check = time(NULL);

    while (state->srt_accept_running) {
        ready_len = MAX_EPOLL_EVENTS;
        int result = srt_epoll_wait(state->srt_epoll, ready, &ready_len, NULL, NULL, 100, NULL, NULL, NULL, NULL);

        if (result < 0) {
            if (srt_getlasterror(NULL) == SRT_ETIMEOUT) {
                /* Periodically check client connections every second */
                time_t now = time(NULL);
                if (now - last_check >= 1) {
                    check_client_connections(state);
                    last_check = now;
                }
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
            } else {
                /* Client socket event - check if it's a disconnect */
                int slot = find_client_by_socket(state, ready[i]);
                if (slot >= 0) {
                    SRT_SOCKSTATUS status = srt_getsockstate(ready[i]);
                    if (status == SRTS_BROKEN || status == SRTS_CLOSED || status == SRTS_NONEXIST) {
                        remove_srt_client(state, slot);
                    }
                }
            }
        }

        /* Periodic check even when there are events */
        time_t now = time(NULL);
        if (now - last_check >= 1) {
            check_client_connections(state);
            last_check = now;
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

/* ============================================================================
 * HTTP API Server for stats and client management
 * ============================================================================ */

/* Send HTTP response */
static void api_send_response(int client_fd, int status_code, const char *status_text,
                              const char *content_type, const char *body) {
    char header[512];
    int body_len = body ? strlen(body) : 0;

    snprintf(header, sizeof(header),
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %d\r\n"
             "Access-Control-Allow-Origin: *\r\n"
             "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
             "Connection: close\r\n"
             "\r\n",
             status_code, status_text, content_type, body_len);

    send(client_fd, header, strlen(header), 0);
    if (body && body_len > 0) {
        send(client_fd, body, body_len, 0);
    }
}

/* Build JSON for a single client's stats */
static int build_client_json(output_state_t *state, int slot, char *buf, int buf_size) {
    srt_client_t *client = &state->srt_clients[slot];
    if (!client->active) return 0;

    /* Get fresh SRT stats */
    SRT_TRACEBSTATS stats;
    memset(&stats, 0, sizeof(stats));
    srt_bstats(client->socket, &stats, 0);

    time_t duration = time(NULL) - client->connected_at;

    return snprintf(buf, buf_size,
        "{"
        "\"slot\":%d,"
        "\"address\":\"%s\","
        "\"connected_at\":%ld,"
        "\"duration\":%ld,"
        "\"bytes_sent\":%lu,"
        "\"packets_sent\":%lu,"
        "\"send_errors\":%lu,"
        "\"rtt_ms\":%.2f,"
        "\"bandwidth_mbps\":%.2f,"
        "\"send_rate_mbps\":%.2f,"
        "\"negotiated_latency_ms\":%d,"
        "\"packets_lost\":%ld,"
        "\"packets_retrans\":%ld,"
        "\"packets_dropped\":%ld,"
        "\"flight_size\":%d,"
        "\"send_buffer_ms\":%d,"
        "\"congestion_window\":%d"
        "}",
        slot,
        client->addr_str,
        (long)client->connected_at,
        (long)duration,
        client->bytes_sent,
        client->packets_sent,
        client->send_errors,
        stats.msRTT,
        stats.mbpsBandwidth,
        stats.mbpsSendRate,
        stats.msSndTsbPdDelay,
        (long)stats.pktSndLossTotal,
        (long)stats.pktRetransTotal,
        (long)stats.pktSndDropTotal,
        stats.pktFlightSize,
        stats.msSndBuf,
        stats.pktCongestionWindow
    );
}

/* Handle /metrics endpoint - overall output stats */
static void api_handle_metrics(output_state_t *state, int client_fd) {
    char json[2048];

    snprintf(json, sizeof(json),
        "{"
        "\"success\":true,"
        "\"id\":\"%s\","
        "\"name\":\"%s\","
        "\"type\":\"%s\","
        "\"udp_input\":{\"address\":\"%s\",\"port\":%d},"
        "\"srt_output\":{\"address\":\"%s\",\"port\":%d,\"latency\":%d,\"max_clients\":%d},"
        "\"stats\":{"
        "\"packets_received\":%lu,"
        "\"packets_sent\":%lu,"
        "\"bytes_received\":%lu,"
        "\"bytes_sent\":%lu,"
        "\"client_count\":%d"
        "}"
        "}",
        state->id,
        state->name,
        state->output_type,
        state->udp_input_address[0] ? state->udp_input_address : "*",
        state->udp_input_port,
        state->srt_listen_address,
        state->srt_port,
        state->srt_latency,
        state->srt_max_clients,
        state->packets_received,
        state->packets_sent,
        state->bytes_received,
        state->bytes_sent,
        state->srt_client_count
    );

    api_send_response(client_fd, 200, "OK", "application/json", json);
}

/* Handle /clients endpoint - list all connected clients */
static void api_handle_clients(output_state_t *state, int client_fd) {
    char *json = malloc(API_BUFFER_SIZE * 4);  /* Allow for many clients */
    if (!json) {
        api_send_response(client_fd, 500, "Internal Server Error",
                         "application/json", "{\"success\":false,\"error\":\"Memory allocation failed\"}");
        return;
    }

    pthread_mutex_lock(&state->srt_clients_lock);

    int offset = snprintf(json, API_BUFFER_SIZE * 4,
        "{\"success\":true,\"client_count\":%d,\"max_clients\":%d,\"clients\":[",
        state->srt_client_count, state->srt_max_clients);

    int first = 1;
    for (int i = 0; i < MAX_SRT_CLIENTS && offset < API_BUFFER_SIZE * 4 - 1024; i++) {
        if (!state->srt_clients[i].active) continue;

        if (!first) {
            json[offset++] = ',';
        }
        first = 0;

        offset += build_client_json(state, i, json + offset, API_BUFFER_SIZE * 4 - offset - 10);
    }

    pthread_mutex_unlock(&state->srt_clients_lock);

    snprintf(json + offset, API_BUFFER_SIZE * 4 - offset, "]}");

    api_send_response(client_fd, 200, "OK", "application/json", json);
    free(json);
}

/* Handle /client/{slot} endpoint - get single client stats */
static void api_handle_client_info(output_state_t *state, int client_fd, int slot) {
    if (slot < 0 || slot >= MAX_SRT_CLIENTS) {
        api_send_response(client_fd, 400, "Bad Request",
                         "application/json", "{\"success\":false,\"error\":\"Invalid slot\"}");
        return;
    }

    pthread_mutex_lock(&state->srt_clients_lock);

    if (!state->srt_clients[slot].active) {
        pthread_mutex_unlock(&state->srt_clients_lock);
        api_send_response(client_fd, 404, "Not Found",
                         "application/json", "{\"success\":false,\"error\":\"Client not found\"}");
        return;
    }

    char json[1024];
    int len = snprintf(json, sizeof(json), "{\"success\":true,\"client\":");
    len += build_client_json(state, slot, json + len, sizeof(json) - len - 2);
    snprintf(json + len, sizeof(json) - len, "}");

    pthread_mutex_unlock(&state->srt_clients_lock);

    api_send_response(client_fd, 200, "OK", "application/json", json);
}

/* Handle /client/{slot}/kick endpoint - disconnect a client */
static void api_handle_client_kick(output_state_t *state, int client_fd, int slot) {
    if (slot < 0 || slot >= MAX_SRT_CLIENTS) {
        api_send_response(client_fd, 400, "Bad Request",
                         "application/json", "{\"success\":false,\"error\":\"Invalid slot\"}");
        return;
    }

    pthread_mutex_lock(&state->srt_clients_lock);

    if (!state->srt_clients[slot].active) {
        pthread_mutex_unlock(&state->srt_clients_lock);
        api_send_response(client_fd, 404, "Not Found",
                         "application/json", "{\"success\":false,\"error\":\"Client not found\"}");
        return;
    }

    char addr_str[64];
    strncpy(addr_str, state->srt_clients[slot].addr_str, sizeof(addr_str) - 1);
    addr_str[sizeof(addr_str) - 1] = '\0';

    pthread_mutex_unlock(&state->srt_clients_lock);

    /* Remove client (this logs the disconnection) */
    CARI_LOG_INFO("Kicking client in slot %d: %s", slot, addr_str);
    remove_srt_client(state, slot);

    char json[256];
    snprintf(json, sizeof(json),
             "{\"success\":true,\"message\":\"Client kicked\",\"slot\":%d,\"address\":\"%s\"}",
             slot, addr_str);

    api_send_response(client_fd, 200, "OK", "application/json", json);
}

/* Parse HTTP request and route to handler */
static void api_handle_request(output_state_t *state, int client_fd) {
    char buffer[API_BUFFER_SIZE];
    ssize_t len = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

    if (len <= 0) return;
    buffer[len] = '\0';

    /* Parse request line */
    char method[16], path[256];
    if (sscanf(buffer, "%15s %255s", method, path) != 2) {
        api_send_response(client_fd, 400, "Bad Request",
                         "application/json", "{\"success\":false,\"error\":\"Bad request\"}");
        return;
    }

    /* Handle OPTIONS for CORS preflight */
    if (strcmp(method, "OPTIONS") == 0) {
        api_send_response(client_fd, 200, "OK", "text/plain", "");
        return;
    }

    /* Route requests */
    if (strcmp(path, "/metrics") == 0) {
        api_handle_metrics(state, client_fd);
    } else if (strcmp(path, "/clients") == 0) {
        api_handle_clients(state, client_fd);
    } else if (strncmp(path, "/client/", 8) == 0) {
        /* Parse slot number */
        int slot = -1;
        char *slash = strchr(path + 8, '/');

        if (slash && strcmp(slash, "/kick") == 0) {
            /* /client/{slot}/kick */
            *slash = '\0';
            slot = atoi(path + 8);
            if (strcmp(method, "POST") == 0) {
                api_handle_client_kick(state, client_fd, slot);
            } else {
                api_send_response(client_fd, 405, "Method Not Allowed",
                                 "application/json", "{\"success\":false,\"error\":\"Use POST\"}");
            }
        } else {
            /* /client/{slot} */
            slot = atoi(path + 8);
            api_handle_client_info(state, client_fd, slot);
        }
    } else {
        api_send_response(client_fd, 404, "Not Found",
                         "application/json", "{\"success\":false,\"error\":\"Unknown endpoint\"}");
    }
}

/* API server thread */
static void *api_server_thread(void *arg) {
    output_state_t *state = (output_state_t *)arg;

    CARI_LOG_DEBUG("API server thread started on port %d", state->api_port);

    while (state->api_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        /* Accept with timeout using select */
        fd_set readfds;
        struct timeval tv = {1, 0};  /* 1 second timeout */

        FD_ZERO(&readfds);
        FD_SET(state->api_socket, &readfds);

        int ret = select(state->api_socket + 1, &readfds, NULL, NULL, &tv);
        if (ret <= 0) continue;

        int client_fd = accept(state->api_socket, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno != EINTR && errno != EAGAIN) {
                CARI_LOG_WARNING("API accept failed: %s", strerror(errno));
            }
            continue;
        }

        /* Set socket timeout */
        struct timeval client_tv = {5, 0};
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &client_tv, sizeof(client_tv));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &client_tv, sizeof(client_tv));

        api_handle_request(state, client_fd);
        close(client_fd);
    }

    CARI_LOG_DEBUG("API server thread stopped");
    return NULL;
}

/* Initialize API server */
static int init_api_server(output_state_t *state) {
    if (state->api_port <= 0) {
        return 0;  /* API disabled */
    }

    state->api_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (state->api_socket < 0) {
        CARI_LOG_ERROR("Failed to create API socket: %s", strerror(errno));
        return -1;
    }

    int reuse = 1;
    setsockopt(state->api_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");  /* Localhost only for security */
    addr.sin_port = htons(state->api_port);

    if (bind(state->api_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        CARI_LOG_ERROR("Failed to bind API socket to port %d: %s", state->api_port, strerror(errno));
        close(state->api_socket);
        return -1;
    }

    if (listen(state->api_socket, 5) < 0) {
        CARI_LOG_ERROR("Failed to listen on API socket: %s", strerror(errno));
        close(state->api_socket);
        return -1;
    }

    state->api_running = 1;
    if (pthread_create(&state->api_thread, NULL, api_server_thread, state) != 0) {
        CARI_LOG_ERROR("Failed to create API thread: %s", strerror(errno));
        close(state->api_socket);
        return -1;
    }

    CARI_LOG_INFO("API server started on port %d", state->api_port);
    return 0;
}

/* Cleanup API server */
static void cleanup_api_server(output_state_t *state) {
    if (state->api_port <= 0) return;

    state->api_running = 0;
    pthread_join(state->api_thread, NULL);

    if (state->api_socket >= 0) {
        close(state->api_socket);
    }
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
    log_config_t log_cfg = LOG_CONFIG_DEFAULT;
    strncpy(log_cfg.ident, "cari-output", sizeof(log_cfg.ident));
    if (debug) {
        log_cfg.min_level = LOG_LEVEL_DEBUG;
    }
    log_init(&log_cfg);
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

    /* Start API server if configured */
    if (init_api_server(&g_state) != 0) {
        CARI_LOG_WARNING("API server disabled or failed to start");
    }

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

            /* Log detailed per-client statistics if clients are connected */
            if (g_state.srt_client_count > 0) {
                log_client_stats(&g_state);
            }

            g_state.stats_bytes_since_report = 0;
            g_state.stats_last_report = now;
        }
    }

    /* Shutdown */
    CARI_LOG_INFO("Shutting down...");

    /* Stop API server first */
    cleanup_api_server(&g_state);

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
