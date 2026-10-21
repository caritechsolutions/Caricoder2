/*
 * caricoder3 test helper: stand-ins for the NVIDIA driver libraries.
 *
 * The machine the NVIDIA backends were written on has no NVIDIA card. To
 * run that code at all before it meets real hardware, this file builds
 * into three shared libraries with the driver's names (libcuda.so.1,
 * libnvcuvid.so.1, libnvidia-encode.so.1) that do nothing useful with
 * video but hold the caller to the rules of the API as its headers state
 * them:
 *
 *   - every structure carries the right version number
 *   - a CUDA context is current around every call that needs one
 *   - a surface is registered before it is mapped, mapped before it is
 *     encoded from, and everything is undone in reverse before closing
 *   - no more decoded pictures are mapped at once than were asked for
 *   - copies stay inside the memory they were given: "card" memory here is
 *     ordinary memory of exactly the size asked for, so AddressSanitizer
 *     sees any overrun
 *
 * A broken rule prints "NVMOCK VIOLATION" and aborts. What this cannot
 * show is whether the real driver agrees with our reading of its rules:
 * only a real card can. See tests/run_tests.py (xc_nvidia_mock).
 *
 * "Encoded" pictures are a few bytes: a start code, a marker and the
 * picture's size, so that the stand-in decoder can announce the right
 * format when they are played back to it.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../third_party/ffnvcodec/dynlink_cuda.h"
#include "../third_party/ffnvcodec/dynlink_nvcuvid.h"
#include "../third_party/ffnvcodec/nvEncodeAPI.h"

#define VIOLATION(...) do { fprintf(stderr, "NVMOCK VIOLATION: " __VA_ARGS__); \
                            fprintf(stderr, "\n"); abort(); } while (0)
#define CHECK(cond, ...) do { if (!(cond)) VIOLATION(__VA_ARGS__); } while (0)

static _Thread_local int ctx_depth;
static int live_ctx, live_mem;

/* NVMOCK_FAIL=session or NVMOCK_FAIL=decoder makes that step fail the way
 * a real card does when it has no session or no decoder to give;
 * NVMOCK_FAIL=field makes the encoder refuse interlaced (field) encoding
 * when it is initialised, though it claimed the capability.
 * NVMOCK_BAD_AFTER=n: from the n-th packet on, the decoder's parser
 * announces a format the code must refuse (4:2:2), as a real stream might
 * change to in mid-run. */
static int fails(const char *what)
{
    const char *env = getenv("NVMOCK_FAIL");

    return env && strstr(env, what) != NULL;
}

#define NEED_CTX(what) CHECK(ctx_depth > 0, "%s called with no CUDA context current", what)

/* ---- libcuda --------------------------------------------------------- */

CUresult cuInit(unsigned int flags) { (void)flags; return CUDA_SUCCESS; }

CUresult cuDeviceGetCount(int *count) { *count = 2; return CUDA_SUCCESS; }

CUresult cuDeviceGet(CUdevice *dev, int ordinal)
{
    if (ordinal < 0 || ordinal > 1)
        return CUDA_ERROR_UNKNOWN;
    *dev = ordinal;
    return CUDA_SUCCESS;
}

CUresult cuDeviceGetName(char *name, int len, CUdevice dev)
{
    snprintf(name, (size_t)len, "Stand-in card %d (not a real GPU)", dev);
    return CUDA_SUCCESS;
}

/* Contexts that were destroyed. Their addresses are kept (and the memory
 * not given back, so no later context gets the same address): using one
 * again is the fault this is here to catch. */
#define MAX_DEAD 256
static void *dead_ctx[MAX_DEAD];
static int n_dead;

static int is_dead(void *ctx)
{
    for (int i = 0; i < n_dead; i++)
        if (dead_ctx[i] == ctx)
            return 1;
    return 0;
}

CUresult cuCtxCreate_v2(CUcontext *pctx, unsigned int flags, CUdevice dev)
{
    (void)flags; (void)dev;
    *pctx = malloc(16);
    __sync_fetch_and_add(&live_ctx, 1);
    if (getenv("NVMOCK_LOG_CTX"))
        fprintf(stderr, "NVMOCK context created\n");
    ctx_depth++;                            /* a new context is current */
    return CUDA_SUCCESS;
}

CUresult cuCtxPushCurrent_v2(CUcontext ctx)
{
    CHECK(ctx != NULL, "cuCtxPushCurrent with a null context");
    CHECK(!is_dead(ctx), "cuCtxPushCurrent with a context that was destroyed");
    ctx_depth++;
    return CUDA_SUCCESS;
}

CUresult cuCtxPopCurrent_v2(CUcontext *pctx)
{
    CHECK(ctx_depth > 0, "cuCtxPopCurrent with nothing pushed");
    ctx_depth--;
    if (pctx)
        *pctx = NULL;
    return CUDA_SUCCESS;
}

CUresult cuCtxDestroy_v2(CUcontext ctx)
{
    CHECK(!is_dead(ctx), "a context destroyed twice");
    if (n_dead < MAX_DEAD)
        dead_ctx[n_dead++] = ctx;
    else
        free(ctx);
    __sync_fetch_and_sub(&live_ctx, 1);
    return CUDA_SUCCESS;
}

