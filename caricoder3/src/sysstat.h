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

#endif
