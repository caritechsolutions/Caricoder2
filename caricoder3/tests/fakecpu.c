/*
 * caricoder3 tests - pretend the machine has FAKE_CPUS processors
 *
 * Preloaded (LD_PRELOAD) under the program, so that the number of threads
 * the codecs start, and with it the pictures they hold back, can be tested
 * for a 64-processor server on a 2-processor test machine.
 */
#define _GNU_SOURCE
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int sched_getaffinity(pid_t pid, size_t sz, cpu_set_t *set)
{
    int n = getenv("FAKE_CPUS") ? atoi(getenv("FAKE_CPUS")) : 2;
    (void)pid;
    memset(set, 0, sz);
    for (int i = 0; i < n && (size_t)i < sz * 8; i++)
        CPU_SET_S(i, sz, set);
    return 0;
}