CUresult cuMemAllocPitch_v2(CUdeviceptr *dptr, size_t *pitch, size_t width_bytes,
                            size_t height, unsigned int elem)
{
    NEED_CTX("cuMemAllocPitch");
    CHECK(elem == 4 || elem == 8 || elem == 16, "cuMemAllocPitch element size %u", elem);
    CHECK(width_bytes > 0 && height > 0, "cuMemAllocPitch of nothing");
    *pitch = (width_bytes + 511) & ~(size_t)511;
    *dptr = (CUdeviceptr)(uintptr_t)malloc(*pitch * height);
    __sync_fetch_and_add(&live_mem, 1);
    return *dptr ? CUDA_SUCCESS : CUDA_ERROR_UNKNOWN;
}

CUresult cuMemFree_v2(CUdeviceptr dptr)
{
    NEED_CTX("cuMemFree");
    free((void *)(uintptr_t)dptr);
    __sync_fetch_and_sub(&live_mem, 1);
    return CUDA_SUCCESS;
}

/* ---- the card's own programs: only caricoder3's scaler (src/nv_scale.cu),
 * run here on the CPU by the same code the card runs (src/nv_scale_px.h) */

#include "../src/nv_scale_px.h"

static int live_mod, live_stream;

CUresult cuModuleLoadData(CUmodule *mod, const void *image)
{
    NEED_CTX("cuModuleLoadData");
    CHECK(image && strstr((const char *)image, ".entry cc3_scale"), "a module that is not caricoder3's scaler");
    *mod = (CUmodule)malloc(8);
    __sync_fetch_and_add(&live_mod, 1);
    return CUDA_SUCCESS;
}

CUresult cuModuleGetFunction(CUfunction *fn, CUmodule mod, const char *name)
{
    NEED_CTX("cuModuleGetFunction");
    CHECK(mod != NULL && !strcmp(name, "cc3_scale"), "function %s", name);
    *fn = (CUfunction)mod;
    return CUDA_SUCCESS;
}

CUresult cuModuleUnload(CUmodule mod)
{
    NEED_CTX("cuModuleUnload");
    free(mod);
    __sync_fetch_and_sub(&live_mod, 1);
    return CUDA_SUCCESS;
}

CUresult cuStreamCreate(CUstream *st, unsigned int flags)
{
    (void)flags;
    NEED_CTX("cuStreamCreate");
    *st = (CUstream)malloc(8);
    __sync_fetch_and_add(&live_stream, 1);
    return CUDA_SUCCESS;
}

CUresult cuStreamSynchronize(CUstream st)
{
    NEED_CTX("cuStreamSynchronize");
    CHECK(st != NULL, "synchronising no stream");
    return CUDA_SUCCESS;
}

CUresult cuStreamDestroy_v2(CUstream st)
{
    NEED_CTX("cuStreamDestroy");
    free(st);
    __sync_fetch_and_sub(&live_stream, 1);
    return CUDA_SUCCESS;
}

CUresult cuLaunchKernel(CUfunction f, unsigned int gx, unsigned int gy, unsigned int gz,
                        unsigned int bx, unsigned int by, unsigned int bz, unsigned int shared,
                        CUstream st, void **p, void **extra)
{
    const unsigned char *src;
    unsigned char *dst;
    int spitch, sw, sh, dpitch, dw, dh, comps, wide, blend;

    NEED_CTX("cuLaunchKernel");
    CHECK(f != NULL && p != NULL && extra == NULL && gz == 1 && bz == 1 && shared == 0 && st != NULL,
          "launch arguments");
    src = (const unsigned char *)(uintptr_t)*(CUdeviceptr *)p[0];
    spitch = *(int *)p[1]; sw = *(int *)p[2]; sh = *(int *)p[3];
    dst = (unsigned char *)(uintptr_t)*(CUdeviceptr *)p[4];
    dpitch = *(int *)p[5]; dw = *(int *)p[6]; dh = *(int *)p[7];
    comps = *(int *)p[8]; wide = *(int *)p[9]; blend = *(int *)p[10];
    CHECK(src && dst && sw > 0 && sh > 0 && dw > 0 && dh > 0 && (comps == 1 || comps == 2),
          "scale %dx%d -> %dx%d, %d components", sw, sh, dw, dh, comps);
    CHECK(gx * bx >= (unsigned)dw && gy * by >= (unsigned)dh, "the grid does not cover the picture");
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++)
            cc3_scale_px(src, spitch, sw, sh, dst, dpitch, dw, dh, comps, wide, blend, x, y);
    if (getenv("NVMOCK_LOG_CTX"))
        fprintf(stderr, "NVMOCK scaled %dx%d -> %dx%d\n", sw, sh, dw, dh);
    return CUDA_SUCCESS;
}

