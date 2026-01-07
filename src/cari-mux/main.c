/*
 * CariTranscoder - TSDuck Mux Application
 * Copyright (c) 2024 CariTech Solutions
 *
 * Reads muxer configuration and executes TSDuck tsp command
 * to multiplex multiple SPTS into MPTS with PSI/SI generation.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

#include "config.h"
#include "logging.h"

#define VERSION "2.0.0"
#define MAX_SERVICES 16
#define MAX_CMD_LEN 8192
#define MAX_ARGS 256

/* Service definition */
typedef struct {
    int enabled;
    char source_type[32];
    char source_id[64];
    char source_address[64];
    int source_port;
    int program_number;
    char service_name[128];
    char service_provider[128];
    int service_type;
    int pmt_pid;
    int is_pcr_reference;
    /* PIDs for the output MPTS (after remapping) */
    int video_pid;
    int audio_pid;
    /* Original PIDs from source SPTS (for remapping) */
    int source_video_pid;
    int source_audio_pid;
} mux_service_t;

/* Mux state */
typedef struct {
    config_t config;
    char config_file[256];
    char id[64];
    char name[128];

    /* Output settings */
    int output_bitrate;
    char output_address[64];
    int output_port;

    /* Network settings */
    int network_id;
    char network_name[64];
    int ts_id;
    int original_network_id;

    /* PSI intervals */
    int pat_interval;
    int pmt_interval;
    int sdt_interval;
    int nit_interval;

    /* Services */
    mux_service_t services[MAX_SERVICES];
    int service_count;
    int pcr_reference_service;

    /* Runtime */
    volatile int running;
    pid_t tsp_pid;
} mux_state_t;

static mux_state_t g_state = {0};

static void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        CARI_LOG_INFO("Received signal %d, shutting down...", signum);
        g_state.running = 0;

        /* Forward signal to tsp child process */
        if (g_state.tsp_pid > 0) {
            kill(g_state.tsp_pid, SIGTERM);
        }
    }
}

