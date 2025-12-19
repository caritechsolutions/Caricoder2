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
    char input_addr[64];
    int input_port;
    char output_addr[64];
    int output_port;
    char log_file[256];
    int api_port;
    uint16_t program_pid;
    uint16_t pids[MAX_PIDS];  // Video/audio PIDs to monitor
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

void print_help(const char *prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  --input ADDRESS:PORT         Input UDP address (required)\n");
    printf("  --output ADDRESS:PORT        Output UDP address (required)\n");
    printf("  --program PID                Program/PMT PID (required)\n");
    printf("  --pids PID1,PID2,...         Video/audio PIDs to monitor (required)\n");
    printf("  --log-file FILE              Log file path (default: /tmp/ts_monitor.log)\n");
    printf("  --api-port PORT              REST API port (default: 8080)\n");
    printf("  --stall-timeout SECONDS      Stall timeout (default: 30)\n");
    printf("  --history-hours HOURS        History retention (default: 24)\n");
    printf("  --help                       Show this help\n");
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
    strcpy(g_ctx.log_file, "/tmp/ts_monitor.log");
    g_ctx.api_port = 8080;
    g_ctx.stall_timeout = 30;
    g_ctx.history_hours = 24;
    g_ctx.last_data_received = time(NULL);
    g_ctx.running = 1;
    pthread_mutex_init(&g_ctx.lock, NULL);
}