CUresult cuMemcpy2D_v2(const CUDA_MEMCPY2D *c)
{
    const uint8_t *src;
    uint8_t *dst;

    NEED_CTX("cuMemcpy2D");
    CHECK(c->srcPitch >= c->WidthInBytes && c->dstPitch >= c->WidthInBytes,
          "cuMemcpy2D: rows of %zu bytes with pitches %zu and %zu", c->WidthInBytes,
          c->srcPitch, c->dstPitch);
    src = c->srcMemoryType == CU_MEMORYTYPE_HOST ? c->srcHost
                                                 : (const uint8_t *)(uintptr_t)c->srcDevice;
    dst = c->dstMemoryType == CU_MEMORYTYPE_HOST ? c->dstHost
                                                 : (uint8_t *)(uintptr_t)c->dstDevice;
    CHECK(src && dst, "cuMemcpy2D with a null pointer");
    src += c->srcY * c->srcPitch + c->srcXInBytes;
    dst += c->dstY * c->dstPitch + c->dstXInBytes;
    for (size_t r = 0; r < c->Height; r++)
        memcpy(dst + r * c->dstPitch, src + r * c->srcPitch, c->WidthInBytes);
    return CUDA_SUCCESS;
}

CUresult cuGetErrorString(CUresult error, const char **pstr)
{
    (void)error;
    *pstr = "stand-in error";
    return CUDA_SUCCESS;
}

/* ---- libnvidia-encode ------------------------------------------------ */

#define MAX_RES 64

struct m_res { void *mem; uint32_t pitch, width, height; NV_ENC_BUFFER_FORMAT fmt; int mapped; };
struct m_bs  { uint8_t data[64]; uint32_t len; uint64_t ts; int idr; int full; };

/* A coded picture waiting to be handed out in decode order. */
struct m_pic {
    uint8_t data[64];
    uint32_t len;
    uint64_t ts;
    int idr;
};

struct m_enc {
    int init, field;
    GUID codec;
    uint32_t w, h, gop;
    NV_ENC_BUFFER_FORMAT fmt;
    uint64_t n;
    int bframes;                            /* frameIntervalP - 1 */
    struct m_pic held[8];                   /* pictures given, not yet coded */
    struct m_bs *held_bs[8];                /* their bitstream buffers, in order given */
    int n_held;
    struct m_res *res[MAX_RES];
    struct m_bs *bs[MAX_RES];
    int n_res, n_bs, pending;
};

static int is_guid(const GUID *a, const GUID *b) { return memcmp(a, b, sizeof(*a)) == 0; }