static int load_config(mux_state_t *state) {
    if (config_load(&state->config, state->config_file) != 0) {
        return -1;
    }

    /* Muxer identification */
    strncpy(state->id,
            config_get_string(&state->config, "muxer", "id", "mux-1"),
            sizeof(state->id) - 1);

    strncpy(state->name,
            config_get_string(&state->config, "muxer", "name", state->id),
            sizeof(state->name) - 1);

    /* Output settings */
    state->output_bitrate = config_get_int(&state->config, "output", "output_bitrate", 20000000);
    strncpy(state->output_address,
            config_get_string(&state->config, "output", "address", "239.1.1.1"),
            sizeof(state->output_address) - 1);
    state->output_port = config_get_int(&state->config, "output", "port", 5000);

    /* Network settings */
    state->network_id = config_get_int(&state->config, "network", "network_id", 1);
    strncpy(state->network_name,
            config_get_string(&state->config, "network", "network_name", "CariTrans"),
            sizeof(state->network_name) - 1);
    state->ts_id = config_get_int(&state->config, "network", "ts_id", 1);
    state->original_network_id = config_get_int(&state->config, "network", "original_network_id", 1);

    /* PSI intervals */
    state->pat_interval = config_get_int(&state->config, "tsduck", "pat_interval", 100);
    state->pmt_interval = config_get_int(&state->config, "tsduck", "pmt_interval", 100);
    state->sdt_interval = config_get_int(&state->config, "tsduck", "sdt_interval", 500);
    state->nit_interval = config_get_int(&state->config, "tsduck", "nit_interval", 10000);

    /* Parse services */
    state->service_count = 0;
    state->pcr_reference_service = 0;

    for (int i = 1; i <= MAX_SERVICES; i++) {
        char key[128];

        snprintf(key, sizeof(key), "service.%d.enabled", i);
        if (!config_get_bool(&state->config, "services", key, false)) {
            continue;
        }

        mux_service_t *svc = &state->services[state->service_count];
        svc->enabled = 1;

        snprintf(key, sizeof(key), "service.%d.source_type", i);
        strncpy(svc->source_type,
                config_get_string(&state->config, "services", key, "input"),
                sizeof(svc->source_type) - 1);

        snprintf(key, sizeof(key), "service.%d.source_id", i);
        strncpy(svc->source_id,
                config_get_string(&state->config, "services", key, ""),
                sizeof(svc->source_id) - 1);

        snprintf(key, sizeof(key), "service.%d.source_address", i);
        strncpy(svc->source_address,
                config_get_string(&state->config, "services", key, ""),
                sizeof(svc->source_address) - 1);

        snprintf(key, sizeof(key), "service.%d.source_port", i);
        svc->source_port = config_get_int(&state->config, "services", key, 0);

        snprintf(key, sizeof(key), "service.%d.program_number", i);
        svc->program_number = config_get_int(&state->config, "services", key, 1000 + i);

        snprintf(key, sizeof(key), "service.%d.service_name", i);
        strncpy(svc->service_name,
                config_get_string(&state->config, "services", key, ""),
                sizeof(svc->service_name) - 1);

        snprintf(key, sizeof(key), "service.%d.service_provider", i);
        strncpy(svc->service_provider,
                config_get_string(&state->config, "services", key, "CariTrans"),
                sizeof(svc->service_provider) - 1);

        snprintf(key, sizeof(key), "service.%d.service_type", i);
        svc->service_type = config_get_int(&state->config, "services", key, 0x01);

        snprintf(key, sizeof(key), "service.%d.pmt_pid", i);
        svc->pmt_pid = config_get_int(&state->config, "services", key, 256 + state->service_count * 256);

        snprintf(key, sizeof(key), "service.%d.is_pcr_reference", i);
        svc->is_pcr_reference = config_get_bool(&state->config, "services", key, false);

        /* Source PIDs (from input SPTS - typically all same due to encoder output) */
        snprintf(key, sizeof(key), "service.%d.source_video_pid", i);
        svc->source_video_pid = config_get_int(&state->config, "services", key, 211);

        snprintf(key, sizeof(key), "service.%d.source_audio_pid", i);
        svc->source_audio_pid = config_get_int(&state->config, "services", key, 221);

        /* Output PIDs (unique per service in MPTS) */
        snprintf(key, sizeof(key), "service.%d.video_pid", i);
        svc->video_pid = config_get_int(&state->config, "services", key,
                                        100 + state->service_count * 100);

        snprintf(key, sizeof(key), "service.%d.audio_pid", i);
        svc->audio_pid = config_get_int(&state->config, "services", key,
                                        101 + state->service_count * 100);

        if (svc->is_pcr_reference && state->pcr_reference_service == 0) {
            state->pcr_reference_service = svc->program_number;
        }

        if (svc->source_address[0] && svc->source_port > 0) {
            state->service_count++;
            CARI_LOG_INFO("Service %d: %s (%s:%d) -> Program %d (V:%d A:%d PMT:%d)",
                     state->service_count, svc->service_name,
                     svc->source_address, svc->source_port, svc->program_number,
                     svc->video_pid, svc->audio_pid, svc->pmt_pid);
        }
    }

    /* Default PCR reference to first service if not set */
    if (state->pcr_reference_service == 0 && state->service_count > 0) {
        state->pcr_reference_service = state->services[0].program_number;
    }

    CARI_LOG_INFO("Configured: %s (%s) - %d services, %d bps output",
             state->name, state->id, state->service_count, state->output_bitrate);

    return 0;
}

/*
 * Build TSDuck tsp command arguments
 *
 * Strategy for MPTS creation:
 * 1. Use null input as stuffing source (provides CBR padding)
 * 2. Merge each SPTS with --no-psi-merge (we inject our own tables)
 *    - Inside subprocess: filter out PSI (PIDs 0-20), remap content PIDs
 * 3. Inject our own PAT, SDT, and PMT tables with correct structure
 * 4. Regulate output for CBR
 *
 * Result: Clean MPTS with each SPTS as separate program/service
 */
