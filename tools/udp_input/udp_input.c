#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <microhttpd.h>
#include <pthread.h>

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
    int api_port;
    char log_file[256];
    uint16_t pids[MAX_PIDS];
    int pid_count;
    int stall_timeout;
    int history_hours;
    PIDMonitor monitors[MAX_PIDS];
    FILE *log_fp;
    time_t last_log_pos;
    time_t last_data_received;
    int running;
    pthread_mutex_t lock;
} AppContext;

AppContext g_ctx;

void print_help(const char *prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  --api-port PORT              REST API port (default: 8080)\n");
    printf("  --log-file FILE              Log file path (default: /tmp/ts_monitor.log)\n");
    printf("  --pids PID1,PID2,...         PIDs to monitor (comma-separated)\n");
    printf("  --stall-timeout SECONDS      Stall timeout (default: 10)\n");
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
    g_ctx.api_port = 8080;
    strcpy(g_ctx.log_file, "/tmp/ts_monitor.log");
    g_ctx.stall_timeout = 10;
    g_ctx.history_hours = 24;
    g_ctx.pid_count = 0;
    g_ctx.log_fp = NULL;
    g_ctx.last_log_pos = 0;
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
            return;  // Already exists
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

// Parse log line like: "* bitrate_monitor: 2025/12/18 14:45:44, PID 0x00D3 (211) bitrate: 1,408,646 bits/s"
void parse_log_line(const char *line) {
    if (strstr(line, "bitrate_monitor") == NULL) {
        return;
    }
    
    // Extract timestamp
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
    
    // Extract PID and bitrate
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

void* log_monitor_thread(void *arg) {
    (void)arg;
    while (g_ctx.running) {
        FILE *fp = fopen(g_ctx.log_file, "r");
        if (!fp) {
            sleep(1);
            continue;
        }
        
        fseek(fp, g_ctx.last_log_pos, SEEK_SET);
        
        char line[MAX_LOG_LINE];
        while (fgets(line, sizeof(line), fp)) {
            parse_log_line(line);
        }
        
        g_ctx.last_log_pos = ftell(fp);
        fclose(fp);
        
        // Check for stall
        time_t now = time(NULL);
        if (now - g_ctx.last_data_received > g_ctx.stall_timeout) {
            fprintf(stderr, "ERROR: Data stall detected. Exiting for systemd restart.\n");
            g_ctx.running = 0;
            exit(1);
        }
        
        usleep(100000);  // 100ms poll interval
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
    if (strcmp(method, "GET") != 0) {
        return MHD_NO;
    }
    
    if (strcmp(url, "/metrics") == 0) {
        pthread_mutex_lock(&g_ctx.lock);
        
        // Build JSON response
        char response[8192] = "{\"status\":\"running\",\"pids\":{";
        
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
    
    // Parse command line
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--api-port") == 0 && i + 1 < argc) {
            g_ctx.api_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--log-file") == 0 && i + 1 < argc) {
            strncpy(g_ctx.log_file, argv[++i], sizeof(g_ctx.log_file) - 1);
        } else if (strcmp(argv[i], "--pids") == 0 && i + 1 < argc) {
            int count = parse_pids(argv[++i]);
            for (int j = 0; j < count; j++) {
                add_monitor(g_ctx.pids[j], "");
            }
        } else if (strcmp(argv[i], "--stall-timeout") == 0 && i + 1 < argc) {
            g_ctx.stall_timeout = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--history-hours") == 0 && i + 1 < argc) {
            g_ctx.history_hours = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        }
    }
    
    // Add monitors for configured PIDs
    for (int i = 0; i < g_ctx.pid_count; i++) {
        char name[64];
        snprintf(name, sizeof(name), "PID %u", g_ctx.pids[i]);
        add_monitor(g_ctx.pids[i], name);
    }
    
    fprintf(stderr, "Starting UDP Input Monitor\n");
    fprintf(stderr, "API Port: %d\n", g_ctx.api_port);
    fprintf(stderr, "Log File: %s\n", g_ctx.log_file);
    fprintf(stderr, "Monitoring %d PIDs\n", g_ctx.pid_count);
    
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
    fprintf(stderr, "Endpoints: GET /metrics, GET /health\n");
    
    // Main loop - keep running until stall detected
    while (g_ctx.running) {
        sleep(1);
    }
    
    MHD_stop_daemon(daemon);
    pthread_cancel(log_thread);
    pthread_join(log_thread, NULL);
    
    return 0;
}
