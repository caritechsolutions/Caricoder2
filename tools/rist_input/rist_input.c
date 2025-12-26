/*
 * rist_input.c - RIST Input Monitor for CariTranscoder
 *
 * Receives MPEG-TS streams via RIST (Reliable Internet Stream Transport)
 * and outputs to UDP/multicast. Provides REST API for bitrate monitoring.
 *
 * Uses ristreceiver with stdout output piped to tsp for processing.
 *
 * Copyright (c) 2024 CariTech Solutions
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <microhttpd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>

#define MAX_PIDS 32
#define HISTORY_SIZE 17280  // 24 hours * 60 min * 60 sec / 5 sec per sample
#define MAX_LOG_LINE 512

typedef struct {
    time_t timestamp;
    uint32_t bitrate;
} BitrateEntry;

typedef struct {
    uint16_t pid;
    char name[64];
    BitrateEntry history[HISTORY_SIZE];
    int history_index;
    int history_count;
    uint32_t current_bitrate;
    time_t last_update;
} PIDMonitor;

typedef struct {
    // RIST input settings
    char rist_url[1024];        // RIST URL (rist://...)
    int buffer_size;            // Buffer size for retransmissions (ms)
    char secret[256];           // Pre-shared encryption secret
    int encryption_type;        // 0=none, 128=AES-128, 256=AES-256
    int profile;                // 0=simple, 1=main, 2=advanced

    // Output settings (UDP)
    char output_addr[64];
    int output_port;

    // Common settings
    char log_file[256];
    int api_port;
    int rist_metrics_port;      // Port for ristreceiver Prometheus metrics
    uint16_t program_pid;
    uint16_t pids[MAX_PIDS];
    int pid_count;
    int stall_timeout;
    int history_hours;
    PIDMonitor monitors[MAX_PIDS];
    int monitor_count;
    time_t last_data_received;
    volatile int running;
    pid_t rist_child;
    pid_t tsp_child;
    pthread_mutex_t lock;
} AppContext;

AppContext g_ctx;

void print_help(const char *prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("RIST Input Options:\n");
    printf("  --url URL                    RIST URL (rist://host:port) (required)\n");
    printf("  --buffer SIZE                Buffer size in ms (default: 0)\n");
    printf("  --secret PWD                 Pre-shared encryption secret\n");
    printf("  --encryption TYPE            Encryption type: 0, 128, 256 (default: 0)\n");
    printf("  --profile NUM                RIST profile: 0=simple, 1=main, 2=advanced (default: 1)\n");
    printf("\nOutput Options:\n");
    printf("  --output ADDRESS:PORT        Output UDP address (required)\n");
    printf("\nStream Options:\n");
    printf("  --program PID                Program/PMT PID (required)\n");
    printf("  --pids PID1,PID2,...         Video/audio PIDs to monitor (required)\n");
    printf("\nGeneral Options:\n");
    printf("  --log-file FILE              Log file path (default: /tmp/rist_input.log)\n");
    printf("  --api-port PORT              REST API port (default: 8080)\n");
    printf("  --rist-metrics-port PORT     RIST metrics port (default: api-port + 1000)\n");
    printf("  --stall-timeout SECONDS      Stall timeout (default: 30)\n");
    printf("  --history-hours HOURS        History retention (default: 24)\n");
    printf("  --help                       Show this help\n");
    printf("\nExamples:\n");
    printf("  Basic RIST receiver:\n");
    printf("    %s --url rist://sender:4600 \\\n", prog);
    printf("         --output 239.1.1.1:5000 --program 256 --pids 257,258\n");
    printf("\n  With encryption:\n");
    printf("    %s --url rist://sender:4600 --secret mypassword --encryption 128 \\\n", prog);
    printf("         --output 239.1.1.1:5000 --program 256 --pids 257,258\n");
}

int parse_pids(const char *pids_str) {
    char *copy = strdup(pids_str);
    char *token = strtok(copy, ",");
    int count = 0;

    while (token && count < MAX_PIDS) {
        int pid = atoi(token);
        if (pid > 0 && pid < 8192) {
            g_ctx.pids[count] = pid;
            count++;
        }
        token = strtok(NULL, ",");
    }

    free(copy);
    return count;
}

void init_context() {
    memset(&g_ctx, 0, sizeof(g_ctx));
    strcpy(g_ctx.log_file, "/tmp/rist_input.log");
    g_ctx.api_port = 8080;
    g_ctx.rist_metrics_port = 0;  // Will default to api_port + 1000 if not set
    g_ctx.stall_timeout = 30;
    g_ctx.history_hours = 24;
    g_ctx.last_data_received = time(NULL);
    g_ctx.running = 1;
    g_ctx.buffer_size = 0;
    g_ctx.encryption_type = 0;
    g_ctx.profile = 1;  // Main profile by default
    pthread_mutex_init(&g_ctx.lock, NULL);
}

void init_monitors() {
    pthread_mutex_lock(&g_ctx.lock);
    for (int i = 0; i < g_ctx.pid_count && i < MAX_PIDS; i++) {
        g_ctx.monitors[i].pid = g_ctx.pids[i];
        snprintf(g_ctx.monitors[i].name, sizeof(g_ctx.monitors[i].name), "PID_%u", g_ctx.pids[i]);
        g_ctx.monitors[i].history_index = 0;
        g_ctx.monitors[i].history_count = 0;
        g_ctx.monitors[i].current_bitrate = 0;
        g_ctx.monitors[i].last_update = 0;
    }
    g_ctx.monitor_count = g_ctx.pid_count;
    pthread_mutex_unlock(&g_ctx.lock);
}

PIDMonitor* find_monitor(uint16_t pid) {
    for (int i = 0; i < g_ctx.monitor_count; i++) {
        if (g_ctx.monitors[i].pid == pid) {
            return &g_ctx.monitors[i];
        }
    }
    return NULL;
}

void add_bitrate_sample(PIDMonitor *m, time_t ts, uint32_t bitrate) {
    m->history[m->history_index].timestamp = ts;
    m->history[m->history_index].bitrate = bitrate;
    m->history_index = (m->history_index + 1) % HISTORY_SIZE;
    if (m->history_count < HISTORY_SIZE) {
        m->history_count++;
    }
    m->current_bitrate = bitrate;
    m->last_update = ts;
    g_ctx.last_data_received = ts;
}

void signal_handler(int sig) {
    (void)sig;
    fprintf(stderr, "Signal received, shutting down...\n");
    g_ctx.running = 0;
    if (g_ctx.rist_child > 0) {
        kill(g_ctx.rist_child, SIGTERM);
    }
    if (g_ctx.tsp_child > 0) {
        kill(g_ctx.tsp_child, SIGTERM);
    }
}

void* log_parser_thread(void *arg) {
    (void)arg;
    FILE *log = NULL;
    long last_pos = 0;
    char line[MAX_LOG_LINE];

    while (g_ctx.running) {
        if (!log) {
            log = fopen(g_ctx.log_file, "r");
            if (log) {
                fseek(log, 0, SEEK_END);
                last_pos = ftell(log);
            }
        }

        if (log) {
            fseek(log, last_pos, SEEK_SET);
            while (fgets(line, sizeof(line), log)) {
                // Parse tsp bitrate_monitor output format:
                // "* bitrate_monitor: 2025/12/23 06:31:56, PID 0x00D3 (211) bitrate: 2,730,361 bits/s"
                uint16_t pid;

                // Look for bitrate_monitor line and extract PID and bitrate
                char *pid_start = strstr(line, ") bitrate:");
                char *dec_pid = NULL;

                if (pid_start && strstr(line, "* bitrate_monitor:")) {
                    // Find the decimal PID in parentheses before ") bitrate:"
                    char *p = pid_start - 1;
                    while (p > line && *p != '(') p--;
                    if (*p == '(') {
                        dec_pid = p + 1;
                        pid = (uint16_t)atoi(dec_pid);

                        // Extract bitrate value after "bitrate: "
                        char *br_start = pid_start + 10;  // Skip ") bitrate:"
                        while (*br_start == ' ') br_start++;

                        // Copy digits and commas until 'b' or space
                        int j = 0;
                        char clean_bitrate[32];
                        for (int i = 0; br_start[i] && j < 31; i++) {
                            if (br_start[i] >= '0' && br_start[i] <= '9') {
                                clean_bitrate[j++] = br_start[i];
                            } else if (br_start[i] == ',') {
                                continue;  // Skip commas
                            } else {
                                break;  // Stop at space or 'b'
                            }
                        }
                        clean_bitrate[j] = '\0';
                        uint32_t bitrate = atoi(clean_bitrate);

                        pthread_mutex_lock(&g_ctx.lock);
                        PIDMonitor *m = find_monitor(pid);
                        if (m && bitrate > 0) {
                            add_bitrate_sample(m, time(NULL), bitrate);
                        }
                        pthread_mutex_unlock(&g_ctx.lock);
                    }
                }
            }
            last_pos = ftell(log);
        }

        usleep(500000);  // 0.5 second
    }

    if (log) fclose(log);
    return NULL;
}

void* pipeline_manager_thread(void *arg) {
    (void)arg;

    while (g_ctx.running) {
        // Build ristreceiver command string for tsp -I fork
        // Redirect ristreceiver stderr to /dev/null to suppress its log output
        char rist_cmd[2048];
        int cmd_len = snprintf(rist_cmd, sizeof(rist_cmd),
            "ristreceiver -i %s",
            g_ctx.rist_url);

        if (g_ctx.buffer_size > 0) {
            cmd_len += snprintf(rist_cmd + cmd_len, sizeof(rist_cmd) - cmd_len,
                " -b %d", g_ctx.buffer_size);
        }

        if (g_ctx.secret[0]) {
            cmd_len += snprintf(rist_cmd + cmd_len, sizeof(rist_cmd) - cmd_len,
                " -s %s", g_ctx.secret);
        }

        if (g_ctx.encryption_type > 0) {
            cmd_len += snprintf(rist_cmd + cmd_len, sizeof(rist_cmd) - cmd_len,
                " -e %d", g_ctx.encryption_type);
        }

        // Add RIST profile
        cmd_len += snprintf(rist_cmd + cmd_len, sizeof(rist_cmd) - cmd_len,
            " -p %d", g_ctx.profile);

        // Enable RIST metrics HTTP endpoint for stats
        cmd_len += snprintf(rist_cmd + cmd_len, sizeof(rist_cmd) - cmd_len,
            " -M --metrics-http --metrics-port=%d", g_ctx.rist_metrics_port);

        // Add output and redirect stderr to suppress ristreceiver logging
        cmd_len += snprintf(rist_cmd + cmd_len, sizeof(rist_cmd) - cmd_len,
            " -o stdout:// 2>/dev/null");

        fprintf(stderr, "Starting pipeline: tsp -I fork \"%s\" ...\n", rist_cmd);

        // Fork tsp process (tsp will fork ristreceiver internally via -I fork)
        pid_t tsp_pid = fork();
        if (tsp_pid == 0) {
            // Child process - tsp
            prctl(PR_SET_PDEATHSIG, SIGKILL);

            // Redirect stderr to log file
            FILE *log = fopen(g_ctx.log_file, "a");
            if (log) {
                dup2(fileno(log), STDERR_FILENO);
                fclose(log);
            }

            // Build tsp arguments
            char *argv[128];
            int argc = 0;
            static char output_arg[128];
            static char pid_args[MAX_PIDS + 4][16];
            static char monitor_pids[MAX_PIDS][16];

            snprintf(output_arg, sizeof(output_arg), "%s:%d", g_ctx.output_addr, g_ctx.output_port);

            argv[argc++] = "tsp";
            argv[argc++] = "-v";  // Verbose for debugging
            argv[argc++] = "--buffer-size-mb";
            argv[argc++] = "1";   // Small buffer for fast startup
            argv[argc++] = "-I";
            argv[argc++] = "fork";
            argv[argc++] = "--format";
            argv[argc++] = "TS";
            argv[argc++] = rist_cmd;  // The ristreceiver command

            // Filter plugin
            argv[argc++] = "-P";
            argv[argc++] = "filter";

            int pid_idx = 0;

            // Add PIDs 0, 17, program
            snprintf(pid_args[pid_idx], sizeof(pid_args[pid_idx]), "0");
            argv[argc++] = "-p";
            argv[argc++] = pid_args[pid_idx++];

            snprintf(pid_args[pid_idx], sizeof(pid_args[pid_idx]), "17");
            argv[argc++] = "-p";
            argv[argc++] = pid_args[pid_idx++];

            snprintf(pid_args[pid_idx], sizeof(pid_args[pid_idx]), "%u", g_ctx.program_pid);
            argv[argc++] = "-p";
            argv[argc++] = pid_args[pid_idx++];

            // Add video/audio PIDs to filter
            for (int i = 0; i < g_ctx.pid_count; i++) {
                snprintf(pid_args[pid_idx], sizeof(pid_args[pid_idx]), "%u", g_ctx.pids[i]);
                argv[argc++] = "-p";
                argv[argc++] = pid_args[pid_idx++];
            }

            // Add bitrate_monitor plugins for video/audio PIDs
            for (int i = 0; i < g_ctx.pid_count; i++) {
                snprintf(monitor_pids[i], sizeof(monitor_pids[i]), "%u", g_ctx.pids[i]);
                argv[argc++] = "-P";
                argv[argc++] = "bitrate_monitor";
                argv[argc++] = "--pid";
                argv[argc++] = monitor_pids[i];
                argv[argc++] = "--periodic-bitrate";
                argv[argc++] = "5";
            }

            // Output plugin (UDP)
            argv[argc++] = "-O";
            argv[argc++] = "ip";
            argv[argc++] = output_arg;
            argv[argc] = NULL;

            execvp("tsp", argv);
            perror("execvp tsp failed");
            _exit(1);
        } else if (tsp_pid < 0) {
            perror("fork tsp failed");
            sleep(2);
            continue;
        }

        g_ctx.tsp_child = tsp_pid;
        g_ctx.rist_child = 0;  // tsp manages ristreceiver internally
        fprintf(stderr, "tsp started with PID %d\n", tsp_pid);

        // Wait for tsp to exit
        int status;
        waitpid(tsp_pid, &status, 0);
        g_ctx.tsp_child = 0;

        if (g_ctx.running) {
            if (WIFEXITED(status)) {
                fprintf(stderr, "tsp exited with code %d, restarting pipeline...\n",
                        WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                fprintf(stderr, "tsp killed by signal %d, restarting pipeline...\n",
                        WTERMSIG(status));
            }
            sleep(2);
        }
    }

    return NULL;
}

static int api_handler(void *cls, struct MHD_Connection *connection,
                      const char *url, const char *method,
                      const char *version, const char *upload_data,
                      size_t *upload_data_size, void **con_cls) {
    (void)cls;
    (void)version;
    (void)upload_data;
    (void)upload_data_size;
    (void)con_cls;

    if (strcmp(method, "GET") != 0) {
        return MHD_NO;
    }

    char response[16384];
    int status_code = MHD_HTTP_OK;

    if (strcmp(url, "/health") == 0) {
        snprintf(response, sizeof(response), "{\"status\":\"ok\",\"service\":\"rist_input\"}");
    } else if (strcmp(url, "/status") == 0) {
        pthread_mutex_lock(&g_ctx.lock);

        int offset = snprintf(response, sizeof(response),
            "{\"url\":\"%s\",\"output\":\"%s:%d\",\"running\":%s,\"pids\":[",
            g_ctx.rist_url, g_ctx.output_addr, g_ctx.output_port,
            g_ctx.tsp_child > 0 ? "true" : "false");

        for (int i = 0; i < g_ctx.monitor_count; i++) {
            PIDMonitor *m = &g_ctx.monitors[i];
            offset += snprintf(response + offset, sizeof(response) - offset,
                "%s{\"pid\":%u,\"name\":\"%s\",\"bitrate\":%u,\"last_update\":%ld}",
                i > 0 ? "," : "",
                m->pid, m->name, m->current_bitrate, m->last_update);
        }

        offset += snprintf(response + offset, sizeof(response) - offset, "]}");

        pthread_mutex_unlock(&g_ctx.lock);
    } else if (strcmp(url, "/metrics") == 0) {
        pthread_mutex_lock(&g_ctx.lock);

        int offset = snprintf(response, sizeof(response), "{\"status\":\"running\",\"pids\":{");

        for (int i = 0; i < g_ctx.monitor_count; i++) {
            PIDMonitor *m = &g_ctx.monitors[i];
            offset += snprintf(response + offset, sizeof(response) - offset,
                "%s\"%u\":{\"pid\":%u,\"name\":\"%s\",\"current_bitrate\":%u,\"last_update\":%ld,\"sample_count\":%d}",
                i > 0 ? "," : "",
                m->pid, m->pid, m->name, m->current_bitrate, m->last_update, m->history_count);
        }

        offset += snprintf(response + offset, sizeof(response) - offset, "}}");

        pthread_mutex_unlock(&g_ctx.lock);
    } else if (strcmp(url, "/metrics/history") == 0) {
        pthread_mutex_lock(&g_ctx.lock);

        // Calculate required buffer size
        size_t buf_size = 1024 + (g_ctx.monitor_count * HISTORY_SIZE * 30);
        char *hist_response = malloc(buf_size);
        if (!hist_response) {
            pthread_mutex_unlock(&g_ctx.lock);
            snprintf(response, sizeof(response), "{\"error\":\"out of memory\"}");
            struct MHD_Response *mhd_response = MHD_create_response_from_buffer(
                strlen(response), response, MHD_RESPMEM_MUST_COPY);
            MHD_add_response_header(mhd_response, "Content-Type", "application/json");
            int ret = MHD_queue_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, mhd_response);
            MHD_destroy_response(mhd_response);
            return ret;
        }

        strcpy(hist_response, "{\"status\":\"running\",\"pids\":{");

        for (int i = 0; i < g_ctx.monitor_count; i++) {
            PIDMonitor *m = &g_ctx.monitors[i];

            if (i > 0) strcat(hist_response, ",");

            sprintf(hist_response + strlen(hist_response),
                   "\"%u\":{\"pid\":%u,\"name\":\"%s\",\"current_bitrate\":%u,\"history\":[",
                   m->pid, m->pid, m->name, m->current_bitrate);

            // Output history in chronological order (oldest first)
            int start_idx = (m->history_count == HISTORY_SIZE) ? m->history_index : 0;
            int first = 1;

            for (int j = 0; j < m->history_count; j++) {
                int idx = (start_idx + j) % HISTORY_SIZE;
                BitrateEntry *e = &m->history[idx];

                if (!first) strcat(hist_response, ",");
                first = 0;

                sprintf(hist_response + strlen(hist_response), "[%ld,%u]", e->timestamp, e->bitrate);
            }

            strcat(hist_response, "]}");
        }

        strcat(hist_response, "}}");

        pthread_mutex_unlock(&g_ctx.lock);

        struct MHD_Response *mhd_response = MHD_create_response_from_buffer(
            strlen(hist_response), hist_response, MHD_RESPMEM_MUST_FREE);
        MHD_add_response_header(mhd_response, "Content-Type", "application/json");
        MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");
        int ret = MHD_queue_response(connection, MHD_HTTP_OK, mhd_response);
        MHD_destroy_response(mhd_response);
        return ret;
    } else if (strncmp(url, "/history/", 9) == 0) {
        uint16_t pid = atoi(url + 9);

        pthread_mutex_lock(&g_ctx.lock);
        PIDMonitor *m = find_monitor(pid);

        if (m) {
            int offset = snprintf(response, sizeof(response),
                "{\"pid\":%u,\"name\":\"%s\",\"history\":[", m->pid, m->name);

            int max_samples = 1000;
            int start = m->history_count > max_samples ?
                        (m->history_index - max_samples + HISTORY_SIZE) % HISTORY_SIZE : 0;
            int count = m->history_count > max_samples ? max_samples : m->history_count;

            for (int i = 0; i < count; i++) {
                int idx = (start + i) % HISTORY_SIZE;
                BitrateEntry *e = &m->history[idx];
                offset += snprintf(response + offset, sizeof(response) - offset,
                    "%s{\"timestamp\":%ld,\"bitrate\":%u}",
                    i > 0 ? "," : "",
                    e->timestamp, e->bitrate);
            }

            offset += snprintf(response + offset, sizeof(response) - offset, "]}");
        } else {
            snprintf(response, sizeof(response), "{\"error\":\"PID not found\"}");
            status_code = MHD_HTTP_NOT_FOUND;
        }

        pthread_mutex_unlock(&g_ctx.lock);
    } else if (strcmp(url, "/rist-stats") == 0) {
        // Fetch RIST metrics from ristreceiver's Prometheus endpoint
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "curl -s --connect-timeout 2 http://localhost:%d/metrics 2>/dev/null",
                 g_ctx.rist_metrics_port);

        FILE *fp = popen(cmd, "r");
        if (!fp) {
            snprintf(response, sizeof(response), "{\"error\":\"Failed to fetch RIST metrics\"}");
            status_code = MHD_HTTP_INTERNAL_SERVER_ERROR;
        } else {
            char line[512];
            double quality = 100.0;
            int peers = 0;
            double bandwidth_bps = 0;
            double retry_bandwidth_bps = 0;
            int64_t sent = 0, received = 0, missing = 0, reordered = 0;
            int64_t recovered = 0, recovered_one_retry = 0, lost = 0;
            double min_iat = 0, cur_iat = 0, max_iat = 0, rtt = 0;
            int found_data = 0;

            while (fgets(line, sizeof(line), fp)) {
                if (line[0] == '#') continue;

                char *brace = strchr(line, '{');
                if (!brace) continue;

                *brace = '\0';
                char *name = line;
                char *value_str = strrchr(brace + 1, '}');
                if (value_str) {
                    value_str++;
                    while (*value_str == ' ') value_str++;
                    double value = atof(value_str);
                    found_data = 1;

                    if (strcmp(name, "rist_client_flow_quality") == 0) quality = value;
                    else if (strcmp(name, "rist_client_flow_peers") == 0) peers = (int)value;
                    else if (strcmp(name, "rist_client_flow_bandwidth_bps") == 0) bandwidth_bps = value;
                    else if (strcmp(name, "rist_client_flow_retry_bandwidth_bps") == 0) retry_bandwidth_bps = value;
                    else if (strcmp(name, "rist_client_flow_sent_packets_total") == 0) sent = (int64_t)value;
                    else if (strcmp(name, "rist_client_flow_received_packets_total") == 0) received = (int64_t)value;
                    else if (strcmp(name, "rist_client_flow_missing_packets_total") == 0) missing = (int64_t)value;
                    else if (strcmp(name, "rist_client_flow_reordered_packets_total") == 0) reordered = (int64_t)value;
                    else if (strcmp(name, "rist_client_flow_recovered_packets_total") == 0) recovered = (int64_t)value;
                    else if (strcmp(name, "rist_client_flow_recovered_one_retry_packets_total") == 0) recovered_one_retry = (int64_t)value;
                    else if (strcmp(name, "rist_client_flow_lost_packets_total") == 0) lost = (int64_t)value;
                    else if (strcmp(name, "rist_client_flow_min_iat_seconds") == 0) min_iat = value;
                    else if (strcmp(name, "rist_client_flow_cur_iat_seconds") == 0) cur_iat = value;
                    else if (strcmp(name, "rist_client_flow_max_iat_seconds") == 0) max_iat = value;
                    else if (strcmp(name, "rist_client_flow_rtt_seconds") == 0) rtt = value;
                }
            }
            pclose(fp);

            if (found_data) {
                snprintf(response, sizeof(response),
                    "{"
                    "\"quality\":%.2f,"
                    "\"peers\":%d,"
                    "\"bandwidth_bps\":%.0f,"
                    "\"retry_bandwidth_bps\":%.0f,"
                    "\"packets\":{"
                        "\"sent\":%ld,"
                        "\"received\":%ld,"
                        "\"missing\":%ld,"
                        "\"reordered\":%ld,"
                        "\"recovered\":%ld,"
                        "\"recovered_one_retry\":%ld,"
                        "\"lost\":%ld"
                    "},"
                    "\"timing\":{"
                        "\"min_iat_ms\":%.2f,"
                        "\"cur_iat_ms\":%.2f,"
                        "\"max_iat_ms\":%.2f,"
                        "\"rtt_ms\":%.2f"
                    "}"
                    "}",
                    quality, peers, bandwidth_bps, retry_bandwidth_bps,
                    (long)sent, (long)received, (long)missing, (long)reordered,
                    (long)recovered, (long)recovered_one_retry, (long)lost,
                    min_iat * 1000, cur_iat * 1000, max_iat * 1000, rtt * 1000);
            } else {
                snprintf(response, sizeof(response),
                    "{\"error\":\"No RIST metrics available\",\"metrics_port\":%d}",
                    g_ctx.rist_metrics_port);
                status_code = MHD_HTTP_SERVICE_UNAVAILABLE;
            }
        }
    } else {
        snprintf(response, sizeof(response), "{\"error\":\"Not found\"}");
        status_code = MHD_HTTP_NOT_FOUND;
    }

    struct MHD_Response *mhd_response = MHD_create_response_from_buffer(
        strlen(response), response, MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(mhd_response, "Content-Type", "application/json");
    MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");
    int ret = MHD_queue_response(connection, status_code, mhd_response);
    MHD_destroy_response(mhd_response);

    return ret;
}

int main(int argc, char *argv[]) {
    init_context();

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--url") == 0 && i + 1 < argc) {
            strncpy(g_ctx.rist_url, argv[++i], sizeof(g_ctx.rist_url) - 1);
        } else if (strcmp(argv[i], "--buffer") == 0 && i + 1 < argc) {
            g_ctx.buffer_size = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--secret") == 0 && i + 1 < argc) {
            strncpy(g_ctx.secret, argv[++i], sizeof(g_ctx.secret) - 1);
        } else if (strcmp(argv[i], "--encryption") == 0 && i + 1 < argc) {
            g_ctx.encryption_type = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
            g_ctx.profile = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            char *output = argv[++i];
            char *colon = strchr(output, ':');
            if (colon) {
                *colon = '\0';
                strncpy(g_ctx.output_addr, output, sizeof(g_ctx.output_addr) - 1);
                g_ctx.output_port = atoi(colon + 1);
            }
        } else if (strcmp(argv[i], "--program") == 0 && i + 1 < argc) {
            g_ctx.program_pid = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--pids") == 0 && i + 1 < argc) {
            g_ctx.pid_count = parse_pids(argv[++i]);
        } else if (strcmp(argv[i], "--log-file") == 0 && i + 1 < argc) {
            strncpy(g_ctx.log_file, argv[++i], sizeof(g_ctx.log_file) - 1);
        } else if (strcmp(argv[i], "--api-port") == 0 && i + 1 < argc) {
            g_ctx.api_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--rist-metrics-port") == 0 && i + 1 < argc) {
            g_ctx.rist_metrics_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--stall-timeout") == 0 && i + 1 < argc) {
            g_ctx.stall_timeout = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--history-hours") == 0 && i + 1 < argc) {
            g_ctx.history_hours = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        }
    }

    // Validate required parameters
    if (!g_ctx.rist_url[0]) {
        fprintf(stderr, "Error: --url is required\n");
        print_help(argv[0]);
        return 1;
    }
    if (!g_ctx.output_addr[0] || g_ctx.output_port == 0) {
        fprintf(stderr, "Error: --output is required (format: ADDRESS:PORT)\n");
        print_help(argv[0]);
        return 1;
    }
    if (g_ctx.program_pid == 0) {
        fprintf(stderr, "Error: --program is required\n");
        print_help(argv[0]);
        return 1;
    }
    if (g_ctx.pid_count == 0) {
        fprintf(stderr, "Error: --pids is required\n");
        print_help(argv[0]);
        return 1;
    }

    // Set default rist_metrics_port if not specified
    if (g_ctx.rist_metrics_port == 0) {
        g_ctx.rist_metrics_port = g_ctx.api_port + 1000;
    }

    // Initialize monitors
    init_monitors();

    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGCHLD, SIG_DFL);

    // Start HTTP API server
    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_THREAD_PER_CONNECTION,
        g_ctx.api_port,
        NULL, NULL,
        api_handler, NULL,
        MHD_OPTION_END);

    if (!daemon) {
        fprintf(stderr, "Failed to start HTTP server on port %d\n", g_ctx.api_port);
        return 1;
    }

    fprintf(stderr, "RIST Input started\n");
    fprintf(stderr, "  RIST URL: %s\n", g_ctx.rist_url);
    fprintf(stderr, "  Output: %s:%d\n", g_ctx.output_addr, g_ctx.output_port);
    fprintf(stderr, "  API Port: %d\n", g_ctx.api_port);
    fprintf(stderr, "  RIST Metrics Port: %d\n", g_ctx.rist_metrics_port);
    fprintf(stderr, "  Profile: %d\n", g_ctx.profile);
    if (g_ctx.secret[0]) {
        fprintf(stderr, "  Encryption: AES-%d\n", g_ctx.encryption_type > 0 ? g_ctx.encryption_type : 128);
    }

    // Start pipeline manager thread
    pthread_t pipeline_thread;
    pthread_create(&pipeline_thread, NULL, pipeline_manager_thread, NULL);

    // Start log parser thread
    pthread_t log_thread;
    pthread_create(&log_thread, NULL, log_parser_thread, NULL);

    // Wait for threads
    pthread_join(pipeline_thread, NULL);
    pthread_join(log_thread, NULL);

    MHD_stop_daemon(daemon);
    pthread_mutex_destroy(&g_ctx.lock);

    fprintf(stderr, "RIST Input stopped\n");
    return 0;
}