static int build_tsp_args(mux_state_t *state, char **argv, int max_args) {
    int argc = 0;

    /* Static buffers for string arguments (must persist after function returns) */
    static char bitrate_str[32];
    static char merge_cmds[MAX_SERVICES][1024];
    static char pat_xml[4096];
    static char sdt_xml[8192];
    static char pmt_xml[MAX_SERVICES][2048];
    static char pat_bitrate[32];
    static char sdt_bitrate[32];
    static char pmt_bitrate[MAX_SERVICES][32];
    static char pmt_pid_str[MAX_SERVICES][32];
    static char output_addr[128];

    /* Command */
    argv[argc++] = "tsp";
    argv[argc++] = "-v";  /* Verbose for debugging */

    /* Global bitrate */
    snprintf(bitrate_str, sizeof(bitrate_str), "%d", state->output_bitrate);
    argv[argc++] = "-b";
    argv[argc++] = bitrate_str;

    /* Input: null packet generator (provides stuffing packets) */
    argv[argc++] = "-I";
    argv[argc++] = "null";

    /*
     * Add merge plugin for each service
     * Each merge runs a subprocess that:
     * - Reads from UDP source
     * - Filters out PSI tables (PIDs 0-20) since we inject our own
     * - Remaps video/audio PIDs to unique values
     */
    for (int i = 0; i < state->service_count && argc < max_args - 50; i++) {
        mux_service_t *svc = &state->services[i];

        /* Build subprocess command:
         * tsp -I ip addr:port -P filter -n -p 0-20 -s -P remap SRC=DST ...
         */
        snprintf(merge_cmds[i], sizeof(merge_cmds[i]),
                 "tsp -I ip %s:%d "
                 "-P filter -n -p 0-20 -s "
                 "-P remap %d=%d %d=%d",
                 svc->source_address, svc->source_port,
                 svc->source_video_pid, svc->video_pid,
                 svc->source_audio_pid, svc->audio_pid);

        argv[argc++] = "-P";
        argv[argc++] = "merge";
        argv[argc++] = "--no-psi-merge";
        argv[argc++] = merge_cmds[i];
    }

    /*
     * Build and inject PAT (Program Association Table)
     * Maps service_id -> PMT PID for each program
     */
    {
        int pos = snprintf(pat_xml, sizeof(pat_xml),
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<tsduck>"
            "<PAT version=\"0\" transport_stream_id=\"%d\">",
            state->ts_id);

        for (int i = 0; i < state->service_count; i++) {
            pos += snprintf(pat_xml + pos, sizeof(pat_xml) - pos,
                "<service service_id=\"%d\" program_map_PID=\"%d\"/>",
                state->services[i].program_number,
                state->services[i].pmt_pid);
        }
        snprintf(pat_xml + pos, sizeof(pat_xml) - pos, "</PAT></tsduck>");

        snprintf(pat_bitrate, sizeof(pat_bitrate), "%d",
                 state->pat_interval > 0 ? 15000 / state->pat_interval * 1000 : 15000);

        argv[argc++] = "-P";
        argv[argc++] = "inject";
        argv[argc++] = pat_xml;
        argv[argc++] = "--pid";
        argv[argc++] = "0";
        argv[argc++] = "--bitrate";
        argv[argc++] = pat_bitrate;
        argv[argc++] = "--stuffing";
    }

    /*
     * Build and inject SDT (Service Description Table)
     * Contains service name, provider, type for each program
     */
    {
        int pos = snprintf(sdt_xml, sizeof(sdt_xml),
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<tsduck>"
            "<SDT version=\"0\" transport_stream_id=\"%d\" "
            "original_network_id=\"%d\" actual=\"true\">",
            state->ts_id, state->original_network_id);

        for (int i = 0; i < state->service_count; i++) {
            mux_service_t *svc = &state->services[i];
            pos += snprintf(sdt_xml + pos, sizeof(sdt_xml) - pos,
                "<service service_id=\"%d\" running_status=\"running\" "
                "EIT_schedule=\"false\" EIT_present_following=\"false\">"
                "<service_descriptor service_type=\"0x%02X\" "
                "service_provider_name=\"%s\" service_name=\"%s\"/>"
                "</service>",
                svc->program_number,
                svc->service_type,
                svc->service_provider,
                svc->service_name);
        }
        snprintf(sdt_xml + pos, sizeof(sdt_xml) - pos, "</SDT></tsduck>");

        snprintf(sdt_bitrate, sizeof(sdt_bitrate), "%d",
                 state->sdt_interval > 0 ? 15000 / state->sdt_interval * 1000 : 3000);

        argv[argc++] = "-P";
        argv[argc++] = "inject";
        argv[argc++] = sdt_xml;
        argv[argc++] = "--pid";
        argv[argc++] = "17";
        argv[argc++] = "--bitrate";
        argv[argc++] = sdt_bitrate;
        argv[argc++] = "--stuffing";
    }

    /*
     * Build and inject PMT (Program Map Table) for each service
     * Each PMT lists video/audio PIDs for its program
     */
    for (int i = 0; i < state->service_count && argc < max_args - 20; i++) {
        mux_service_t *svc = &state->services[i];

        /* PMT with video (H.264=0x1B) and audio (AAC=0x0F) components
         * PCR_PID is typically the video PID */
        snprintf(pmt_xml[i], sizeof(pmt_xml[i]),
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<tsduck>"
            "<PMT version=\"0\" service_id=\"%d\" PCR_PID=\"%d\">"
            "<component elementary_PID=\"%d\" stream_type=\"0x1B\"/>"
            "<component elementary_PID=\"%d\" stream_type=\"0x0F\"/>"
            "</PMT>"
            "</tsduck>",
            svc->program_number,
            svc->video_pid,
            svc->video_pid,
            svc->audio_pid);

        snprintf(pmt_bitrate[i], sizeof(pmt_bitrate[i]), "%d",
                 state->pmt_interval > 0 ? 15000 / state->pmt_interval * 1000 : 15000);

        snprintf(pmt_pid_str[i], sizeof(pmt_pid_str[i]), "%d", svc->pmt_pid);

        argv[argc++] = "-P";
        argv[argc++] = "inject";
        argv[argc++] = pmt_xml[i];
        argv[argc++] = "--pid";
        argv[argc++] = pmt_pid_str[i];
        argv[argc++] = "--bitrate";
        argv[argc++] = pmt_bitrate[i];
        argv[argc++] = "--stuffing";
    }

    /* Regulate for CBR output */
    argv[argc++] = "-P";
    argv[argc++] = "regulate";

    /* Output to UDP multicast */
    snprintf(output_addr, sizeof(output_addr), "%s:%d",
             state->output_address, state->output_port);
    argv[argc++] = "-O";
    argv[argc++] = "ip";
    argv[argc++] = output_addr;

    /* Null terminate */
    argv[argc] = NULL;

    return argc;
}

