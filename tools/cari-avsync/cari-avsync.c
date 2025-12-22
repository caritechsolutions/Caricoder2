/*
 * cari-avsync.c - A/V Sync Monitor Service
 *
 * Workflow:
 * 1. CAPTURE - Run tsp to extract PTS values from live stream
 * 2. PARSE & SORT - Extract PTS entries, sort chronologically
 * 3. FILTER - Keep only alternating video/audio frames
 * 4. COMPARE - Calculate gaps between frame pairs, separate into A→V and V→A
 * 5. ANALYZE - Compare to baseline, detect patterns, assign status/score
 * 6. SAVE - Store results with trending data for API consumption
 *
 * Copyright (c) 2024 CariTech Solutions
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
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
#define BASELINE_FILE "/var/lib/caritrans/avsync_baseline.json"
#define CHECK_INTERVAL 300      // 5 minutes between checks
#define SAMPLE_DURATION 10      // 10 seconds per sample (user specified)
#define HISTORY_SIZE 288        // 24 hours at 5-minute intervals
#define MAX_INPUTS 100
#define MAX_PTS_SAMPLES 500     // Max PTS samples to capture

// Alarm thresholds (milliseconds)
#define THRESHOLD_OK 300.0       // < 300ms = OK
#define THRESHOLD_WARNING 600.0  // 300-600ms = WARNING, > 600ms = ERROR

// PTS sample for sorting
typedef struct {
    int pid;
    int64_t pts;           // 90kHz ticks
    int packet_num;        // Original packet order
} PtsSample;

// Single measurement result - just the means with timestamp
typedef struct {
    char timestamp[32];
    double a2v_avg_ms;      // Audio→Video mean gap
    double v2a_avg_ms;      // Video→Audio mean gap
    int a2v_count;          // Sample count used
    int v2a_count;          // Sample count used
    char status[16];        // "OK", "WARNING", "ERROR"
} MeasurementResult;

typedef struct {
    char id[64];
    char name[128];
    char type[32];
    char address[64];
    int port;
    int video_pid;
    int audio_pid;
    int running;

    // Current measurement
    MeasurementResult current;

    // History (circular buffer)
    MeasurementResult history[HISTORY_SIZE];
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
void discover_inputs(void);
void check_input_avsync(InputStatus* input);
void save_data(void);
void load_data(void);

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

    char* start = str;
    while (*start && isspace((unsigned char)*start)) start++;

    char* end = start + strlen(start) - 1;
    while (end > start && (isspace((unsigned char)*end) || *end == '\n' || *end == '\r')) {
        *end = '\0';
        end--;
    }

    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
}

// Parse INI-style config file
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

        if (line[0] == '#' || line[0] == ';' || line[0] == '\0') continue;

        if (line[0] == '[') {
            char* end = strchr(line, ']');
            if (end) {
                *end = '\0';
                snprintf(section, sizeof(section), "%s", line + 1);
            }
            continue;
        }

        char* eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char key[64], value[256];
        snprintf(key, sizeof(key), "%s", line);
        snprintf(value, sizeof(value), "%s", eq + 1);
        trim(key);
        trim(value);

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
                input->audio_pid = atoi(value);
            }
        } else if (strcmp(section, "output") == 0) {
            if (strcmp(key, "address") == 0) {
                snprintf(input->address, sizeof(input->address), "%s", value);
            } else if (strcmp(key, "port") == 0) {
                input->port = atoi(value);
            }
        }
    }

    fclose(fp);

    if (input->name[0]) {
        printf("  Parsed: %s (type=%s) - output=%s:%d, video=%d, audio=%d\n",
               input->name, input->type[0] ? input->type : "unknown",
               input->address, input->port,
               input->video_pid, input->audio_pid);
    }

    return 0;
}

// Check if systemd service is running
int is_service_running(const char* input_name, const char* input_type) {
    char cmd[256];
    // UDP uses cari-udp-{name}, SRT uses cari-srt-{name}
    if (strcmp(input_type, "srt") == 0) {
        snprintf(cmd, sizeof(cmd), "systemctl is-active --quiet cari-srt-%s 2>/dev/null", input_name);
    } else {
        snprintf(cmd, sizeof(cmd), "systemctl is-active --quiet cari-udp-%s 2>/dev/null", input_name);
    }
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

        // Preserve existing history if same input
        char old_id[64];
        snprintf(old_id, sizeof(old_id), "%s", input->id);
        int old_history_count = input->history_count;
        int old_history_index = input->history_index;
        MeasurementResult old_history[HISTORY_SIZE];
        if (old_history_count > 0) {
            memcpy(old_history, input->history, sizeof(old_history));
        }

        pthread_mutex_t saved_lock = input->lock;
        memset(input, 0, sizeof(InputStatus));
        input->lock = saved_lock;

        if (parse_config(filepath, input) == 0) {
            if (input->address[0] && input->port > 0 &&
                input->video_pid > 0 && input->audio_pid > 0) {

                input->running = is_service_running(input->id, input->type);
                const char* svc_prefix = (strcmp(input->type, "srt") == 0) ? "cari-srt" : "cari-udp";
                printf("  Service %s-%s: %s\n", svc_prefix, input->id,
                       input->running ? "RUNNING" : "not running");

                // Restore history if same input
                if (strcmp(old_id, input->id) == 0 && old_history_count > 0) {
                    memcpy(input->history, old_history, sizeof(input->history));
                    input->history_count = old_history_count;
                    input->history_index = old_history_index;
                }

                count++;
            } else {
                printf("  Skipping: missing required fields\n");
            }
        }
    }

    g_ctx.input_count = count;
    pthread_mutex_unlock(&g_ctx.global_lock);
    closedir(dir);

    printf("Discovered %d inputs with A/V sync capability\n", count);
}

// Comparison function for qsort - sort by PTS value
static int compare_pts(const void* a, const void* b) {
    const PtsSample* pa = (const PtsSample*)a;
    const PtsSample* pb = (const PtsSample*)b;

    if (pa->pts < pb->pts) return -1;
    if (pa->pts > pb->pts) return 1;
    return 0;
}

// STEP 1-4: Capture, Parse, Sort, Filter, Compare
int measure_avsync(InputStatus* input, MeasurementResult* result) {
    char cmd[512];

    memset(result, 0, sizeof(MeasurementResult));

    // STEP 1: CAPTURE - Run tsp to extract PTS values
    snprintf(cmd, sizeof(cmd),
        "timeout %d tsp -I ip %s:%d "
        "-P pcrextract --pid %d --pid %d --pts --csv "
        "-O drop 2>&1",
        SAMPLE_DURATION + 2,
        input->address, input->port,
        input->video_pid, input->audio_pid);

    printf("  [CAPTURE] Running: %s\n", cmd);
    fflush(stdout);

    FILE* fp = popen(cmd, "r");
    if (!fp) {
        printf("  popen failed\n");
        return -1;
    }

    char line[512];

    // Collect all PTS samples
    PtsSample samples[MAX_PTS_SAMPLES];
    int sample_count = 0;

    // Skip header
    if (!fgets(line, sizeof(line), fp)) {
        pclose(fp);
        return -1;
    }

    // STEP 2: PARSE - Extract PTS entries
    while (fgets(line, sizeof(line), fp) && sample_count < MAX_PTS_SAMPLES) {
        int pid = 0;
        int packet = 0;
        char type[16] = "";
        int64_t value = 0;

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
                case 5: value = strtoll(token, NULL, 10); break;
            }
            field++;
            token = strtok_r(NULL, ",", &saveptr);
        }

        // Collect only PTS entries
        if (strcmp(type, "PTS") == 0 && (pid == input->video_pid || pid == input->audio_pid)) {
            samples[sample_count].pid = pid;
            samples[sample_count].pts = value;
            samples[sample_count].packet_num = packet;
            sample_count++;
        }
    }

    pclose(fp);

    printf("  [PARSE] Collected %d PTS samples\n", sample_count);

    if (sample_count < 10) {
        printf("  Failed: insufficient PTS samples\n");
        return -1;
    }

    // STEP 2 (cont): SORT by PTS value (chronological presentation order)
    qsort(samples, sample_count, sizeof(PtsSample), compare_pts);

    printf("  [SORT] Sorted %d samples by PTS\n", sample_count);

    // STEP 3: FILTER - Remove consecutive same-PID frames
    // Keep only alternating video/audio frames
    PtsSample filtered[MAX_PTS_SAMPLES];
    int filtered_count = 0;

    for (int i = 0; i < sample_count; i++) {
        // Skip if current PID == next PID (not alternating)
        if (i < sample_count - 1 && samples[i].pid == samples[i + 1].pid) {
            continue;
        }
        filtered[filtered_count++] = samples[i];
    }

    printf("  [FILTER] After filter: %d alternating frames\n", filtered_count);

    if (filtered_count < 4) {
        printf("  Failed: insufficient alternating frames\n");
        return -1;
    }

    // STEP 4: COMPARE - Calculate gaps between frame pairs
    double a2v_gaps[MAX_PTS_SAMPLES];  // Audio→Video gaps
    double v2a_gaps[MAX_PTS_SAMPLES];  // Video→Audio gaps
    int a2v_count = 0;
    int v2a_count = 0;

    for (int i = 0; i < filtered_count - 1; i++) {
        int64_t pts_diff = filtered[i + 1].pts - filtered[i].pts;
        double gap_ms = (double)pts_diff / 90.0;  // Convert 90kHz ticks to ms

        // Classify based on direction
        if (filtered[i].pid == input->audio_pid && filtered[i + 1].pid == input->video_pid) {
            // Audio→Video transition
            a2v_gaps[a2v_count++] = gap_ms;
        } else if (filtered[i].pid == input->video_pid && filtered[i + 1].pid == input->audio_pid) {
            // Video→Audio transition
            v2a_gaps[v2a_count++] = gap_ms;
        }
    }

    printf("  [COMPARE] Gaps: %d A→V, %d V→A\n", a2v_count, v2a_count);

    // Calculate mean for Audio→Video gaps (excluding max value as it's likely a capture error)
    if (a2v_count > 1) {
        // Find max index
        int max_idx = 0;
        for (int i = 1; i < a2v_count; i++) {
            if (a2v_gaps[i] > a2v_gaps[max_idx]) max_idx = i;
        }
        // Calculate mean excluding max
        double sum = 0;
        for (int i = 0; i < a2v_count; i++) {
            if (i != max_idx) sum += a2v_gaps[i];
        }
        result->a2v_avg_ms = sum / (a2v_count - 1);
        result->a2v_count = a2v_count - 1;  // Report count without the excluded max
    } else if (a2v_count == 1) {
        result->a2v_avg_ms = a2v_gaps[0];
        result->a2v_count = 1;
    }

    // Calculate mean for Video→Audio gaps (excluding max value as it's likely a capture error)
    if (v2a_count > 1) {
        // Find max index
        int max_idx = 0;
        for (int i = 1; i < v2a_count; i++) {
            if (v2a_gaps[i] > v2a_gaps[max_idx]) max_idx = i;
        }
        // Calculate mean excluding max
        double sum = 0;
        for (int i = 0; i < v2a_count; i++) {
            if (i != max_idx) sum += v2a_gaps[i];
        }
        result->v2a_avg_ms = sum / (v2a_count - 1);
        result->v2a_count = v2a_count - 1;  // Report count without the excluded max
    } else if (v2a_count == 1) {
        result->v2a_avg_ms = v2a_gaps[0];
        result->v2a_count = 1;
    }

    // Set timestamp
    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    strftime(result->timestamp, sizeof(result->timestamp), "%Y-%m-%d %H:%M:%S", tm);

    // Set status based on A→V mean
    if (result->a2v_avg_ms < THRESHOLD_OK) {
        snprintf(result->status, sizeof(result->status), "OK");
    } else if (result->a2v_avg_ms < THRESHOLD_WARNING) {
        snprintf(result->status, sizeof(result->status), "WARNING");
    } else {
        snprintf(result->status, sizeof(result->status), "ERROR");
    }

    printf("  [STATS] A→V mean: %.1fms, V→A mean: %.1fms, Status: %s\n",
           result->a2v_avg_ms, result->v2a_avg_ms, result->status);

    return 0;
}

// Save result to history
void check_input_avsync(InputStatus* input) {
    MeasurementResult result;

    if (measure_avsync(input, &result) == 0) {
        pthread_mutex_lock(&input->lock);

        // Store current result
        input->current = result;

        // Add to history
        input->history[input->history_index] = result;
        input->history_index = (input->history_index + 1) % HISTORY_SIZE;
        if (input->history_count < HISTORY_SIZE) {
            input->history_count++;
        }

        pthread_mutex_unlock(&input->lock);

        printf("[%s] %s: Status=%s, A→V=%.1fms, V→A=%.1fms\n",
               result.timestamp, input->id, result.status,
               result.a2v_avg_ms, result.v2a_avg_ms);
    } else {
        printf("[CHECK] %s: Failed to measure A/V sync\n", input->id);
    }
}

// Background thread for periodic checks
void* check_thread(void* arg) {
    (void)arg;

    while (g_ctx.running) {
        discover_inputs();

        for (int i = 0; i < g_ctx.input_count && g_ctx.running; i++) {
            InputStatus* input = &g_ctx.inputs[i];

            input->running = is_service_running(input->id, input->type);

            if (input->running) {
                check_input_avsync(input);
            }

            usleep(500000);  // 0.5 second between checks
        }

        // Save data periodically
        time_t now = time(NULL);
        if (now - g_ctx.last_save >= 60) {
            save_data();
            g_ctx.last_save = now;
        }

        // Wait for next interval
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

// Build JSON for measurement result
int build_result_json(MeasurementResult* result, char* buf, size_t buf_size) {
    return snprintf(buf, buf_size,
        "{"
        "\"timestamp\":\"%s\","
        "\"a2v_mean_ms\":%.2f,"
        "\"v2a_mean_ms\":%.2f,"
        "\"a2v_samples\":%d,"
        "\"v2a_samples\":%d,"
        "\"status\":\"%s\""
        "}",
        result->timestamp,
        result->a2v_avg_ms,
        result->v2a_avg_ms,
        result->a2v_count,
        result->v2a_count,
        result->status);
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
        "\"current\":",
        input->id,
        name_escaped,
        input->type[0] ? input->type : "unknown",
        input->address, input->port,
        input->video_pid,
        input->audio_pid,
        input->running ? "true" : "false");

    // Add current measurement
    len += build_result_json(&input->current, buf + len, buf_size - len);

    // Add history if requested
    if (include_history && len < (int)buf_size - 1000) {
        len += snprintf(buf + len, buf_size - len, ",\"history\":[");

        int start = (input->history_count < HISTORY_SIZE) ? 0 : input->history_index;
        for (int i = 0; i < input->history_count && len < (int)buf_size - 500; i++) {
            int idx = (start + i) % HISTORY_SIZE;
            if (i > 0) len += snprintf(buf + len, buf_size - len, ",");
            len += build_result_json(&input->history[idx], buf + len, buf_size - len);
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
    size_t buf_size = 2 * 1024 * 1024;  // 2MB buffer for full history

    if (strcmp(method, "GET") != 0) {
        const char* error = "{\"error\":\"Method not allowed\"}";
        response = MHD_create_response_from_buffer(strlen(error), (void*)error, MHD_RESPMEM_PERSISTENT);
        MHD_add_response_header(response, "Content-Type", "application/json");
        MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
        ret = MHD_queue_response(connection, MHD_HTTP_METHOD_NOT_ALLOWED, response);
        MHD_destroy_response(response);
        return ret;
    }

    buf = malloc(buf_size);
    if (!buf) {
        const char* error = "{\"error\":\"Out of memory\"}";
        response = MHD_create_response_from_buffer(strlen(error), (void*)error, MHD_RESPMEM_PERSISTENT);
        MHD_add_response_header(response, "Content-Type", "application/json");
        MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
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
        snprintf(buf, buf_size,
            "{\"error\":\"Not found\","
            "\"endpoints\":["
                "\"/health\","
                "\"/status\","
                "\"/status/{id}\","
                "\"/history/{id}\""
            "]}");
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

        char buf[1024 * 500];
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
    FILE* fp = fopen(DATA_FILE, "r");
    if (!fp) {
        printf("No saved data found at %s, starting fresh\n", DATA_FILE);
        return;
    }

    // Read entire file
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 10 * 1024 * 1024) {  // Max 10MB
        fclose(fp);
        printf("Invalid data file size, starting fresh\n");
        return;
    }

    char* data = malloc(fsize + 1);
    if (!data) {
        fclose(fp);
        return;
    }

    size_t read_size = fread(data, 1, fsize, fp);
    fclose(fp);
    data[read_size] = '\0';

    printf("Loading saved data from %s (%ld bytes)\n", DATA_FILE, fsize);

    // Parse JSON manually - find each input
    char* ptr = data;
    int loaded_count = 0;

    while ((ptr = strstr(ptr, "\"id\":\"")) != NULL) {
        ptr += 6;  // Skip "id":"

        // Extract ID
        char id[64] = {0};
        int i = 0;
        while (*ptr && *ptr != '"' && i < 63) {
            id[i++] = *ptr++;
        }
        id[i] = '\0';

        if (strlen(id) == 0) continue;

        // Find this input in our list
        InputStatus* input = NULL;
        for (int j = 0; j < g_ctx.input_count; j++) {
            if (strcmp(g_ctx.inputs[j].id, id) == 0) {
                input = &g_ctx.inputs[j];
                break;
            }
        }

        if (!input) {
            // Input not found in current config, skip to next
            continue;
        }

        // Find history array for this input
        char* hist_start = strstr(ptr, "\"history\":[");
        char* next_input = strstr(ptr, "},{\"id\":");  // Next input marker

        if (!hist_start || (next_input && hist_start > next_input)) {
            continue;  // No history for this input
        }

        hist_start += 11;  // Skip "history":[

        pthread_mutex_lock(&input->lock);
        input->history_count = 0;
        input->history_index = 0;

        // Parse each history entry
        while (*hist_start && input->history_count < HISTORY_SIZE) {
            if (*hist_start == ']') break;  // End of history array

            // Find next history object
            char* obj_start = strchr(hist_start, '{');
            if (!obj_start) break;

            char* obj_end = strchr(obj_start, '}');
            if (!obj_end) break;

            // Parse timestamp
            char* ts = strstr(obj_start, "\"timestamp\":\"");
            if (ts && ts < obj_end) {
                ts += 13;
                int k = 0;
                while (*ts && *ts != '"' && k < 31) {
                    input->history[input->history_count].timestamp[k++] = *ts++;
                }
                input->history[input->history_count].timestamp[k] = '\0';
            }

            // Parse a2v_mean_ms
            char* a2v = strstr(obj_start, "\"a2v_mean_ms\":");
            if (a2v && a2v < obj_end) {
                a2v += 14;
                input->history[input->history_count].a2v_avg_ms = atof(a2v);
            }

            // Parse v2a_mean_ms
            char* v2a = strstr(obj_start, "\"v2a_mean_ms\":");
            if (v2a && v2a < obj_end) {
                v2a += 14;
                input->history[input->history_count].v2a_avg_ms = atof(v2a);
            }

            // Parse a2v_samples
            char* a2v_s = strstr(obj_start, "\"a2v_samples\":");
            if (a2v_s && a2v_s < obj_end) {
                a2v_s += 14;
                input->history[input->history_count].a2v_count = atoi(a2v_s);
            }

            // Parse v2a_samples
            char* v2a_s = strstr(obj_start, "\"v2a_samples\":");
            if (v2a_s && v2a_s < obj_end) {
                v2a_s += 14;
                input->history[input->history_count].v2a_count = atoi(v2a_s);
            }

            // Parse status
            char* status = strstr(obj_start, "\"status\":\"");
            if (status && status < obj_end) {
                status += 10;
                int k = 0;
                while (*status && *status != '"' && k < 15) {
                    input->history[input->history_count].status[k++] = *status++;
                }
                input->history[input->history_count].status[k] = '\0';
            }

            input->history_count++;
            hist_start = obj_end + 1;
        }

        // Set current to last history entry
        if (input->history_count > 0) {
            input->current = input->history[input->history_count - 1];
            input->history_index = input->history_count % HISTORY_SIZE;
        }

        pthread_mutex_unlock(&input->lock);
        loaded_count++;

        printf("  Loaded %d history entries for %s\n", input->history_count, id);
    }

    free(data);
    printf("Restored data for %d inputs\n", loaded_count);
}

void print_help(const char* prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("A/V Sync Monitor Service\n\n");
    printf("Options:\n");
    printf("  --help         Show this help\n");
    printf("\n");
    printf("API Endpoints (port %d):\n", API_PORT);
    printf("  GET /health            - Health check\n");
    printf("  GET /status            - All inputs current status\n");
    printf("  GET /status/{id}       - Single input status\n");
    printf("  GET /history/{id}      - Single input with 24h history\n");
    printf("\n");
    printf("Workflow:\n");
    printf("  1. CAPTURE   - Run tsp to extract PTS values (10s sample)\n");
    printf("  2. PARSE     - Extract PTS entries for video/audio PIDs\n");
    printf("  3. SORT      - Sort by PTS value (presentation order)\n");
    printf("  4. FILTER    - Keep only alternating video/audio frames\n");
    printf("  5. COMPARE   - Calculate A→V and V→A mean gaps (max excluded)\n");
    printf("  6. SAVE      - Store results with timestamp for trending\n");
    printf("\n");
    printf("Status Thresholds (A→V mean):\n");
    printf("  OK:      < %.0fms\n", THRESHOLD_OK);
    printf("  WARNING: %.0fms - %.0fms\n", THRESHOLD_OK, THRESHOLD_WARNING);
    printf("  ERROR:   > %.0fms\n", THRESHOLD_WARNING);
}

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help(argv[0]);
            return 0;
        }
    }

    printf("CariTranscoder A/V Sync Monitor\n");
    printf("API port: %d\n", API_PORT);
    printf("Check interval: %d seconds\n", CHECK_INTERVAL);
    printf("Sample duration: %d seconds\n", SAMPLE_DURATION);

    init_context();
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    discover_inputs();
    load_data();

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

    pthread_t check_tid;
    pthread_create(&check_tid, NULL, check_thread, NULL);

    while (g_ctx.running) {
        sleep(1);
    }

    printf("\nShutting down...\n");

    pthread_join(check_tid, NULL);
    MHD_stop_daemon(daemon);

    save_data();

    printf("Done.\n");
    return 0;
}