static NVENCSTATUS NVENCAPI m_open(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS *p, void **enc)
{
    NEED_CTX("nvEncOpenEncodeSessionEx");
    CHECK(p->version == NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER, "session params version");
    CHECK(p->deviceType == NV_ENC_DEVICE_TYPE_CUDA && p->device, "session device");
    CHECK(p->apiVersion == NVENCAPI_VERSION, "session apiVersion");
    if (fails("session"))
        return NV_ENC_ERR_OUT_OF_MEMORY;
    *enc = calloc(1, sizeof(struct m_enc));
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI m_caps(void *enc, GUID codec, NV_ENC_CAPS_PARAM *p, int *val)
{
    (void)enc;
    NEED_CTX("nvEncGetEncodeCaps");
    CHECK(p->version == NV_ENC_CAPS_PARAM_VER, "caps param version");
    switch (p->capsToQuery) {
    case NV_ENC_CAPS_WIDTH_MAX:
    case NV_ENC_CAPS_HEIGHT_MAX: *val = 4096; break;
    case NV_ENC_CAPS_SUPPORT_10BIT_ENCODE: *val = is_guid(&codec, &NV_ENC_CODEC_HEVC_GUID); break;
    case NV_ENC_CAPS_SUPPORT_FIELD_ENCODING: *val = is_guid(&codec, &NV_ENC_CODEC_H264_GUID); break;
    /* Like a Pascal card: B-frames for H.264 only. */
    case NV_ENC_CAPS_NUM_MAX_BFRAMES: *val = is_guid(&codec, &NV_ENC_CODEC_H264_GUID) ? 4 : 0; break;
    default: *val = 0; break;
    }
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI m_preset(void *enc, GUID codec, GUID preset, NV_ENC_TUNING_INFO t,
                                     NV_ENC_PRESET_CONFIG *pc)
{
    (void)enc; (void)codec; (void)preset;
    NEED_CTX("nvEncGetEncodePresetConfigEx");
    CHECK(pc->version == NV_ENC_PRESET_CONFIG_VER, "preset config version");
    CHECK(pc->presetCfg.version == NV_ENC_CONFIG_VER, "preset config: inner config version");
    CHECK(t != NV_ENC_TUNING_INFO_UNDEFINED, "tuning info not set");
    pc->presetCfg.gopLength = 250;
    pc->presetCfg.frameIntervalP = 3;       /* the caller is expected to overrule this */
    pc->presetCfg.rcParams.enableLookahead = 1;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI m_init(void *enc, NV_ENC_INITIALIZE_PARAMS *p)
{
    struct m_enc *e = enc;

    NEED_CTX("nvEncInitializeEncoder");
    /* NVMOCK_SLOW_INIT=ms: an encoder takes this long to open, as a real card's
     * does with many sessions starting at once */
    if (getenv("NVMOCK_SLOW_INIT"))
        usleep((useconds_t)atoi(getenv("NVMOCK_SLOW_INIT")) * 1000);
    CHECK(p->version == NV_ENC_INITIALIZE_PARAMS_VER, "initialize params version");
    CHECK(p->encodeConfig && p->encodeConfig->version == NV_ENC_CONFIG_VER, "encode config version");
    CHECK(p->encodeWidth >= 16 && p->encodeHeight >= 16 && !(p->encodeWidth & 1) &&
          !(p->encodeHeight & 1), "picture size %ux%u", p->encodeWidth, p->encodeHeight);
    CHECK(p->frameRateNum && p->frameRateDen, "frame rate %u/%u", p->frameRateNum, p->frameRateDen);
    CHECK(p->darWidth && p->darHeight, "display aspect ratio %u:%u", p->darWidth, p->darHeight);
    CHECK(p->enablePTD == 1 && p->enableEncodeAsync == 0, "expected PTD on, async off");
    CHECK(p->tuningInfo != NV_ENC_TUNING_INFO_UNDEFINED, "tuning info not set");
    CHECK(p->encodeConfig->frameIntervalP >= 1 && p->encodeConfig->frameIntervalP <= 5,
          "frameIntervalP %d", (int)p->encodeConfig->frameIntervalP);
    CHECK(p->encodeConfig->frameIntervalP == 1 || is_guid(&p->encodeGUID, &NV_ENC_CODEC_H264_GUID),
          "B-frames asked for where this card has none");
    e->bframes = (int)p->encodeConfig->frameIntervalP - 1;
    CHECK(p->encodeConfig->rcParams.enableLookahead == 0, "this stand-in has no look-ahead");
    CHECK(p->encodeConfig->rcParams.averageBitRate > 0 &&
          p->encodeConfig->rcParams.vbvBufferSize > 0, "rate control not set");
    CHECK(p->encodeConfig->gopLength > 0, "gop length 0");
    if (p->encodeConfig->frameFieldMode == NV_ENC_PARAMS_FRAME_FIELD_MODE_FIELD) {
        CHECK(is_guid(&p->encodeGUID, &NV_ENC_CODEC_H264_GUID), "field mode outside H.264");
        if (fails("field"))
            return NV_ENC_ERR_UNSUPPORTED_PARAM;
    }
    e->field = p->encodeConfig->frameFieldMode == NV_ENC_PARAMS_FRAME_FIELD_MODE_FIELD;
    if (is_guid(&p->encodeGUID, &NV_ENC_CODEC_H264_GUID)) {
        CHECK(p->encodeConfig->encodeCodecConfig.h264Config.repeatSPSPPS &&
              p->encodeConfig->encodeCodecConfig.h264Config.outputAUD, "H.264 for TS needs "
              "repeated parameter sets and access unit delimiters");
        e->fmt = NV_ENC_BUFFER_FORMAT_NV12;
    } else {
        const NV_ENC_CONFIG_HEVC *h = &p->encodeConfig->encodeCodecConfig.hevcConfig;

        CHECK(is_guid(&p->encodeGUID, &NV_ENC_CODEC_HEVC_GUID), "unknown codec");
        CHECK(h->repeatSPSPPS && h->outputAUD, "H.265 for TS needs repeated parameter sets "
              "and access unit delimiters");
        e->fmt = h->pixelBitDepthMinus8 ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT
                                        : NV_ENC_BUFFER_FORMAT_NV12;
        CHECK(!h->pixelBitDepthMinus8 ||
              is_guid(&p->encodeConfig->profileGUID, &NV_ENC_HEVC_PROFILE_MAIN10_GUID),
              "10-bit H.265 without the Main10 profile");
    }
    e->codec = p->encodeGUID;
    e->w = p->encodeWidth;
    e->h = p->encodeHeight;
    e->gop = p->encodeConfig->gopLength;
    e->init = 1;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI m_register(void *enc, NV_ENC_REGISTER_RESOURCE *r)
{
    struct m_enc *e = enc;
    struct m_res *x;
    uint32_t bps;

    NEED_CTX("nvEncRegisterResource");
    CHECK(e->init, "register before initialize");
    CHECK(r->version == NV_ENC_REGISTER_RESOURCE_VER, "register resource version");
    CHECK(r->resourceType == NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR, "resource type");
    CHECK(r->bufferFormat == e->fmt, "resource format does not match the encoder's");
    CHECK(r->bufferUsage == NV_ENC_INPUT_IMAGE, "resource usage");
    bps = e->fmt == NV_ENC_BUFFER_FORMAT_NV12 ? 1 : 2;
    CHECK(r->width == e->w && r->height == e->h && r->pitch >= r->width * bps &&
          r->resourceToRegister, "resource %ux%u pitch %u", r->width, r->height, r->pitch);
    CHECK(e->n_res < MAX_RES, "too many resources");
    x = calloc(1, sizeof(*x));
    x->mem = r->resourceToRegister;
    x->pitch = r->pitch;
    x->width = r->width;
    x->height = r->height;
    x->fmt = r->bufferFormat;
    e->res[e->n_res++] = x;
    r->registeredResource = x;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI m_unregister(void *enc, NV_ENC_REGISTERED_PTR reg)
{
    struct m_enc *e = enc;
    struct m_res *x = reg;

    NEED_CTX("nvEncUnregisterResource");
    CHECK(!x->mapped, "unregistering a resource that is still mapped");
    for (int i = 0; i < e->n_res; i++)
        if (e->res[i] == x) {
            e->res[i] = e->res[--e->n_res];
            free(x);
            return NV_ENC_SUCCESS;
        }
    VIOLATION("unregistering something that was not registered");
}

static NVENCSTATUS NVENCAPI m_map(void *enc, NV_ENC_MAP_INPUT_RESOURCE *m)
{
    struct m_res *x = m->registeredResource;

    (void)enc;
    NEED_CTX("nvEncMapInputResource");
    CHECK(m->version == NV_ENC_MAP_INPUT_RESOURCE_VER, "map input resource version");
    CHECK(x && !x->mapped, "mapping a resource twice");
    x->mapped = 1;
    m->mappedResource = x;
    m->mappedBufferFmt = x->fmt;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI m_unmap(void *enc, NV_ENC_INPUT_PTR in)
{
    struct m_res *x = in;

    (void)enc;
    NEED_CTX("nvEncUnmapInputResource");
    CHECK(x && x->mapped, "unmapping a resource that is not mapped");
    x->mapped = 0;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI m_create_bs(void *enc, NV_ENC_CREATE_BITSTREAM_BUFFER *b)
{
    struct m_enc *e = enc;

    NEED_CTX("nvEncCreateBitstreamBuffer");
    CHECK(b->version == NV_ENC_CREATE_BITSTREAM_BUFFER_VER, "bitstream buffer version");
    CHECK(e->init && e->n_bs < MAX_RES, "bitstream buffer before initialize, or too many");
    e->bs[e->n_bs] = calloc(1, sizeof(struct m_bs));
    b->bitstreamBuffer = e->bs[e->n_bs++];
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI m_destroy_bs(void *enc, NV_ENC_OUTPUT_PTR bs)
{
    struct m_enc *e = enc;
    struct m_bs *x = bs;

    NEED_CTX("nvEncDestroyBitstreamBuffer");
    /* The real driver crashed (Tesla P40, 535) when torn down with
     * pictures still held back: insist that they were brought out. */
    CHECK(e->n_held == 0, "destroying a bitstream buffer while the encoder still holds "
          "%d picture(s): send EOS and collect them first", e->n_held);
    CHECK(x && !x->full, "destroying a bitstream buffer whose output was not collected");
    for (int i = 0; i < e->n_bs; i++)
        if (e->bs[i] == bs) {
            e->bs[i] = e->bs[--e->n_bs];
            free(bs);
            return NV_ENC_SUCCESS;
        }
    VIOLATION("destroying a bitstream buffer that does not exist");
}

/* Codes what is held: the last picture as the reference, first, then the
 * B pictures before it in the order given. Each goes into the bitstream
 * buffer that came with the k-th picture, as the real encoder does. */
static void put_pic(struct m_bs *b, const struct m_pic *pic)
{
    b->idr = pic->idr;
    b->ts = pic->ts;
    b->len = pic->len;
    memcpy(b->data, pic->data, pic->len);
    b->full = 1;
}

static void emit_held(struct m_enc *e)
{
    int n = e->n_held;

    if (n == 0)
        return;
    put_pic(e->held_bs[0], &e->held[n - 1]);
    for (int i = 0; i < n - 1; i++)
        put_pic(e->held_bs[i + 1], &e->held[i]);
    e->n_held = 0;
}

static NVENCSTATUS NVENCAPI m_encode(void *enc, NV_ENC_PIC_PARAMS *p)
{
    struct m_enc *e = enc;
    struct m_res *x = p->inputBuffer;
    struct m_bs *b = p->outputBitstream;
    struct m_pic pic;
    uint32_t bps, sum = 0;
    const uint8_t *mem;

    NEED_CTX("nvEncEncodePicture");
    CHECK(p->version == NV_ENC_PIC_PARAMS_VER, "pic params version");
    if (p->encodePicFlags & NV_ENC_PIC_FLAG_EOS) {
        emit_held(e);
        return NV_ENC_SUCCESS;
    }
    CHECK(x && x->mapped, "encoding from a surface that is not mapped");
    CHECK(b && !b->full, "encoding into a bitstream buffer whose last output was not collected");
    CHECK(p->inputWidth == e->w && p->inputHeight == e->h && p->inputPitch == x->pitch,
          "pic params %ux%u pitch %u", p->inputWidth, p->inputHeight, p->inputPitch);
    CHECK(p->bufferFmt == e->fmt, "pic params buffer format");
    CHECK(e->field ? (p->pictureStruct == NV_ENC_PIC_STRUCT_FIELD_TOP_BOTTOM ||
                      p->pictureStruct == NV_ENC_PIC_STRUCT_FIELD_BOTTOM_TOP)
                   : p->pictureStruct == NV_ENC_PIC_STRUCT_FRAME,
          "picture structure %d does not go with the encoder's %s mode", (int)p->pictureStruct,
          e->field ? "field" : "frame");
    /* Read every sample the encoder would read. */
    bps = e->fmt == NV_ENC_BUFFER_FORMAT_NV12 ? 1 : 2;
    mem = x->mem;
    for (uint32_t r = 0; r < e->h * 3 / 2; r++)
        for (uint32_t i = 0; i < e->w * bps; i += 97)
            sum += mem[(size_t)r * x->pitch + i];

    memset(&pic, 0, sizeof(pic));
    pic.idr = (e->n % e->gop) == 0 || (p->encodePicFlags & NV_ENC_PIC_FLAG_FORCEIDR) != 0;
    if (pic.idr)
        e->n = 0;                           /* a forced key frame restarts the count */
    pic.ts = p->inputTimeStamp;
    memcpy(pic.data, "\0\0\0\1\x09NVMK", 9);
    pic.data[9] = (uint8_t)(e->w >> 8);
    pic.data[10] = (uint8_t)e->w;
    pic.data[11] = (uint8_t)(e->h >> 8);
    pic.data[12] = (uint8_t)e->h;
    pic.data[13] = (uint8_t)(bps == 2 ? 10 : 8);
    pic.data[14] = (uint8_t)pic.idr;
    pic.data[15] = is_guid(&e->codec, &NV_ENC_CODEC_HEVC_GUID);
    memcpy(pic.data + 16, &sum, 4);
    pic.len = 20 + (pic.idr ? 30 : 4);
    e->n++;

    if (e->bframes == 0) {
        put_pic(b, &pic);
        return NV_ENC_SUCCESS;
    }
    /* An IDR picture is a group of its own: what is held is coded first,
     * with its last picture as the reference. */
    if (pic.idr) {
        emit_held(e);
        put_pic(b, &pic);
        return NV_ENC_SUCCESS;
    }
    e->held[e->n_held] = pic;
    e->held_bs[e->n_held++] = b;
    if (e->n_held == e->bframes + 1) {
        emit_held(e);
        return NV_ENC_SUCCESS;
    }
    return NV_ENC_ERR_NEED_MORE_INPUT;
}

static NVENCSTATUS NVENCAPI m_lock(void *enc, NV_ENC_LOCK_BITSTREAM *l)
{
    struct m_bs *b = l->outputBitstream;

    (void)enc;
    NEED_CTX("nvEncLockBitstream");
    CHECK(l->version == NV_ENC_LOCK_BITSTREAM_VER, "lock bitstream version");
    CHECK(b && b->full, "locking a bitstream buffer with nothing in it");
    l->bitstreamBufferPtr = b->data;
    l->bitstreamSizeInBytes = b->len;
    l->outputTimeStamp = b->ts;
    l->pictureType = b->idr ? NV_ENC_PIC_TYPE_IDR : NV_ENC_PIC_TYPE_P;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI m_unlock(void *enc, NV_ENC_OUTPUT_PTR bs)
{
    struct m_bs *b = bs;

    (void)enc;
    NEED_CTX("nvEncUnlockBitstream");
    CHECK(b && b->full, "unlocking a bitstream buffer that was not locked");
    b->full = 0;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS NVENCAPI m_destroy(void *enc)
{
    struct m_enc *e = enc;

    NEED_CTX("nvEncDestroyEncoder");
    CHECK(e->n_held == 0, "encoder destroyed while it still holds %d picture(s)", e->n_held);
    CHECK(e->n_res == 0, "encoder destroyed with %d resources still registered", e->n_res);
    CHECK(e->n_bs == 0, "encoder destroyed with %d bitstream buffers left", e->n_bs);
    free(e);
    return NV_ENC_SUCCESS;
}

static const char *NVENCAPI m_last_error(void *enc) { (void)enc; return ""; }

NVENCSTATUS NVENCAPI NvEncodeAPIGetMaxSupportedVersion(uint32_t *version)
{
    *version = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    return NV_ENC_SUCCESS;
}

NVENCSTATUS NVENCAPI NvEncodeAPICreateInstance(NV_ENCODE_API_FUNCTION_LIST *f)
{
    CHECK(f->version == NV_ENCODE_API_FUNCTION_LIST_VER, "function list version");
    f->nvEncOpenEncodeSessionEx = m_open;
    f->nvEncGetEncodeCaps = m_caps;
    f->nvEncGetEncodePresetConfigEx = m_preset;
    f->nvEncInitializeEncoder = m_init;
    f->nvEncRegisterResource = m_register;
    f->nvEncUnregisterResource = m_unregister;
    f->nvEncMapInputResource = m_map;
    f->nvEncUnmapInputResource = m_unmap;
    f->nvEncCreateBitstreamBuffer = m_create_bs;
    f->nvEncDestroyBitstreamBuffer = m_destroy_bs;
    f->nvEncEncodePicture = m_encode;
    f->nvEncLockBitstream = m_lock;
    f->nvEncUnlockBitstream = m_unlock;
    f->nvEncDestroyEncoder = m_destroy;
    f->nvEncGetLastErrorString = m_last_error;
    return NV_ENC_SUCCESS;
}

/* ---- libnvcuvid ------------------------------------------------------ */

#define DELAY 3                             /* pictures held back for reordering */

struct m_dec {
    CUVIDDECODECREATEINFO ci;
    int mapped;
};

struct m_parser {
    CUVIDPARSERPARAMS pp;
    int started, surfaces, next_idx, packets;
    unsigned w, h, depth;
    CUVIDPARSERDISPINFO held[DELAY];
    int n_held;
};

static int live_decoders;                   /* a card has several decoders at once */

CUresult cuvidCreateVideoParser(CUvideoparser *obj, CUVIDPARSERPARAMS *pp)
{
    struct m_parser *p;

    NEED_CTX("cuvidCreateVideoParser");
    CHECK(pp->pfnSequenceCallback && pp->pfnDecodePicture && pp->pfnDisplayPicture,
          "parser without its callbacks");
    CHECK(pp->CodecType == cudaVideoCodec_H264 || pp->CodecType == cudaVideoCodec_HEVC, "codec");
    CHECK(pp->ulMaxNumDecodeSurfaces >= 1, "ulMaxNumDecodeSurfaces 0");
    p = calloc(1, sizeof(*p));
    p->pp = *pp;
    *obj = (CUvideoparser)p;
    return CUDA_SUCCESS;
}

CUresult cuvidDestroyVideoParser(CUvideoparser obj)
{
    NEED_CTX("cuvidDestroyVideoParser");
    free(obj);
    return CUDA_SUCCESS;
}

static void show(struct m_parser *p, const CUVIDPARSERDISPINFO *di)
{
    CUVIDPARSERDISPINFO copy = *di;

    p->pp.pfnDisplayPicture(p->pp.pUserData, &copy);
}

CUresult cuvidParseVideoData(CUvideoparser obj, CUVIDSOURCEDATAPACKET *pkt)
{
    struct m_parser *p = (struct m_parser *)obj;
    const unsigned char *d = pkt->payload;
    unsigned w = 320, h = 240, depth = 8;
    char il = 'p';                          /* NVMOCK_FORMAT=WxH:depth:i = interlaced */
    int bad = 0;
    const char *env = getenv("NVMOCK_FORMAT");
    CUVIDPARSERDISPINFO di;
    CUVIDPICPARAMS pic;

    NEED_CTX("cuvidParseVideoData");
    if (pkt->flags & CUVID_PKT_ENDOFSTREAM) {
        while (p->n_held > 0) {
            int first = 0;

            for (int i = 1; i < p->n_held; i++)
                if (p->held[i].timestamp < p->held[first].timestamp)
                    first = i;
            show(p, &p->held[first]);
            p->held[first] = p->held[--p->n_held];
        }
        return CUDA_SUCCESS;
    }
    CHECK(d && pkt->payload_size > 0, "a packet with no data");
    /* touch all of it, as a parser would */
    {
        volatile unsigned sum = 0;

        for (unsigned long i = 0; i < pkt->payload_size; i++)
            sum += d[i];
    }
    if (env)
        sscanf(env, "%ux%u:%u:%c", &w, &h, &depth, &il);
    if (pkt->payload_size >= 20 && !memcmp(d + 5, "NVMK", 4)) {
        w = (unsigned)d[9] << 8 | d[10];
        h = (unsigned)d[11] << 8 | d[12];
        depth = d[13];
    }
    if (getenv("NVMOCK_BAD_AFTER") && ++p->packets >= atoi(getenv("NVMOCK_BAD_AFTER"))) {
        bad = 1;
        p->started = 0;
    }
    if (!p->started || w != p->w || h != p->h || depth != p->depth) {
        CUVIDEOFORMAT f;
        int n;

        memset(&f, 0, sizeof(f));
        f.codec = p->pp.CodecType;
        f.frame_rate.numerator = 25;
        f.frame_rate.denominator = 1;
        f.progressive_sequence = il != 'i';
        f.bit_depth_luma_minus8 = f.bit_depth_chroma_minus8 = (unsigned char)(depth - 8);
        f.min_num_decode_surfaces = 6;
        f.coded_width = (w + 15) & ~15u;
        f.coded_height = (h + 15) & ~15u;
        f.display_area.right = (int)w;
        f.display_area.bottom = (int)h;
        f.chroma_format = bad ? cudaVideoChromaFormat_422 : cudaVideoChromaFormat_420;
        f.display_aspect_ratio.x = (int)w;
        f.display_aspect_ratio.y = (int)h;
        f.video_signal_description.color_primaries = 1;
        f.video_signal_description.transfer_characteristics = 1;
        f.video_signal_description.matrix_coefficients = 1;
        n = p->pp.pfnSequenceCallback(p->pp.pUserData, &f);
        if (n <= 0)
            return CUDA_ERROR_UNKNOWN;
        p->surfaces = n > 1 ? n : (int)p->pp.ulMaxNumDecodeSurfaces;
        p->started = 1;
        p->w = w;
        p->h = h;
        p->depth = depth;
        p->n_held = 0;
    }
    memset(&pic, 0, sizeof(pic));
    pic.CurrPicIdx = p->next_idx;
    if (!p->pp.pfnDecodePicture(p->pp.pUserData, &pic))
        return CUDA_ERROR_UNKNOWN;

    memset(&di, 0, sizeof(di));
    di.picture_index = p->next_idx;
    di.progressive_frame = il != 'i';
    di.top_field_first = 1;
    di.timestamp = (pkt->flags & CUVID_PKT_TIMESTAMP) ? pkt->timestamp : 0;
    p->next_idx = (p->next_idx + 1) % p->surfaces;
    /* Pictures arrive in decoding order and are shown in order of time:
     * of those held back, the earliest goes first. */
    if (p->n_held == DELAY) {
        int first = 0;

        for (int i = 1; i < p->n_held; i++)
            if (p->held[i].timestamp < p->held[first].timestamp)
                first = i;
        show(p, &p->held[first]);
        p->held[first] = p->held[--p->n_held];
    }
    p->held[p->n_held++] = di;
    return CUDA_SUCCESS;
}

CUresult cuvidCreateDecoder(CUvideodecoder *dec, CUVIDDECODECREATEINFO *ci)
{
    struct m_dec *d;

    NEED_CTX("cuvidCreateDecoder");
    if (fails("decoder"))
        return CUDA_ERROR_UNKNOWN;
    if (getenv("NVMOCK_LOG_CTX"))
        fprintf(stderr, "NVMOCK decoder created %ux%u\n", (unsigned)ci->ulTargetWidth, (unsigned)ci->ulTargetHeight);
    CHECK(live_decoders < 32, "%d decoders at once: something is not closing them", live_decoders);
    CHECK(ci->ulWidth && ci->ulHeight && ci->ulTargetWidth && ci->ulTargetHeight &&
          !(ci->ulTargetWidth & 1) && !(ci->ulTargetHeight & 1), "decoder size");
    CHECK(ci->ulNumDecodeSurfaces >= 6, "fewer decode surfaces than the stream needs");
    CHECK(ci->ulNumOutputSurfaces >= 1, "no output surfaces");
    CHECK(ci->ChromaFormat == cudaVideoChromaFormat_420, "chroma format");
    CHECK(ci->OutputFormat == cudaVideoSurfaceFormat_NV12 ||
          (ci->OutputFormat == cudaVideoSurfaceFormat_P016 && ci->bitDepthMinus8 > 0),
          "16-bit output asked for 8-bit video");
    d = calloc(1, sizeof(*d));
    d->ci = *ci;
    live_decoders++;
    *dec = (CUvideodecoder)d;
    return CUDA_SUCCESS;
}

CUresult cuvidDestroyDecoder(CUvideodecoder dec)
{
    struct m_dec *d = (struct m_dec *)dec;

    NEED_CTX("cuvidDestroyDecoder");
    CHECK(d->mapped == 0, "decoder destroyed with %d pictures still mapped", d->mapped);
    live_decoders--;
    free(d);
    return CUDA_SUCCESS;
}

CUresult cuvidDecodePicture(CUvideodecoder dec, CUVIDPICPARAMS *pp)
{
    struct m_dec *d = (struct m_dec *)dec;

    NEED_CTX("cuvidDecodePicture");
    CHECK(pp->CurrPicIdx >= 0 && (tcu_ulong)pp->CurrPicIdx < d->ci.ulNumDecodeSurfaces,
          "picture index %d of %lu", pp->CurrPicIdx, (unsigned long)d->ci.ulNumDecodeSurfaces);
    return CUDA_SUCCESS;
}

CUresult cuvidMapVideoFrame64(CUvideodecoder dec, int idx, unsigned long long *ptr,
                              unsigned int *pitch, CUVIDPROCPARAMS *vpp)
{
    struct m_dec *d = (struct m_dec *)dec;
    size_t bps = d->ci.OutputFormat == cudaVideoSurfaceFormat_P016 ? 2 : 1;
    size_t p = (d->ci.ulTargetWidth * bps + 255) & ~(size_t)255;
    size_t rows = d->ci.ulTargetHeight * 3 / 2;
    uint8_t *mem;

    NEED_CTX("cuvidMapVideoFrame");
    CHECK(vpp != NULL, "map without processing parameters");
    CHECK(idx >= 0 && (tcu_ulong)idx < d->ci.ulNumDecodeSurfaces, "map of picture %d", idx);
    CHECK((tcu_ulong)d->mapped < d->ci.ulNumOutputSurfaces,
          "more pictures mapped at once (%d) than output surfaces asked for (%lu)",
          d->mapped + 1, (unsigned long)d->ci.ulNumOutputSurfaces);
    mem = malloc(p * rows);                 /* exactly the surface, no more */
    memset(mem, 0x40 + idx, p * rows);
    d->mapped++;
    *ptr = (unsigned long long)(uintptr_t)mem;
    *pitch = (unsigned int)p;
    return CUDA_SUCCESS;
}

CUresult cuvidUnmapVideoFrame64(CUvideodecoder dec, unsigned long long ptr)
{
    struct m_dec *d = (struct m_dec *)dec;

    NEED_CTX("cuvidUnmapVideoFrame");
    CHECK(d->mapped > 0 && ptr, "unmap of a picture that is not mapped");
    free((void *)(uintptr_t)ptr);
    d->mapped--;
    return CUDA_SUCCESS;
}

/* At unload: nothing of ours may be left behind by the caller. */
__attribute__((destructor)) static void at_exit(void)
{
    if (getenv("NVMOCK_CHECK_LEAKS") && (live_mod || live_stream))
        VIOLATION("left behind: %d modules, %d streams", live_mod, live_stream);
    if (getenv("NVMOCK_CHECK_LEAKS") && (live_ctx || live_mem || live_decoders))
        VIOLATION("%d contexts, %d card allocations and %d decoders were never released",
                  live_ctx, live_mem, live_decoders);
}

/* Everything else the loader insists on finding, never called by us. */
#include "nvmock_stubs.h"
