/*
 * cari-avsync.c - A/V Sync Monitor Service
 *
 * Monitors audio/video synchronization for all running inputs.
 * Uses tsp pcrextract to measure PTS-PCR offsets.
 * Formula: A/V offset = (audio_PTS - PCR) - (video_PTS - PCR)
 *
 * Copyright (c) 2024 CariTech Solutions
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <pthread.h>
#include <microhttpd.h>
#include <ctype.h>
#include <math.h>

// Compatibility for older libmicrohttpd versions (< 0.9.71)
#ifndef MHD_Result
typedef int MHD_Result;
#endif

#define API_PORT 8082
#define CONFIG_DIR "/etc/caritrans/inputs"
#define DATA_FILE "/var/lib/caritrans/avsync.json"
#define CHECK_INTERVAL 300      // 5 minutes between checks
#define SAMPLE_DURATION 5       // 5 seconds per sample
#define HISTORY_SIZE 288        // 24 hours at 5-minute intervals
#define MAX_INPUTS 100
#define MAX_WORKERS 10

// Color status thresholds (milliseconds)
#define THRESHOLD_GREEN 10.0
#define THRESHOLD_YELLOW 25.0
#define THRESHOLD_ORANGE 45.0

typedef struct {
    char timestamp[32];
    double offset_ms;
} HistorySample;

typedef struct {
    char id[64];                // e.g., "bet" (from [general] name)
    char name[128];             // e.g., "bet"
    char type[32];              // e.g., "udp", "srt", "rist", "hls"
    char address[64];           // Output address (e.g., "239.100.0.1")
    int port;                   // Output port (e.g., 10000)
    int video_pid;
    int audio_pid;              // Primary audio PID
    int running;                // Is the input service running?

    // Current measurement
    double current_offset_ms;
    char current_status[16];    // "green", "yellow", "orange", "red"
    int status_code;            // 0=green, 1=yellow, 2=orange, 3=red
    char last_check[32];

    // History (circular buffer)
    HistorySample history[HISTORY_SIZE];
    int history_count;
    int history_index;

    pthread_mutex_t lock;
} InputStatus;

typedef struct {
    InputStatus inputs[MAX_INPUTS];
    int input_count;
    volatile int running;
    pthread_mutex_t global_lock;
    time_t last_save;
} AppContext;

AppContext g_ctx;

// Forward declarations
void* check_thread(void* arg);
void* api_thread(void* arg);
void discover_inputs(void);
void check_input_avsync(InputStatus* input);
void save_data(void);
void load_data(void);
const char* get_status_color(double offset_ms);
int get_status_code(double offset_ms);

void signal_handler(int sig) {
    (void)sig;
    g_ctx.running = 0;
}

void init_context(void) {
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.running = 1;
    pthread_mutex_init(&g_ctx.global_lock, NULL);

    for (int i = 0; i < MAX_INPUTS; i++) {
        pthread_mutex_init(&g_ctx.inputs[i].lock, NULL);
    }
}

// Trim whitespace from string (in-place)
static void trim(char* str) {
    if (!str || !*str) return;

    // Trim leading
    char* start = str;
    while (*start && isspace((unsigned char)*start)) start++;

    // Trim trailing
    char* end = start + strlen(start) - 1;
    while (end > start && (isspace((unsigned char)*end) || *end == '\n' || *end == '\r')) {
        *end = '\0';
        end--;
    }

    // Shift if needed
    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
}

// Parse INI-style config file
// Config format:
// [general]
// name = bet
// type = udp
// enabled = 1
// [pids]
// video = 211
// audio = 221
// [output]
// address = 239.100.0.1
// port = 10000
int parse_config(const char* filepath, InputStatus* input) {
    FILE* fp = fopen(filepath, "r");
    if (!fp) return -1;

    char line[512];
    char section[64] = "";

    memset(input->id, 0, sizeof(input->id));
    memset(input->name, 0, sizeof(input->name));
    memset(input->type, 0, sizeof(input->type));
    memset(input->address, 0, sizeof(input->address));
    input->port = 0;
    input->video_pid = 0;
    input->audio_pid = 0;

    while (fgets(line, sizeof(line), fp)) {
        trim(line);

        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == ';' || line[0] == '\0') continue;

        // Section header
        if (line[0] == '[') {
            char* end = strchr(line, ']');
            if (end) {
                *end = '\0';
                snprintf(section, sizeof(section), "%s", line + 1);
            }
            continue;
        }

        // Key = Value
        char* eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char key[64], value[256];
        snprintf(key, sizeof(key), "%s", line);
        snprintf(value, sizeof(value), "%s", eq + 1);
        trim(key);
        trim(value);

        // Parse based on section
        if (strcmp(section, "general") == 0) {
            if (strcmp(key, "name") == 0) {
                snprintf(input->name, sizeof(input->name), "%s", value);
                snprintf(input->id, sizeof(input->id), "%s", value);
            } else if (strcmp(key, "type") == 0) {
                snprintf(input->type, sizeof(input->type), "%s", value);
            }
        } else if (strcmp(section, "pids") == 0) {
            if (strcmp(key, "video") == 0) {
                input->video_pid = atoi(value);
            } else if (strcmp(key, "audio") == 0) {
                // Take first audio PID if comma-separated
                input->audio_pid = atoi(value);
            }
        } else if (strcmp(section, "output") == 0) {
            // Read output address and port (this is what we monitor)
            if (strcmp(key, "address") == 0) {
                snprintf(input->address, sizeof(input->address), "%s", value);
            } else if (strcmp(key, "port") == 0) {
                input->port = atoi(value);
            }
        }
    }

    fclose(fp);

    // Debug output
    if (input->name[0]) {
        printf("  Parsed: %s (type=%s) - output=%s:%d, video=%d, audio=%d\n",
               input->name, input->type[0] ? input->type : "unknown",
               input->address, input->port,
               input->video_pid, input->audio_pid);
    }

    return 0;
}

// Check if a systemd service is running
int is_service_running(const char* input_name) {
    char cmd[256];
    // Service is named cari-udp-{name}, e.g., cari-udp-bet
    snprintf(cmd, sizeof(cmd), "systemctl is-active --quiet cari-udp-%s 2>/dev/null", input_name);
    return system(cmd) == 0;
}

// Discover all inputs from config files
void discover_inputs(void) {
    DIR* dir = opendir(CONFIG_DIR);
    if (!dir) {
        fprintf(stderr, "Cannot open config directory: %s\n", CONFIG_DIR);
        return;
    }

    pthread_mutex_lock(&g_ctx.global_lock);

    struct dirent* entry;
    int count = 0;

    printf("Scanning config directory: %s\n", CONFIG_DIR);

    while ((entry = readdir(dir)) != NULL && count < MAX_INPUTS) {
        if (entry->d_type != DT_REG) continue;

        const char* ext = strrchr(entry->d_name, '.');
        if (!ext || strcmp(ext, ".conf") != 0) continue;

        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s/%s", CONFIG_DIR, entry->d_name);

        printf("Reading config: %s\n", filepath);

        InputStatus* input = &g_ctx.inputs[count];

        // Preserve history if same input
        char old_id[64];
        snprintf(old_id, sizeof(old_id), "%s", input->id);
        int old_history_count = input->history_count;
        int old_history_index = input->history_index;
        HistorySample old_history[HISTORY_SIZE];
        if (old_history_count > 0) {
            memcpy(old_history, input->history, sizeof(old_history));
        }

        // Clear and reinit
        pthread_mutex_t saved_lock = input->lock;
        memset(input, 0, sizeof(InputStatus));
        input->lock = saved_lock;

        if (parse_config(filepath, input) == 0) {
            // Check if we have required info
            if (input->address[0] && input->port > 0 &&
                input->video_pid > 0 && input->audio_pid > 0) {

                // Check if service is running
                input->running = is_service_running(input->id);
                printf("  Service cari-udp-%s: %s\n", input->id,
                       input->running ? "RUNNING" : "not running");

                // Restore history if same input
                if (strcmp(old_id, input->id) == 0 && old_history_count > 0) {
                    memcpy(input->history, old_history, sizeof(input->history));
                    input->history_count = old_history_count;
                    input->history_index = old_history_index;
                }

                count++;
            } else {
                printf("  Skipping: missing required fields (addr=%s, port=%d, vpid=%d, apid=%d)\n",
                       input->address, input->port, input->video_pid, input->audio_pid);
            }
        }
    }

    g_ctx.input_count = count;
    pthread_mutex_unlock(&g_ctx.global_lock);
    closedir(dir);

    printf("Discovered %d inputs with A/V sync capability\n", count);
}

// Run tsp pcrextract and parse output
int measure_avsync(InputStatus* input, double* offset_ms) {
    char cmd[512];
    // Don't filter by PID - let tsp find PCR on any PID (might be PMT or separate PCR PID)
    snprintf(cmd, sizeof(cmd),
        "timeout %d tsp -I ip %s:%d "
        "-P pcrextract --pts --pcr --csv "
        "-O drop 2>&1",
        SAMPLE_DURATION + 2,
        input->address, input->port);

    printf("  Running: %s\n", cmd);
    fflush(stdout);

    FILE* fp = popen(cmd, "r");
    if (!fp) {
        printf("  popen failed\n");
        fflush(stdout);
        return -1;
    }

    char line[512];

    // Store PCR values with packet positions
    typedef struct { int packet; double pcr; } PcrSample;
    PcrSample pcrs[200];
    int pcr_count = 0;

    // Store PTS values with packet positions
    typedef struct { int packet; double pts; } PtsSample;
    PtsSample video_pts[100];
    PtsSample audio_pts[100];
    int video_count = 0;
    int audio_count = 0;

    // Skip header
    if (!fgets(line, sizeof(line), fp)) {
        pclose(fp);
        return -1;
    }

    // Parse CSV: PID,Packet,PID-Packet,Type,Count,Value,ValueOffset,OffsetFromPCR
    while (fgets(line, sizeof(line), fp)) {
        int pid = 0;
        int packet = 0;
        char type[16] = "";
        double value = 0;

        char* token;
        char* saveptr;
        int field = 0;

        char line_copy[512];
        snprintf(line_copy, sizeof(line_copy), "%s", line);

        token = strtok_r(line_copy, ",", &saveptr);
        while (token) {
            switch (field) {
                case 0: pid = atoi(token); break;
                case 1: packet = atoi(token); break;
                case 3: snprintf(type, sizeof(type), "%s", token); break;
                case 5: value = atof(token); break;
            }
            field++;
            token = strtok_r(NULL, ",", &saveptr);
        }

        // Collect PCR entries (from video PID which carries PCR)
        if (strcmp(type, "PCR") == 0 && pcr_count < 200) {
            pcrs[pcr_count].packet = packet;
            pcrs[pcr_count].pcr = value;
            pcr_count++;
        }
        // Collect PTS entries
        else if (strcmp(type, "PTS") == 0) {
            if (pid == input->video_pid && video_count < 100) {
                video_pts[video_count].packet = packet;
                video_pts[video_count].pts = value;
                video_count++;
            } else if (pid == input->audio_pid && audio_count < 100) {
                audio_pts[audio_count].packet = packet;
                audio_pts[audio_count].pts = value;
                audio_count++;
            }
        }
    }

    pclose(fp);

    printf("  Collected: %d PCR, %d video PTS, %d audio PTS\n",
           pcr_count, video_count, audio_count);
    fflush(stdout);

    if (pcr_count == 0 || video_count == 0 || audio_count == 0) {
        printf("  Failed: insufficient data\n");
        fflush(stdout);
        return -1;
    }

    // Helper macro to find interpolated PCR at a packet position
    #define GET_PCR_AT_PACKET(pkt, result) do { \
        int idx; \
        for (idx = 0; idx < pcr_count - 1; idx++) { \
            if (pcrs[idx+1].packet >= (pkt)) break; \
        } \
        if (idx >= pcr_count - 1) idx = pcr_count - 2; \
        if (idx < 0) idx = 0; \
        int p1 = pcrs[idx].packet; \
        int p2 = pcrs[idx+1].packet; \
        double pcr1 = pcrs[idx].pcr; \
        double pcr2 = pcrs[idx+1].pcr; \
        if (p2 == p1) { result = pcr1; } \
        else { \
            double ratio = (double)((pkt) - p1) / (double)(p2 - p1); \
            result = pcr1 + ratio * (pcr2 - pcr1); \
        } \
    } while(0)

    // Calculate average offset for video PTS
    // PTS is in 90kHz, PCR is in 27MHz
    // Convert PTS to 27MHz: PTS * 300
    double video_offset_sum = 0;
    for (int i = 0; i < video_count; i++) {
        double pts_27mhz = video_pts[i].pts * 300.0;
        double pcr_at_pkt;
        GET_PCR_AT_PACKET(video_pts[i].packet, pcr_at_pkt);
        video_offset_sum += (pts_27mhz - pcr_at_pkt);
    }
    double video_avg_offset = video_offset_sum / video_count;

    // Calculate average offset for audio PTS
    double audio_offset_sum = 0;
    for (int i = 0; i < audio_count; i++) {
        double pts_27mhz = audio_pts[i].pts * 300.0;
        double pcr_at_pkt;
        GET_PCR_AT_PACKET(audio_pts[i].packet, pcr_at_pkt);
        audio_offset_sum += (pts_27mhz - pcr_at_pkt);
    }
    double audio_avg_offset = audio_offset_sum / audio_count;

    #undef GET_PCR_AT_PACKET

    // A/V offset in 27MHz ticks, convert to ms
    double offset_ticks = audio_avg_offset - video_avg_offset;
    *offset_ms = offset_ticks / 27000.0;

    return 0;
}

const char* get_status_color(double offset_ms) {
    double abs_offset = fabs(offset_ms);
    if (abs_offset <= THRESHOLD_GREEN) return "green";
    if (abs_offset <= THRESHOLD_YELLOW) return "yellow";
    if (abs_offset <= THRESHOLD_ORANGE) return "orange";
    return "red";
}

int get_status_code(double offset_ms) {
    double abs_offset = fabs(offset_ms);
    if (abs_offset <= THRESHOLD_GREEN) return 0;
    if (abs_offset <= THRESHOLD_YELLOW) return 1;
    if (abs_offset <= THRESHOLD_ORANGE) return 2;
    return 3;
}

void check_input_avsync(InputStatus* input) {
    double offset_ms = 0;

    if (measure_avsync(input, &offset_ms) == 0) {
        pthread_mutex_lock(&input->lock);

        input->current_offset_ms = offset_ms;
        snprintf(input->current_status, sizeof(input->current_status), "%s", get_status_color(offset_ms));
        input->status_code = get_status_code(offset_ms);

        // Update timestamp
        time_t now = time(NULL);
        struct tm* tm = localtime(&now);
        strftime(input->last_check, sizeof(input->last_check), "%Y-%m-%dT%H:%M:%SZ", tm);

        // Add to history
        HistorySample* sample = &input->history[input->history_index];
        snprintf(sample->timestamp, sizeof(sample->timestamp), "%s", input->last_check);
        sample->offset_ms = offset_ms;

        input->history_index = (input->history_index + 1) % HISTORY_SIZE;
        if (input->history_count < HISTORY_SIZE) {
            input->history_count++;
        }

        pthread_mutex_unlock(&input->lock);

        printf("[%s] %s: A/V offset = %.2f ms (%s)\n",
               input->last_check, input->id, offset_ms, input->current_status);
    } else {
        printf("[CHECK] %s: Failed to measure A/V sync\n", input->id);
    }
}

// Background thread for periodic checks
void* check_thread(void* arg) {
    (void)arg;

    while (g_ctx.running) {
        // Refresh input list
        discover_inputs();

        // Check each running input
        for (int i = 0; i < g_ctx.input_count && g_ctx.running; i++) {
            InputStatus* input = &g_ctx.inputs[i];

            // Re-check if service is running
            input->running = is_service_running(input->id);

            if (input->running) {
                check_input_avsync(input);
            }

            // Small delay between checks to avoid overwhelming
            usleep(500000);  // 0.5 second
        }

        // Save data periodically
        time_t now = time(NULL);
        if (now - g_ctx.last_save >= 60) {
            save_data();
            g_ctx.last_save = now;
        }

        // Wait for next check interval
        for (int i = 0; i < CHECK_INTERVAL && g_ctx.running; i++) {
            sleep(1);
        }
    }

    return NULL;
}

// JSON helpers
void json_escape(const char* str, char* out, size_t out_size) {
    size_t j = 0;
    for (size_t i = 0; str[i] && j < out_size - 2; i++) {
        if (str[i] == '"' || str[i] == '\\') {
            out[j++] = '\\';
        }
        out[j++] = str[i];
    }
    out[j] = '\0';
}

// Build JSON for single input
int build_input_json(InputStatus* input, char* buf, size_t buf_size, int include_history) {
    char name_escaped[256];
    json_escape(input->name, name_escaped, sizeof(name_escaped));

    int len = snprintf(buf, buf_size,
        "{"
        "\"id\":\"%s\","
        "\"name\":\"%s\","
        "\"type\":\"%s\","
        "\"address\":\"%s:%d\","
        "\"video_pid\":%d,"
        "\"audio_pid\":%d,"
        "\"running\":%s,"
        "\"current\":{"
            "\"av_offset_ms\":%.2f,"
            "\"status\":\"%s\","
            "\"status_code\":%d,"
            "\"timestamp\":\"%s\""
        "}",
        input->id,
        name_escaped,
        input->type[0] ? input->type : "unknown",
        input->address, input->port,
        input->video_pid,
        input->audio_pid,
        input->running ? "true" : "false",
        input->current_offset_ms,
        input->current_status,
        input->status_code,
        input->last_check);

    if (include_history && len < (int)buf_size - 100) {
        len += snprintf(buf + len, buf_size - len, ",\"history\":[");

        // Output history in chronological order
        int start = (input->history_count < HISTORY_SIZE) ? 0 : input->history_index;
        for (int i = 0; i < input->history_count && len < (int)buf_size - 100; i++) {
            int idx = (start + i) % HISTORY_SIZE;
            if (i > 0) len += snprintf(buf + len, buf_size - len, ",");
            len += snprintf(buf + len, buf_size - len,
                "{\"ts\":\"%s\",\"offset_ms\":%.2f}",
                input->history[idx].timestamp,
                input->history[idx].offset_ms);
        }
        len += snprintf(buf + len, buf_size - len, "]");
    }

    len += snprintf(buf + len, buf_size - len, "}");
    return len;
}

// REST API handler
static MHD_Result api_handler(void* cls, struct MHD_Connection* connection,
                       const char* url, const char* method,
                       const char* version, const char* upload_data,
                       size_t* upload_data_size, void** con_cls) {
    (void)cls;
    (void)version;
    (void)upload_data;
    (void)upload_data_size;
    (void)con_cls;

    struct MHD_Response* response;
    int ret;
    char* buf = NULL;
    size_t buf_size = 1024 * 1024;  // 1MB buffer for history

    // Only handle GET
    if (strcmp(method, "GET") != 0) {
        const char* error = "{\"error\":\"Method not allowed\"}";
        response = MHD_create_response_from_buffer(strlen(error), (void*)error, MHD_RESPMEM_PERSISTENT);
        MHD_add_response_header(response, "Content-Type", "application/json");
        ret = MHD_queue_response(connection, MHD_HTTP_METHOD_NOT_ALLOWED, response);
        MHD_destroy_response(response);
        return ret;
    }

    buf = malloc(buf_size);
    if (!buf) {
        const char* error = "{\"error\":\"Out of memory\"}";
        response = MHD_create_response_from_buffer(strlen(error), (void*)error, MHD_RESPMEM_PERSISTENT);
        ret = MHD_queue_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, response);
        MHD_destroy_response(response);
        return ret;
    }

    int status = MHD_HTTP_OK;

    if (strcmp(url, "/health") == 0) {
        snprintf(buf, buf_size, "{\"status\":\"ok\",\"inputs\":%d}", g_ctx.input_count);
    }
    else if (strcmp(url, "/status") == 0) {
        // All inputs status (without history)
        int len = snprintf(buf, buf_size, "{\"inputs\":[");
        pthread_mutex_lock(&g_ctx.global_lock);
        for (int i = 0; i < g_ctx.input_count; i++) {
            if (i > 0) len += snprintf(buf + len, buf_size - len, ",");
            pthread_mutex_lock(&g_ctx.inputs[i].lock);
            len += build_input_json(&g_ctx.inputs[i], buf + len, buf_size - len, 0);
            pthread_mutex_unlock(&g_ctx.inputs[i].lock);
        }
        pthread_mutex_unlock(&g_ctx.global_lock);
        len += snprintf(buf + len, buf_size - len, "]}");
    }
    else if (strncmp(url, "/status/", 8) == 0) {
        // Single input status (without history)
        const char* input_id = url + 8;
        int found = 0;

        pthread_mutex_lock(&g_ctx.global_lock);
        for (int i = 0; i < g_ctx.input_count; i++) {
            if (strcmp(g_ctx.inputs[i].id, input_id) == 0) {
                pthread_mutex_lock(&g_ctx.inputs[i].lock);
                build_input_json(&g_ctx.inputs[i], buf, buf_size, 0);
                pthread_mutex_unlock(&g_ctx.inputs[i].lock);
                found = 1;
                break;
            }
        }
        pthread_mutex_unlock(&g_ctx.global_lock);

        if (!found) {
            snprintf(buf, buf_size, "{\"error\":\"Input not found\"}");
            status = MHD_HTTP_NOT_FOUND;
        }
    }
    else if (strncmp(url, "/history/", 9) == 0) {
        // Single input with history
        const char* input_id = url + 9;
        int found = 0;

        pthread_mutex_lock(&g_ctx.global_lock);
        for (int i = 0; i < g_ctx.input_count; i++) {
            if (strcmp(g_ctx.inputs[i].id, input_id) == 0) {
                pthread_mutex_lock(&g_ctx.inputs[i].lock);
                build_input_json(&g_ctx.inputs[i], buf, buf_size, 1);
                pthread_mutex_unlock(&g_ctx.inputs[i].lock);
                found = 1;
                break;
            }
        }
        pthread_mutex_unlock(&g_ctx.global_lock);

        if (!found) {
            snprintf(buf, buf_size, "{\"error\":\"Input not found\"}");
            status = MHD_HTTP_NOT_FOUND;
        }
    }
    else {
        snprintf(buf, buf_size, "{\"error\":\"Not found\"}");
        status = MHD_HTTP_NOT_FOUND;
    }

    response = MHD_create_response_from_buffer(strlen(buf), buf, MHD_RESPMEM_MUST_FREE);
    MHD_add_response_header(response, "Content-Type", "application/json");
    MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
    ret = MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);

    return ret;
}

void save_data(void) {
    FILE* fp = fopen(DATA_FILE, "w");
    if (!fp) {
        fprintf(stderr, "Cannot save data to %s\n", DATA_FILE);
        return;
    }

    fprintf(fp, "{\"inputs\":[");

    pthread_mutex_lock(&g_ctx.global_lock);
    for (int i = 0; i < g_ctx.input_count; i++) {
        if (i > 0) fprintf(fp, ",");

        char buf[1024 * 100];
        pthread_mutex_lock(&g_ctx.inputs[i].lock);
        build_input_json(&g_ctx.inputs[i], buf, sizeof(buf), 1);
        pthread_mutex_unlock(&g_ctx.inputs[i].lock);

        fprintf(fp, "%s", buf);
    }
    pthread_mutex_unlock(&g_ctx.global_lock);

    fprintf(fp, "]}\n");
    fclose(fp);

    printf("Saved data to %s\n", DATA_FILE);
}

void load_data(void) {
    // TODO: Parse JSON and restore history
    // For now, start fresh each time
    printf("Starting with fresh data (persistence loading not yet implemented)\n");
}

void print_help(const char* prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("A/V Sync Monitor Service\n\n");
    printf("Options:\n");
    printf("  --help         Show this help\n");
    printf("\n");
    printf("API Endpoints (port %d):\n", API_PORT);
    printf("  GET /health            - Health check\n");
    printf("  GET /status            - All inputs status\n");
    printf("  GET /status/{id}       - Single input status\n");
    printf("  GET /history/{id}      - Single input with 24h history\n");
    printf("\n");
    printf("Configuration:\n");
    printf("  Check interval: %d seconds\n", CHECK_INTERVAL);
    printf("  Sample duration: %d seconds\n", SAMPLE_DURATION);
    printf("  History size: %d samples (24 hours)\n", HISTORY_SIZE);
    printf("\n");
    printf("Status Thresholds:\n");
    printf("  Green:  0-%.0f ms\n", THRESHOLD_GREEN);
    printf("  Yellow: %.0f-%.0f ms\n", THRESHOLD_GREEN, THRESHOLD_YELLOW);
    printf("  Orange: %.0f-%.0f ms\n", THRESHOLD_YELLOW, THRESHOLD_ORANGE);
    printf("  Red:    >%.0f ms\n", THRESHOLD_ORANGE);
}

int main(int argc, char* argv[]) {
    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help(argv[0]);
            return 0;
        }
    }

    printf("CariTranscoder A/V Sync Monitor\n");
    printf("API port: %d\n", API_PORT);
    printf("Check interval: %d seconds\n", CHECK_INTERVAL);

    // Setup
    init_context();
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Load previous data if exists
    load_data();

    // Initial discovery
    discover_inputs();

    // Start HTTP API
    struct MHD_Daemon* daemon = MHD_start_daemon(
        MHD_USE_SELECT_INTERNALLY,
        API_PORT,
        NULL, NULL,
        &api_handler, NULL,
        MHD_OPTION_END);

    if (!daemon) {
        fprintf(stderr, "Failed to start HTTP server on port %d\n", API_PORT);
        return 1;
    }

    printf("API server started on port %d\n", API_PORT);

    // Start check thread
    pthread_t check_tid;
    pthread_create(&check_tid, NULL, check_thread, NULL);

    // Main loop - just wait for signals
    while (g_ctx.running) {
        sleep(1);
    }

    printf("\nShutting down...\n");

    // Cleanup
    pthread_join(check_tid, NULL);
    MHD_stop_daemon(daemon);

    // Final save
    save_data();

    printf("Done.\n");
    return 0;
}
