/*
 * caricoder3 - NVIDIA: libraries, cards, contexts
 * Copyright (c) 2026 CariTech Solutions
 */
#ifdef HAVE_NVIDIA

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "nv_common.h"

#define MAX_GPU 64

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_tried, g_ok;
static char g_why[200];
static CudaFunctions *g_cu;
static CuvidFunctions *g_cv;
static NvencFunctions *g_ne;
static int g_count;

static struct {
    CUcontext ctx;
    int refs;
} g_dev[MAX_GPU];

const char *nv_cu_err(CUresult r)
{
    const char *s = NULL;

    if (g_cu && g_cu->cuGetErrorString && g_cu->cuGetErrorString(r, &s) == CUDA_SUCCESS && s)
        return s;
    return "unknown CUDA error";
}

static void load_locked(void)
{
    CUresult r;

    g_tried = true;
    if (cuda_load_functions(&g_cu, NULL) < 0 || !g_cu) {
        snprintf(g_why, sizeof(g_why), "libcuda.so.1 not found (no NVIDIA driver?)");
        return;
    }
    if (cuvid_load_functions(&g_cv, NULL) < 0 || !g_cv) {
        snprintf(g_why, sizeof(g_why), "libnvcuvid.so.1 not found");
        return;
    }
    if (nvenc_load_functions(&g_ne, NULL) < 0 || !g_ne) {
        snprintf(g_why, sizeof(g_why), "libnvidia-encode.so.1 not found");
        return;
    }
    r = g_cu->cuInit(0);
    if (r != CUDA_SUCCESS) {
        snprintf(g_why, sizeof(g_why), "CUDA would not start: %s", nv_cu_err(r));
        return;
    }
    r = g_cu->cuDeviceGetCount(&g_count);
    if (r != CUDA_SUCCESS || g_count <= 0) {
        snprintf(g_why, sizeof(g_why), "no NVIDIA card found");
        return;
    }
    if (g_count > MAX_GPU)
        g_count = MAX_GPU;
    g_ok = true;
}

bool nv_load(char *why, size_t len)
{
    bool ok;

    pthread_mutex_lock(&g_lock);
    if (!g_tried)
        load_locked();
    ok = g_ok;
    if (!ok && why && len)
        snprintf(why, len, "%s", g_why);
    pthread_mutex_unlock(&g_lock);
    return ok;
}

CudaFunctions *nv_cu(void)  { return g_cu; }
CuvidFunctions *nv_cv(void) { return g_cv; }
NvencFunctions *nv_ne(void) { return g_ne; }

int nv_device_count(void)
{
    return g_ok ? g_count : 0;
}

bool nv_probe(char *why, size_t len)
{
    return nv_load(why, len);
}

bool nv_device_name(int gpu, char *name, size_t len, size_t *mem_bytes)
{
    CUdevice dev;

    if (!nv_load(NULL, 0) || gpu < 0 || gpu >= g_count)
        return false;
    if (g_cu->cuDeviceGet(&dev, gpu) != CUDA_SUCCESS ||
        g_cu->cuDeviceGetName(name, (int)len, dev) != CUDA_SUCCESS)
        return false;
    if (mem_bytes)
        *mem_bytes = 0;                     /* not offered by this API subset */
    return true;
}

CUcontext nv_ctx_get(int gpu, char *why, size_t len)
{
    CUcontext ctx = NULL, dummy;
    CUdevice dev;
    CUresult r;

    if (!nv_load(why, len))
        return NULL;
    if (gpu < 0 || gpu >= g_count) {
        snprintf(why, len, "there is no card %d (this machine has %d)", gpu, g_count);
        return NULL;
    }
    pthread_mutex_lock(&g_lock);
    if (g_dev[gpu].refs == 0) {
        r = g_cu->cuDeviceGet(&dev, gpu);
        if (r == CUDA_SUCCESS)
            /* Blocking sync: a thread waiting on the card sleeps instead
             * of spinning a CPU core. */
            r = g_cu->cuCtxCreate(&g_dev[gpu].ctx, CU_CTX_SCHED_BLOCKING_SYNC, dev);
        if (r != CUDA_SUCCESS) {
            snprintf(why, len, "card %d: no CUDA context: %s", gpu, nv_cu_err(r));
            pthread_mutex_unlock(&g_lock);
            return NULL;
        }
        /* Creating a context also makes it current. Callers push and pop. */
        g_cu->cuCtxPopCurrent(&dummy);
    }
    g_dev[gpu].refs++;
    ctx = g_dev[gpu].ctx;
    pthread_mutex_unlock(&g_lock);
    return ctx;
}

void nv_ctx_put(CUcontext ctx)
{
    if (!ctx)
        return;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_GPU; i++) {
        if (g_dev[i].refs > 0 && g_dev[i].ctx == ctx) {
            if (--g_dev[i].refs == 0) {
                g_cu->cuCtxDestroy(ctx);
                g_dev[i].ctx = NULL;
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

#endif /* HAVE_NVIDIA */
