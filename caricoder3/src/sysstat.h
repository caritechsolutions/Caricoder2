/*
 * caricoder3 - what the machine is doing: CPU, memory, GPUs, interfaces
 * Copyright (c) 2026 CariTech Solutions
 *
 * A thread samples /proc every two seconds (and nvidia-smi every five,
 * when there is one); sysstat_json() writes the latest sample with the
 * rates since the one before.
 */
#ifndef CC3_SYSSTAT_H
#define CC3_SYSSTAT_H

#include <stdio.h>

void sysstat_start(void);
void sysstat_stop(void);
/* {"cpu": {...}, "memory": {...}, "gpus": [...], "interfaces": [...]} */
void sysstat_json(FILE *f);
/* The latest figures one by one, for the record: cpu.percent, load.1,
 * mem.used_pct, gpuN.util/encoder/decoder/mem_mb/temp_c/power_w,
 * net.IF.rx_mbps/tx_mbps, and net.IF.rx_drop/rx_err as increments since
 * the previous call. Nothing before the first sample. */
double sysstat_cpu_percent(void);        /* -1: not sampled yet */
/* The cards as last seen by nvidia-smi: index, memory free (MB, -1
 * unknown) and how busy (%); returns how many. */
int sysstat_gpu_free(int *index, int *free_mb, int *busy_pct, int max);
void sysstat_metrics(void (*cb)(const char *name, double v, void *op), void *op);

#endif
