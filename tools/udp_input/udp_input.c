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

#define MAX_PIDS 32
#define HISTORY_SIZE 17280  // 24 hours * 60 min * 60 sec / 5 sec per sample
#define MAX_LOG_LINE 512
#define MAX_PIDS_STR 256

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
    char input_addr[64];
    int input_port;
    char output_addr[64];
    int output_port;
    char log_file[256];
    int api_port;
    uint16_t pids[MAX_PIDS];
    int pid_count;
    int stall_timeout;
    int history_hours;
    PIDMonitor monitors[MAX_PIDS];
    pid_t tsp_pid;
    FILE *tsp_stderr;
    time_t last_data_received;
    int running;
    pthread_mutex_t lock;
} AppContext;

AppContext g_ctx;

void print_help(const char *prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  --input ADDRESS:PORT         Input UDP address (required)\n");
    printf("  --output ADDRESS:PORT        Output UDP address (required)\n");
    printf("  --log-file FILE              Log file path (default: /tmp/ts_monitor.log)\n");
    printf("  --api-port PORT              REST API port (default: 8080)\n");
    printf("  --pids PID1,PID2,...         PIDs to monitor (comma-separated, required)\n");
    printf("  --stall-timeout SECONDS      Stall timeout (default: 10)\n");
    printf("  --history-hours HOURS        History retention (default: 24)\n");
    printf("  --help                       Show this help\n");
}

int parse_pids(const char *pids_str) {
    char *copy = strdup(pids_str);
    char *token = strtok(copy, ",");
    int count = 2;  // Start at 2 for hardcoded PIDs 0 and 17

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
    g_ctx.input_port = 0;
    g_ctx.output_port = 0;
    strcpy(g_ctx.log_file, "/tmp/ts_monitor.log");
    g_ctx.api_port = 8080;
    g_ctx.stall_timeout = 10;
    g_ctx.history_hours = 24;
    g_ctx.pid_count = 0;
    g_ctx.tsp_pid = -1;
    g_ctx.tsp_stderr = NULL;
    g_ctx.last_data_received = time(NULL);
    g_ctx.running = 1;
    pthread_mutex_init(&g_ctx.lock, NULL);

    // Hardcode PIDs 0 and 17
    g_ctx.pids[0] = 0;
    g_ctx.pids[1] = 17;
    g_ctx.pid_count = 2;
}

void add_monitor(uint16_t pid, const char *name) {
    for (int i = 0; i < g_ctx.pid_count; i++) {
        if (g_ctx.monitors[i].pid == pid) {
            return;
        }
    }

    if (g_ctx.pid_count < MAX_PIDS) {
        PIDMonitor *m = &g_ctx.monitors[g_ctx.pid_count];
        m->pid = pid;
        strncpy(m->name, name, sizeof(m->name) - 1);
        m->history_index = 0;
        m->history_count = 0;
        m->current_bitrate = 0;
        m->last_update = time(NULL);
        g_ctx.pid_count++;
    }
}