/*
 * Print the command that will be executed
 */
static void print_command(char **argv) {
    fprintf(stderr, "Executing: ");
    for (int i = 0; argv[i] != NULL; i++) {
        /* Quote arguments with spaces */
        if (strchr(argv[i], ' ')) {
            fprintf(stderr, "\"%s\" ", argv[i]);
        } else {
            fprintf(stderr, "%s ", argv[i]);
        }
    }
    fprintf(stderr, "\n");
}

/*
 * Execute TSDuck tsp command
 */
static int run_tsp(mux_state_t *state) {
    char *argv[MAX_ARGS];

    int argc = build_tsp_args(state, argv, MAX_ARGS);
    if (argc <= 0) {
        CARI_LOG_ERROR("Failed to build tsp arguments");
        return -1;
    }

    print_command(argv);

    /* Fork and exec */
    pid_t pid = fork();
    if (pid < 0) {
        CARI_LOG_ERROR("Fork failed: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* Child process - execute tsp */
        execvp("tsp", argv);

        /* If exec fails */
        fprintf(stderr, "Failed to execute tsp: %s\n", strerror(errno));
        _exit(127);
    }

    /* Parent process */
    state->tsp_pid = pid;
    CARI_LOG_INFO("Started tsp process with PID %d", pid);

    /* Wait for child to exit */
    int status;
    while (state->running) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            /* Child exited */
            if (WIFEXITED(status)) {
                int exit_code = WEXITSTATUS(status);
                CARI_LOG_INFO("tsp exited with code %d", exit_code);
                return exit_code;
            } else if (WIFSIGNALED(status)) {
                CARI_LOG_INFO("tsp killed by signal %d", WTERMSIG(status));
                return -1;
            }
            break;
        } else if (result < 0 && errno != EINTR) {
            CARI_LOG_ERROR("waitpid error: %s", strerror(errno));
            break;
        }

        /* Sleep briefly to avoid busy waiting */
        usleep(100000);  /* 100ms */
    }

    /* If we're stopping, ensure child is terminated */
    if (!state->running && state->tsp_pid > 0) {
        kill(state->tsp_pid, SIGTERM);
        waitpid(state->tsp_pid, &status, 0);
    }

    state->tsp_pid = 0;
    return 0;
}

