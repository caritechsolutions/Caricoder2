/*
 * srt_input.c - SRT Input Monitor for CariTranscoder
 *
 * Receives MPEG-TS streams via SRT (Secure Reliable Transport) and outputs
 * to UDP/multicast. Provides REST API for bitrate monitoring.
 *
 * Based on udp_input.c
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

// SRT modes
typedef enum {
    SRT_MODE_CALLER,
    SRT_MODE_LISTENER,
    SRT_MODE_RENDEZVOUS
} SrtMode;

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
    // SRT input settings
    char srt_address[64];
    int srt_port;
    SrtMode srt_mode;
    int latency;                // SRT latency in ms
    char streamid[512];         // Optional stream ID
    char passphrase[256];       // Optional encryption passphrase
    int pbkeylen;               // Passphrase key length (0, 16, 24, 32)

    // Output settings (UDP)
    char output_addr[64];
    int output_port;

    // Common settings
    char log_file[256];
    int api_port;
    uint16_t program_pid;
    uint16_t pids[MAX_PIDS];
    int pid_count;
    int stall_timeout;
    int history_hours;
    PIDMonitor monitors[MAX_PIDS];
    int monitor_count;
    time_t last_data_received;
    volatile int running;
    pid_t tsp_child;
    pthread_mutex_t lock;
} AppContext;

AppContext g_ctx;

const char* srt_mode_to_string(SrtMode mode) {
    switch (mode) {
        case SRT_MODE_CALLER: return "caller";
        case SRT_MODE_LISTENER: return "listener";
        case SRT_MODE_RENDEZVOUS: return "rendezvous";
        default: return "unknown";
    }
}

SrtMode parse_srt_mode(const char *mode_str) {
    if (strcasecmp(mode_str, "caller") == 0) return SRT_MODE_CALLER;
    if (strcasecmp(mode_str, "listener") == 0) return SRT_MODE_LISTENER;
    if (strcasecmp(mode_str, "rendezvous") == 0) return SRT_MODE_RENDEZVOUS;
    return SRT_MODE_CALLER;  // Default
}

void print_help(const char *prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("SRT Input Options:\n");
    printf("  --address ADDRESS            SRT address (required)\n");
    printf("  --port PORT                  SRT port (required)\n");
    printf("  --mode MODE                  SRT mode: caller, listener, rendezvous (default: caller)\n");
    printf("  --latency MS                 SRT latency in milliseconds (default: 120)\n");
    printf("  --streamid ID                SRT stream ID (optional)\n");
    printf("  --passphrase KEY             SRT encryption passphrase (optional)\n");
    printf("  --pbkeylen LENGTH            Passphrase key length: 0, 16, 24, 32 (default: 0)\n");
    printf("\nOutput Options:\n");
    printf("  --output ADDRESS:PORT        Output UDP address (required)\n");
    printf("\nStream Options:\n");
    printf("  --program PID                Program/PMT PID (required)\n");
    printf("  --pids PID1,PID2,...         Video/audio PIDs to monitor (required)\n");
    printf("\nGeneral Options:\n");
    printf("  --log-file FILE              Log file path (default: /tmp/srt_input.log)\n");
    printf("  --api-port PORT              REST API port (default: 8080)\n");
    printf("  --stall-timeout SECONDS      Stall timeout (default: 30)\n");
    printf("  --history-hours HOURS        History retention (default: 24)\n");
    printf("  --help                       Show this help\n");
    printf("\nExamples:\n");
    printf("  Caller mode (connect to remote):\n");
    printf("    %s --address 192.168.1.100 --port 9000 --mode caller --latency 200 \\\n", prog);
    printf("         --output 239.1.1.1:5000 --program 256 --pids 257,258\n");
    printf("\n  Listener mode (accept connections):\n");
    printf("    %s --address 0.0.0.0 --port 9000 --mode listener --latency 200 \\\n", prog);
    printf("         --output 239.1.1.1:5000 --program 256 --pids 257,258\n");
    printf("\n  With encryption:\n");
    printf("    %s --address 192.168.1.100 --port 9000 --mode caller --latency 200 \\\n", prog);
    printf("         --passphrase \"mysecretkey\" --pbkeylen 16 \\\n");
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
    strcpy(g_ctx.log_file, "/tmp/srt_input.log");
    g_ctx.api_port = 8080;
    g_ctx.stall_timeout = 30;
    g_ctx.history_hours = 24;
    g_ctx.last_data_received = time(NULL);
    g_ctx.running = 1;
    g_ctx.srt_mode = SRT_MODE_CALLER;
    g_ctx.latency = 120;  // Default SRT latency
    g_ctx.pbkeylen = 0;   // No encryption by default
    pthread_mutex_init(&g_ctx.lock, NULL);
}

void init_monitors() {
    g_ctx.monitor_count = g_ctx.pid_count;
    for (int i = 0; i < g_ctx.pid_count; i++) {
        PIDMonitor *m = &g_ctx.monitors[i];
        m->pid = g_ctx.pids[i];
        snprintf(m->name, sizeof(m->name), "PID %u", g_ctx.pids[i]);
        m->history_index = 0;
        m->history_count = 0;
        m->current_bitrate = 0;
        m->last_update = time(NULL);
    }
}

void kill_tsp_child() {
    if (g_ctx.tsp_child > 0) {
        fprintf(stderr, "Killing tsp child process %d\n", g_ctx.tsp_child);
        kill(g_ctx.tsp_child, SIGTERM);
        usleep(100000);
        kill(g_ctx.tsp_child, SIGKILL);
        waitpid(g_ctx.tsp_child, NULL, WNOHANG);
        g_ctx.tsp_child = 0;
    }
}

void signal_handler(int sig) {
    fprintf(stderr, "Received signal %d, shutting down...\n", sig);
    g_ctx.running = 0;
    kill_tsp_child();
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
    g_ctx.last_data_received = time(NULL);
}

void* log_monitor_thread(void *arg) {
    (void)arg;
    char line[MAX_LOG_LINE];
    FILE *log = NULL;
    long last_pos = 0;

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
                // Parse bitrate monitor output: "PID: 0x00XX (XXX), bitrate: XXX,XXX b/s"
                uint16_t pid;
                uint32_t bitrate;
                char *bitrate_str = strstr(line, "bitrate:");
                if (bitrate_str && sscanf(line, "* PID: 0x%hx", &pid) == 1) {
                    bitrate_str += 8;
                    // Remove commas from bitrate
                    char clean_bitrate[32];
                    int j = 0;
                    for (int i = 0; bitrate_str[i] && j < 31; i++) {
                        if (bitrate_str[i] >= '0' && bitrate_str[i] <= '9') {
                            clean_bitrate[j++] = bitrate_str[i];
                        } else if (bitrate_str[i] == ' ' || bitrate_str[i] == 'b') {
                            break;
                        }
                    }
                    clean_bitrate[j] = '\0';
                    bitrate = atoi(clean_bitrate);

                    pthread_mutex_lock(&g_ctx.lock);
                    PIDMonitor *m = find_monitor(pid);
                    if (m) {
                        add_bitrate_sample(m, time(NULL), bitrate);
                    }
                    pthread_mutex_unlock(&g_ctx.lock);
                }
            }
            last_pos = ftell(log);
        }

        usleep(500000);  // 0.5 second
    }

    if (log) fclose(log);
    return NULL;
}

void* tsp_manager_thread(void *arg) {
    (void)arg;

    while (g_ctx.running) {
        // Build argument array for execvp
        char *argv[128];
        int argc = 0;

        // Static strings for arguments that need to persist
        static char srt_arg[256];
        static char latency_str[16];
        static char pbkeylen_str[16];
        static char output_arg[128];
        static char pid_args[MAX_PIDS + 4][16];
        static char monitor_pids[MAX_PIDS][16];

        snprintf(output_arg, sizeof(output_arg), "%s:%d", g_ctx.output_addr, g_ctx.output_port);
        snprintf(latency_str, sizeof(latency_str), "%d", g_ctx.latency);
        snprintf(pbkeylen_str, sizeof(pbkeylen_str), "%d", g_ctx.pbkeylen);

        argv[argc++] = "tsp";
        argv[argc++] = "-I";
        argv[argc++] = "srt";

        // SRT connection mode
        switch (g_ctx.srt_mode) {
            case SRT_MODE_CALLER:
                snprintf(srt_arg, sizeof(srt_arg), "%s:%d", g_ctx.srt_address, g_ctx.srt_port);
                argv[argc++] = "--caller";
                argv[argc++] = srt_arg;
                break;
            case SRT_MODE_LISTENER:
                snprintf(srt_arg, sizeof(srt_arg), "%s:%d", g_ctx.srt_address, g_ctx.srt_port);
                argv[argc++] = "--listener";
                argv[argc++] = srt_arg;
                break;
            case SRT_MODE_RENDEZVOUS:
                // Rendezvous requires both --caller and --listener
                snprintf(srt_arg, sizeof(srt_arg), "%s:%d", g_ctx.srt_address, g_ctx.srt_port);
                argv[argc++] = "--caller";
                argv[argc++] = srt_arg;
                argv[argc++] = "--listener";
                argv[argc++] = srt_arg;
                break;
        }

        // Latency
        argv[argc++] = "--latency";
        argv[argc++] = latency_str;

        // Optional: Stream ID
        if (g_ctx.streamid[0]) {
            argv[argc++] = "--streamid";
            argv[argc++] = g_ctx.streamid;
        }

        // Optional: Encryption
        if (g_ctx.passphrase[0]) {
            argv[argc++] = "--passphrase";
            argv[argc++] = g_ctx.passphrase;
            if (g_ctx.pbkeylen > 0) {
                argv[argc++] = "--pbkeylen";
                argv[argc++] = pbkeylen_str;
            }
        }

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

            // Redirect stderr to log file
            FILE *log = fopen(g_ctx.log_file, "a");
            if (log) {
                dup2(fileno(log), STDERR_FILENO);
                fclose(log);
            }

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

    if (strcmp(url, "/metrics") == 0) {
        pthread_mutex_lock(&g_ctx.lock);

        char response[16384] = "{\"status\":\"running\",\"type\":\"srt\",\"pids\":{";

        for (int i = 0; i < g_ctx.monitor_count; i++) {
            PIDMonitor *m = &g_ctx.monitors[i];

            if (i > 0) strcat(response, ",");

            sprintf(response + strlen(response),
                   "\"%u\":{\"pid\":%u,\"name\":\"%s\",\"current_bitrate\":%u,\"last_update\":%ld,\"sample_count\":%d}",
                   m->pid, m->pid, m->name, m->current_bitrate, m->last_update, m->history_count);
        }

        strcat(response, "}}");

        pthread_mutex_unlock(&g_ctx.lock);

        struct MHD_Response *mhd_response = MHD_create_response_from_buffer(
            strlen(response), (void *)response, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(mhd_response, "Content-Type", "application/json");
        MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");
        int ret = MHD_queue_response(connection, MHD_HTTP_OK, mhd_response);
        MHD_destroy_response(mhd_response);
        return ret;
    }

    if (strcmp(url, "/metrics/history") == 0) {
        pthread_mutex_lock(&g_ctx.lock);

        size_t buf_size = 1024 + (g_ctx.monitor_count * HISTORY_SIZE * 30);
        char *response = malloc(buf_size);
        if (!response) {
            pthread_mutex_unlock(&g_ctx.lock);
            const char *err = "{\"error\":\"out of memory\"}";
            struct MHD_Response *mhd_response = MHD_create_response_from_buffer(
                strlen(err), (void *)err, MHD_RESPMEM_MUST_COPY);
            int ret = MHD_queue_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, mhd_response);
            MHD_destroy_response(mhd_response);
            return ret;
        }

        strcpy(response, "{\"status\":\"running\",\"type\":\"srt\",\"pids\":{");

        for (int i = 0; i < g_ctx.monitor_count; i++) {
            PIDMonitor *m = &g_ctx.monitors[i];

            if (i > 0) strcat(response, ",");

            sprintf(response + strlen(response),
                   "\"%u\":{\"pid\":%u,\"name\":\"%s\",\"current_bitrate\":%u,\"history\":[",
                   m->pid, m->pid, m->name, m->current_bitrate);

            int start_idx = (m->history_count == HISTORY_SIZE) ? m->history_index : 0;
            int first = 1;

            for (int j = 0; j < m->history_count; j++) {
                int idx = (start_idx + j) % HISTORY_SIZE;
                BitrateEntry *e = &m->history[idx];

                if (!first) strcat(response, ",");
                first = 0;

                sprintf(response + strlen(response), "[%ld,%u]", e->timestamp, e->bitrate);
            }

            strcat(response, "]}");
        }

        sprintf(response + strlen(response), "},\"output_address\":\"%s:%d\"}",
                g_ctx.output_addr, g_ctx.output_port);

        pthread_mutex_unlock(&g_ctx.lock);

        struct MHD_Response *mhd_response = MHD_create_response_from_buffer(
            strlen(response), response, MHD_RESPMEM_MUST_FREE);
        MHD_add_response_header(mhd_response, "Content-Type", "application/json");
        MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");
        int ret = MHD_queue_response(connection, MHD_HTTP_OK, mhd_response);
        MHD_destroy_response(mhd_response);
        return ret;
    }

    if (strcmp(url, "/health") == 0) {
        const char *response = "{\"status\":\"ok\",\"type\":\"srt\"}";
        struct MHD_Response *mhd_response = MHD_create_response_from_buffer(
            strlen(response), (void *)response, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(mhd_response, "Content-Type", "application/json");
        MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");
        int ret = MHD_queue_response(connection, MHD_HTTP_OK, mhd_response);
        MHD_destroy_response(mhd_response);
        return ret;
    }

    if (strcmp(url, "/status") == 0) {
        char response[1024];
        snprintf(response, sizeof(response),
            "{\"status\":\"running\",\"type\":\"srt\",\"mode\":\"%s\","
            "\"address\":\"%s\",\"port\":%d,\"latency\":%d,"
            "\"output\":\"%s:%d\"}",
            srt_mode_to_string(g_ctx.srt_mode),
            g_ctx.srt_address, g_ctx.srt_port, g_ctx.latency,
            g_ctx.output_addr, g_ctx.output_port);

        struct MHD_Response *mhd_response = MHD_create_response_from_buffer(
            strlen(response), (void *)response, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(mhd_response, "Content-Type", "application/json");
        MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");
        int ret = MHD_queue_response(connection, MHD_HTTP_OK, mhd_response);
        MHD_destroy_response(mhd_response);
        return ret;
    }

    const char *response = "{\"error\":\"not found\"}";
    struct MHD_Response *mhd_response = MHD_create_response_from_buffer(
        strlen(response), (void *)response, MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");
    int ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, mhd_response);
    MHD_destroy_response(mhd_response);
    return ret;
}

int main(int argc, char *argv[]) {
    init_context();

    int has_address = 0, has_port = 0, has_output = 0, has_program = 0, has_pids = 0;

    // Parse command line
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--address") == 0 && i + 1 < argc) {
            strncpy(g_ctx.srt_address, argv[++i], sizeof(g_ctx.srt_address) - 1);
            has_address = 1;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_ctx.srt_port = atoi(argv[++i]);
            has_port = 1;
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            g_ctx.srt_mode = parse_srt_mode(argv[++i]);
        } else if (strcmp(argv[i], "--latency") == 0 && i + 1 < argc) {
            g_ctx.latency = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--streamid") == 0 && i + 1 < argc) {
            strncpy(g_ctx.streamid, argv[++i], sizeof(g_ctx.streamid) - 1);
        } else if (strcmp(argv[i], "--passphrase") == 0 && i + 1 < argc) {
            strncpy(g_ctx.passphrase, argv[++i], sizeof(g_ctx.passphrase) - 1);
        } else if (strcmp(argv[i], "--pbkeylen") == 0 && i + 1 < argc) {
            g_ctx.pbkeylen = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            char *addr_port = argv[++i];
            char *colon = strchr(addr_port, ':');
            if (colon) {
                strncpy(g_ctx.output_addr, addr_port, colon - addr_port);
                g_ctx.output_addr[colon - addr_port] = '\0';
                g_ctx.output_port = atoi(colon + 1);
                has_output = 1;
            }
        } else if (strcmp(argv[i], "--program") == 0 && i + 1 < argc) {
            g_ctx.program_pid = atoi(argv[++i]);
            has_program = 1;
        } else if (strcmp(argv[i], "--log-file") == 0 && i + 1 < argc) {
            strncpy(g_ctx.log_file, argv[++i], sizeof(g_ctx.log_file) - 1);
        } else if (strcmp(argv[i], "--api-port") == 0 && i + 1 < argc) {
            g_ctx.api_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--pids") == 0 && i + 1 < argc) {
            g_ctx.pid_count = parse_pids(argv[++i]);
            has_pids = 1;
        } else if (strcmp(argv[i], "--stall-timeout") == 0 && i + 1 < argc) {
            g_ctx.stall_timeout = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--history-hours") == 0 && i + 1 < argc) {
            g_ctx.history_hours = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        }
    }

    // Validate required arguments
    if (!has_address || !has_port || !has_output || !has_program || !has_pids) {
        fprintf(stderr, "ERROR: Missing required arguments\n");
        fprintf(stderr, "  --address ADDRESS (required)\n");
        fprintf(stderr, "  --port PORT (required)\n");
        fprintf(stderr, "  --output ADDRESS:PORT (required)\n");
        fprintf(stderr, "  --program PID (required)\n");
        fprintf(stderr, "  --pids PID1,PID2,... (required)\n\n");
        print_help(argv[0]);
        return 1;
    }

    // Validate pbkeylen
    if (g_ctx.pbkeylen != 0 && g_ctx.pbkeylen != 16 && g_ctx.pbkeylen != 24 && g_ctx.pbkeylen != 32) {
        fprintf(stderr, "ERROR: --pbkeylen must be 0, 16, 24, or 32\n");
        return 1;
    }

    // Initialize monitors from PIDs
    init_monitors();

    // Set up signal handlers
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    // Clear/create log file
    FILE *lf = fopen(g_ctx.log_file, "w");
    if (lf) fclose(lf);

    fprintf(stderr, "SRT Input Monitor Starting\n");
    fprintf(stderr, "SRT Mode: %s\n", srt_mode_to_string(g_ctx.srt_mode));
    fprintf(stderr, "SRT Address: %s:%d\n", g_ctx.srt_address, g_ctx.srt_port);
    fprintf(stderr, "SRT Latency: %d ms\n", g_ctx.latency);
    if (g_ctx.streamid[0]) {
        fprintf(stderr, "SRT Stream ID: %s\n", g_ctx.streamid);
    }
    if (g_ctx.passphrase[0]) {
        fprintf(stderr, "SRT Encryption: enabled (keylen=%d)\n", g_ctx.pbkeylen);
    }
    fprintf(stderr, "Output: %s:%d\n", g_ctx.output_addr, g_ctx.output_port);
    fprintf(stderr, "Program PID: %u\n", g_ctx.program_pid);
    fprintf(stderr, "Monitoring PIDs: ");
    for (int i = 0; i < g_ctx.pid_count; i++) {
        fprintf(stderr, "%u ", g_ctx.pids[i]);
    }
    fprintf(stderr, "(%d PIDs)\n", g_ctx.pid_count);
    fprintf(stderr, "API Port: %d\n", g_ctx.api_port);
    fprintf(stderr, "Log File: %s\n", g_ctx.log_file);
    fprintf(stderr, "Stall Timeout: %d seconds\n", g_ctx.stall_timeout);

    // Start tsp manager thread
    pthread_t tsp_thread;
    pthread_create(&tsp_thread, NULL, tsp_manager_thread, NULL);

    // Start log monitor thread
    pthread_t log_thread;
    pthread_create(&log_thread, NULL, log_monitor_thread, NULL);

    // Start HTTP server
    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_SELECT_INTERNALLY,
        g_ctx.api_port,
        NULL, NULL,
        &api_handler, NULL,
        MHD_OPTION_END);

    if (!daemon) {
        fprintf(stderr, "ERROR: Failed to start HTTP server on port %d\n", g_ctx.api_port);
        return 1;
    }

    fprintf(stderr, "HTTP server started on port %d\n", g_ctx.api_port);
    fprintf(stderr, "Endpoints: GET /metrics, GET /metrics/history, GET /health, GET /status\n");

    // Main loop
    while (g_ctx.running) {
        sleep(1);

        // Check for stall
        time_t now = time(NULL);
        if (now - g_ctx.last_data_received > g_ctx.stall_timeout) {
            fprintf(stderr, "ERROR: Data stall detected (%ld seconds). Exiting for systemd restart.\n",
                    now - g_ctx.last_data_received);
            g_ctx.running = 0;
        }
    }

    // Cleanup
    fprintf(stderr, "Shutting down...\n");
    kill_tsp_child();
    MHD_stop_daemon(daemon);
    pthread_cancel(log_thread);
    pthread_join(tsp_thread, NULL);
    pthread_join(log_thread, NULL);

    fprintf(stderr, "Cleanup complete\n");
    return 0;
}