void init_monitors() {
    // Initialize monitors array from pids array
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
        usleep(100000);  // 100ms grace period
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

// Parse tsp bitrate_monitor output
void parse_bitrate_line(const char *line) {
    if (strstr(line, "bitrate_monitor") == NULL) {
        return;
    }

    // Parse timestamp and PID/bitrate from line like:
    // * bitrate_monitor: 2025/12/18 14:45:44, PID 0x00D3 (211) bitrate: 1,408,646 bits/s
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

    // Extract PID - look for "PID 0x" pattern
    const char *pid_pos = strstr(line, "PID 0x");
    if (!pid_pos) return;

    unsigned int pid_hex;
    if (sscanf(pid_pos, "PID 0x%x", &pid_hex) != 1) {
        return;
    }

    // Extract bitrate - look for "bitrate:" pattern
    const char *bitrate_pos = strstr(line, "bitrate:");
    if (!bitrate_pos) return;

    // Parse bitrate, handling commas in number
    unsigned long bitrate = 0;
    const char *p = bitrate_pos + 8;  // Skip "bitrate:"
    while (*p && *p != 'b') {  // Stop at "bits/s"
        if (*p >= '0' && *p <= '9') {
            bitrate = bitrate * 10 + (*p - '0');
        }
        p++;
    }

    if (bitrate > 0) {
        pthread_mutex_lock(&g_ctx.lock);
        PIDMonitor *m = find_monitor(pid_hex);
        if (m) {
            add_bitrate_sample(m, timestamp, (uint32_t)bitrate);
        }
        pthread_mutex_unlock(&g_ctx.lock);
    }
}

void* log_monitor_thread(void *arg) {
    (void)arg;
    long last_pos = 0;

    while (g_ctx.running) {
        FILE *fp = fopen(g_ctx.log_file, "r");
        if (!fp) {
            usleep(500000);  // 500ms
            continue;
        }

        fseek(fp, last_pos, SEEK_SET);

        char line[MAX_LOG_LINE];
        while (fgets(line, sizeof(line), fp)) {
            parse_bitrate_line(line);
        }

        last_pos = ftell(fp);
        fclose(fp);

        usleep(500000);  // 500ms poll interval
    }

    return NULL;
}

void* tsp_manager_thread(void *arg) {
    (void)arg;

    while (g_ctx.running) {
        // Build argument array for execvp
        // Max args: tsp -I ip addr:port -P filter -p X ... -P bitrate_monitor --pid X --periodic-bitrate 5 ... -O ip addr:port
        char *argv[128];
        int argc = 0;

        char input_arg[128], output_arg[128];
        snprintf(input_arg, sizeof(input_arg), "%s:%d", g_ctx.input_addr, g_ctx.input_port);
        snprintf(output_arg, sizeof(output_arg), "%s:%d", g_ctx.output_addr, g_ctx.output_port);

        argv[argc++] = "tsp";
        argv[argc++] = "-I";
        argv[argc++] = "ip";
        argv[argc++] = input_arg;

        // Filter plugin
        argv[argc++] = "-P";
        argv[argc++] = "filter";

        // Static PID strings (need to persist during exec)
        static char pid_args[MAX_PIDS + 4][16];
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
        static char monitor_pids[MAX_PIDS][16];
        for (int i = 0; i < g_ctx.pid_count; i++) {
            snprintf(monitor_pids[i], sizeof(monitor_pids[i]), "%u", g_ctx.pids[i]);
            argv[argc++] = "-P";
            argv[argc++] = "bitrate_monitor";
            argv[argc++] = "--pid";
            argv[argc++] = monitor_pids[i];
            argv[argc++] = "--periodic-bitrate";
            argv[argc++] = "5";
        }

        // Output plugin
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
            // Set up to die when parent dies
            prctl(PR_SET_PDEATHSIG, SIGKILL);

            // Redirect stderr to log file
            FILE *log = fopen(g_ctx.log_file, "a");
            if (log) {
                dup2(fileno(log), STDERR_FILENO);
                fclose(log);
            }

            execvp("tsp", argv);
            // If exec fails
            perror("execvp tsp failed");
            _exit(1);
        } else if (pid > 0) {
            // Parent process
            g_ctx.tsp_child = pid;
            fprintf(stderr, "tsp started with PID %d\n", pid);

            // Wait for child to exit
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

        char response[16384] = "{\"status\":\"running\",\"pids\":{";

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
        int ret = MHD_queue_response(connection, MHD_HTTP_OK, mhd_response);
        MHD_destroy_response(mhd_response);
        return ret;
    }

    if (strcmp(url, "/metrics/history") == 0) {
        pthread_mutex_lock(&g_ctx.lock);

        // Calculate required buffer size (worst case: HISTORY_SIZE entries per PID)
        // Each entry: [timestamp,bitrate], ~ 25 chars max
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

        strcpy(response, "{\"status\":\"running\",\"pids\":{");

        for (int i = 0; i < g_ctx.monitor_count; i++) {
            PIDMonitor *m = &g_ctx.monitors[i];

            if (i > 0) strcat(response, ",");

            sprintf(response + strlen(response),
                   "\"%u\":{\"pid\":%u,\"name\":\"%s\",\"current_bitrate\":%u,\"history\":[",
                   m->pid, m->pid, m->name, m->current_bitrate);

            // Output history in chronological order (oldest first)
            // Circular buffer: if full, start from history_index (oldest), else start from 0
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

        strcat(response, "}}");

        pthread_mutex_unlock(&g_ctx.lock);

        struct MHD_Response *mhd_response = MHD_create_response_from_buffer(
            strlen(response), (void *)response, MHD_RESPMEM_MUST_FREE);
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

    int has_input = 0, has_output = 0, has_program = 0, has_pids = 0;

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
    if (!has_input || !has_output || !has_program || !has_pids) {
        fprintf(stderr, "ERROR: Missing required arguments\n");
        fprintf(stderr, "  --input ADDRESS:PORT (required)\n");
        fprintf(stderr, "  --output ADDRESS:PORT (required)\n");
        fprintf(stderr, "  --program PID (required)\n");
        fprintf(stderr, "  --pids PID1,PID2,... (required)\n\n");
        print_help(argv[0]);
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

    fprintf(stderr, "UDP Input Monitor Starting\n");
    fprintf(stderr, "Input:  %s:%d\n", g_ctx.input_addr, g_ctx.input_port);
    fprintf(stderr, "Output: %s:%d\n", g_ctx.output_addr, g_ctx.output_port);
    fprintf(stderr, "Program PID: %u\n", g_ctx.program_pid);
    fprintf(stderr, "Monitoring PIDs: ");
    for (int i = 0; i < g_ctx.pid_count; i++) {
        fprintf(stderr, "%u ", g_ctx.pids[i]);
    }
    fprintf(stderr, "(%d PIDs)\n", g_ctx.pid_count);
    fprintf(stderr, "Filter PIDs: 0, 17, %u", g_ctx.program_pid);
    for (int i = 0; i < g_ctx.pid_count; i++) {
        fprintf(stderr, ", %u", g_ctx.pids[i]);
    }
    fprintf(stderr, "\n");
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
    fprintf(stderr, "Endpoints: GET /metrics, GET /metrics/history, GET /health\n");

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
