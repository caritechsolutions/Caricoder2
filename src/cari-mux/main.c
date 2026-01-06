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

        if (svc->is_pcr_reference && state->pcr_reference_service == 0) {
            state->pcr_reference_service = svc->program_number;
        }

        if (svc->source_address[0] && svc->source_port > 0) {
            state->service_count++;
            CARI_LOG_INFO("Service %d: %s (%s:%d) -> Program %d",
                     state->service_count, svc->service_name,
                     svc->source_address, svc->source_port, svc->program_number);
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
 */
static int build_tsp_args(mux_state_t *state, char **argv, int max_args) {
    int argc = 0;
    static char bitrate_str[32];
    static char service_args[MAX_SERVICES][16][256];

    /* Command */
    argv[argc++] = "tsp";

    /* Global bitrate */
    snprintf(bitrate_str, sizeof(bitrate_str), "%d", state->output_bitrate);
    argv[argc++] = "-b";
    argv[argc++] = bitrate_str;

    /* First input: null packet generator (sets overall bitrate) */
    argv[argc++] = "-I";
    argv[argc++] = "null";
    argv[argc++] = "--bitrate";
    argv[argc++] = bitrate_str;

    /* Add inputs for each service */
    for (int i = 0; i < state->service_count && argc < max_args - 20; i++) {
        mux_service_t *svc = &state->services[i];

        snprintf(service_args[i][0], sizeof(service_args[i][0]),
                 "%s:%d", svc->source_address, svc->source_port);

        argv[argc++] = "-I";
        argv[argc++] = "ip";
        argv[argc++] = service_args[i][0];
    }

    /* Merge plugin */
    argv[argc++] = "-P";
    argv[argc++] = "merge";

    /* PAT plugin - remap services */
    argv[argc++] = "-P";
    argv[argc++] = "pat";
    for (int i = 0; i < state->service_count && argc < max_args - 10; i++) {
        mux_service_t *svc = &state->services[i];
        snprintf(service_args[i][1], sizeof(service_args[i][1]),
                 "%d=%d", i + 1, svc->program_number);
        argv[argc++] = "--service";
        argv[argc++] = service_args[i][1];
    }

    /* SDT plugin - set service names and types */
    argv[argc++] = "-P";
    argv[argc++] = "sdt";
    argv[argc++] = "--create";
    for (int i = 0; i < state->service_count && argc < max_args - 20; i++) {
        mux_service_t *svc = &state->services[i];

        /* Service name */
        snprintf(service_args[i][2], sizeof(service_args[i][2]),
                 "%d=%s", svc->program_number, svc->service_name);
        argv[argc++] = "--service-name";
        argv[argc++] = service_args[i][2];

        /* Service provider */
        snprintf(service_args[i][3], sizeof(service_args[i][3]),
                 "%d=%s", svc->program_number, svc->service_provider);
        argv[argc++] = "--service-provider";
        argv[argc++] = service_args[i][3];

        /* Service type */
        snprintf(service_args[i][4], sizeof(service_args[i][4]),
                 "%d=%d", svc->program_number, svc->service_type);
        argv[argc++] = "--service-type";
        argv[argc++] = service_args[i][4];
    }

    /* PCR adjust */
    static char pcr_ref_str[32];
    snprintf(pcr_ref_str, sizeof(pcr_ref_str), "%d", state->pcr_reference_service);
    argv[argc++] = "-P";
    argv[argc++] = "pcradjust";
    argv[argc++] = "--reference-service";
    argv[argc++] = pcr_ref_str;

    /* Regulate for CBR output */
    argv[argc++] = "-P";
    argv[argc++] = "regulate";

    /* Output to UDP */
    static char output_addr[128];
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