PIDMonitor* find_monitor(uint16_t pid) {
    for (int i = 0; i < g_ctx.pid_count; i++) {
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

// Parse tsp bitrate_monitor output
void parse_bitrate_line(const char *line) {
    if (strstr(line, "bitrate_monitor") == NULL) {
        return;
    }

    struct tm tm_info = {0};
    int year, month, day, hour, min, sec;
    if (sscanf(line, "* bitrate_monitor: %d/%d/%d %d:%d:%d",
               &year, &month, &day, &hour, &min, &sec) != 6) {
        return;
    }

    tm_info.tm_year = year - 1900;
    tm_info.tm_mon = month - 1;
    tm_info.tm_mday = day;
    tm_info.tm_hour = hour;
    tm_info.tm_min = min;
    tm_info.tm_sec = sec;
    time_t timestamp = mktime(&tm_info);

    unsigned int pid;
    unsigned long bitrate;
    if (sscanf(line, "* bitrate_monitor: %*s PID 0x%x (%u) bitrate: %lu bits/s",
               &pid, &pid, &bitrate) != 3) {
        return;
    }

    pthread_mutex_lock(&g_ctx.lock);
    PIDMonitor *m = find_monitor(pid);
    if (m) {
        add_bitrate_sample(m, timestamp, (uint32_t)bitrate);
    }
    pthread_mutex_unlock(&g_ctx.lock);
}

void build_tsp_command(char *cmd, size_t len) {
    char pids_filter[256] = "";

    // Build PID filter string
    for (int i = 0; i < g_ctx.pid_count; i++) {
        if (i > 0) strcat(pids_filter, " -p ");
        else strcat(pids_filter, "-p ");
        sprintf(pids_filter + strlen(pids_filter), "%u", g_ctx.pids[i]);
    }

    // Build full tsp command
    snprintf(cmd, len,
             "tsp -I ip %s:%d "
             "-P filter %s "
             "-P bitrate_monitor --pid 211 --periodic-bitrate 5 "
             "-P bitrate_monitor --pid 221 --periodic-bitrate 5 "
             "-O ip %s:%d "
             "2> %s",
             g_ctx.input_addr, g_ctx.input_port,
             pids_filter,
             g_ctx.output_addr, g_ctx.output_port,
             g_ctx.log_file);
}

void* tsp_manager_thread(void *arg) {
    (void)arg;
    char cmd[1024];

    while (g_ctx.running) {
        build_tsp_command(cmd, sizeof(cmd));

        fprintf(stderr, "Starting tsp: %s\n", cmd);

        FILE *fp = popen(cmd, "r");
        if (!fp) {
            fprintf(stderr, "ERROR: Failed to start tsp\n");
            sleep(2);
            continue;
        }

        // Read from tsp stderr output
        char line[MAX_LOG_LINE];
        while (fgets(line, sizeof(line), fp) && g_ctx.running) {
            parse_bitrate_line(line);
        }

        pclose(fp);

        if (g_ctx.running) {
            fprintf(stderr, "tsp process ended, restarting...\n");
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

        char response[16384] = "{\"status\":\"running\",\"pids\":{";

        for (int i = 0; i < g_ctx.pid_count; i++) {
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
        int ret = MHD_queue_response(connection, MHD_HTTP_OK, mhd_response);
        MHD_destroy_response(mhd_response);
        return ret;
    }

    if (strcmp(url, "/health") == 0) {
        const char *response = "{\"status\":\"ok\"}";
        struct MHD_Response *mhd_response = MHD_create_response_from_buffer(
            strlen(response), (void *)response, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(mhd_response, "Content-Type", "application/json");
        int ret = MHD_queue_response(connection, MHD_HTTP_OK, mhd_response);
        MHD_destroy_response(mhd_response);
        return ret;
    }

    const char *response = "{\"error\":\"not found\"}";
    struct MHD_Response *mhd_response = MHD_create_response_from_buffer(
        strlen(response), (void *)response, MHD_RESPMEM_MUST_COPY);
    int ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, mhd_response);
    MHD_destroy_response(mhd_response);
    return ret;
}

int main(int argc, char *argv[]) {
    init_context();

    int has_input = 0, has_output = 0, has_pids = 0;

    // Parse command line
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            char *addr_port = argv[++i];
            char *colon = strchr(addr_port, ':');
            if (colon) {
                strncpy(g_ctx.input_addr, addr_port, colon - addr_port);
                g_ctx.input_port = atoi(colon + 1);
                has_input = 1;
            }
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            char *addr_port = argv[++i];
            char *colon = strchr(addr_port, ':');
            if (colon) {
                strncpy(g_ctx.output_addr, addr_port, colon - addr_port);
                g_ctx.output_port = atoi(colon + 1);
                has_output = 1;
            }
        } else if (strcmp(argv[i], "--log-file") == 0 && i + 1 < argc) {
            strncpy(g_ctx.log_file, argv[++i], sizeof(g_ctx.log_file) - 1);
        } else if (strcmp(argv[i], "--api-port") == 0 && i + 1 < argc) {
            g_ctx.api_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--pids") == 0 && i + 1 < argc) {
            int count = parse_pids(argv[++i]);
            g_ctx.pid_count = count;
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
    if (!has_input || !has_output || !has_pids) {
        fprintf(stderr, "ERROR: Missing required arguments\n");
        fprintf(stderr, "  --input ADDRESS:PORT (required)\n");
        fprintf(stderr, "  --output ADDRESS:PORT (required)\n");
        fprintf(stderr, "  --pids PID1,PID2,... (required)\n\n");
        print_help(argv[0]);
        return 1;
    }

    // Initialize monitors for all PIDs
    for (int i = 0; i < g_ctx.pid_count; i++) {
        char name[64];
        snprintf(name, sizeof(name), "PID %u", g_ctx.pids[i]);
        add_monitor(g_ctx.pids[i], name);
    }

    fprintf(stderr, "UDP Input Monitor Starting\n");
    fprintf(stderr, "Input:  %s:%d\n", g_ctx.input_addr, g_ctx.input_port);
    fprintf(stderr, "Output: %s:%d\n", g_ctx.output_addr, g_ctx.output_port);
    fprintf(stderr, "API Port: %d\n", g_ctx.api_port);
    fprintf(stderr, "Log File: %s\n", g_ctx.log_file);
    fprintf(stderr, "Monitoring %d PIDs\n", g_ctx.pid_count);

    // Start tsp manager thread
    pthread_t tsp_thread;
    pthread_create(&tsp_thread, NULL, tsp_manager_thread, NULL);

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
    fprintf(stderr, "Endpoints: GET /metrics, GET /health\n");

    // Main loop
    while (g_ctx.running) {
        sleep(1);

        // Check for stall
        time_t now = time(NULL);
        if (now - g_ctx.last_data_received > g_ctx.stall_timeout) {
            fprintf(stderr, "ERROR: Data stall detected. Exiting for systemd restart.\n");
            g_ctx.running = 0;
        }
    }

    MHD_stop_daemon(daemon);
    pthread_cancel(tsp_thread);
    pthread_join(tsp_thread, NULL);

    return 0;
}