static void print_usage(const char *prog) {
    printf("CariTranscoder Mux (TSDuck) - v%s\n", VERSION);
    printf("Multiplexes SPTS streams into MPTS using TSDuck\n\n");
    printf("Usage: %s -c <config_file> [options]\n\n", prog);
    printf("Options:\n");
    printf("  -c, --config FILE    Configuration file (required)\n");
    printf("  -d, --debug          Enable debug logging\n");
    printf("  -t, --test           Test mode - print command without executing\n");
    printf("  -h, --help           Show this help\n");
    printf("\n");
    printf("The configuration file should contain:\n");
    printf("  [muxer]     - id, name\n");
    printf("  [output]    - output_bitrate, address, port\n");
    printf("  [network]   - network_id, network_name, ts_id\n");
    printf("  [tsduck]    - pat_interval, pmt_interval, sdt_interval\n");
    printf("  [services]  - service.N.enabled, source_address, program_number, etc.\n");
}

int main(int argc, char *argv[]) {
    int opt, debug = 0, test_mode = 0;
    const char *config_file = NULL;

    static struct option long_options[] = {
        {"config", required_argument, 0, 'c'},
        {"debug", no_argument, 0, 'd'},
        {"test", no_argument, 0, 't'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "c:dth", long_options, NULL)) != -1) {
        switch (opt) {
            case 'c': config_file = optarg; break;
            case 'd': debug = 1; break;
            case 't': test_mode = 1; break;
            case 'h': print_usage(argv[0]); return 0;
            default: return 1;
        }
    }

    if (!config_file) {
        fprintf(stderr, "Configuration file required (-c)\n");
        print_usage(argv[0]);
        return 1;
    }

    /* Initialize logging */
    log_config_t log_cfg = LOG_CONFIG_DEFAULT;
    strncpy(log_cfg.ident, "cari-mux", sizeof(log_cfg.ident));
    if (debug) log_cfg.min_level = LOG_LEVEL_DEBUG;
    log_init(&log_cfg);

    CARI_LOG_INFO("CariTranscoder Mux (TSDuck) v%s starting...", VERSION);

    /* Load configuration */
    strncpy(g_state.config_file, config_file, sizeof(g_state.config_file) - 1);
    if (load_config(&g_state) != 0) {
        CARI_LOG_ERROR("Failed to load configuration");
        return 1;
    }

    if (g_state.service_count == 0) {
        CARI_LOG_ERROR("No services configured");
        return 1;
    }

    /* Test mode - just print command and exit */
    if (test_mode) {
        char *argv_tsp[MAX_ARGS];
        build_tsp_args(&g_state, argv_tsp, MAX_ARGS);
        print_command(argv_tsp);
        config_free(&g_state.config);
        log_shutdown();
        return 0;
    }

    /* Set up signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_DFL);

    g_state.running = 1;

    /* Run tsp */
    int ret = run_tsp(&g_state);

    /* Cleanup */
    CARI_LOG_INFO("Mux %s shutting down", g_state.id);
    config_free(&g_state.config);
    log_shutdown();

    return ret;
}
