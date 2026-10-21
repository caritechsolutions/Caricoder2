/*
 * caricoder3 - H.264 / H.265 encode on an NVIDIA card (NVENC)
 * Copyright (c) 2026 CariTech Solutions
 *
 * Pictures arrive either already on the card (from NVDEC, same CUDA
 * context) or in ordinary memory (from the CPU decoder). Either way they
 * are copied into one of a small pool of surfaces the encoder knows
 * about: card to card for the first kind, which never touches the CPU,
 * and an upload for the second.
 *
 * B-frames: H.264 is coded with two B-frames between references
 * (frameIntervalP 3) when the card offers them, which takes about a tenth
 * off the bit rate for the same quality. The card then holds pictures
 * back: a picture is given, the call says "need more input", and once a
 * reference has been coded the whole group comes out at once, in decode
 * order, each in the bitstream buffer that was handed in with the k-th
 * picture, not the one of its own picture. The decode time of each is
 * worked out here (the presentation time of the picture given two
 * earlier: the usual rule for a delay of two). H.265 has no B-frames on
 * the Tesla P40 (Pascal) and is coded without. No look-ahead.
 *
 * First run on real hardware (Tesla P40, driver 535) on 2026-10-14 for
 * the part without B-frames; `caricoder3 --gpu-test` exercises it.
 */
#ifdef HAVE_NVIDIA

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec.h"
#include "log.h"
#include "nv_common.h"

#define POOL 8
#define BFRAMES_H264 2                  /* between references; 0 turns them off */
#define PTS_RING 64                     /* input time stamps kept for decode times */

struct slot {
    CUdeviceptr mem;
    size_t pitch;
    NV_ENC_REGISTERED_PTR reg;
    NV_ENC_INPUT_PTR mapped;
    NV_ENC_OUTPUT_PTR bs;
    bool busy;
};

struct nve {
    CudaFunctions *cu;
    NV_ENCODE_API_FUNCTION_LIST fn;
    void *enc;
    CUcontext ctx;
    bool own_ctx;
    bool types_logged;

    enum codec_id codec;
    int width, height, depth, bps;      /* bps: bytes per sample */
    NV_ENC_BUFFER_FORMAT fmt;
    NV_ENC_PIC_STRUCT pic_struct;

    struct slot slot[POOL];
    int pend[POOL];                     /* slots submitted, oldest first */
    int pend_head, pend_n;
    int ready;                          /* how many of them can be collected */
    bool flushing, eos_sent;
    bool init_done;                     /* nvEncInitializeEncoder succeeded */
    int bframes;                        /* pictures the card holds back */
    int64_t in_pts[PTS_RING];           /* presentation times, in the order given */
    uint64_t n_in, n_out;
    int64_t frame90;                    /* one picture, 90 kHz */

    uint8_t *host;                      /* staging for pictures from the CPU */
    size_t host_cap;
    uint8_t *out;                       /* the access unit last collected */
    size_t out_cap;
};

static void push_ctx(struct nve *e)
{
    e->cu->cuCtxPushCurrent(e->ctx);
}

static void pop_ctx(struct nve *e)
{
    CUcontext dummy;

    e->cu->cuCtxPopCurrent(&dummy);
}

static const char *nv_status(struct nve *e, NVENCSTATUS st)
{
    static const char *const names[] = {
        "success", "no encode device", "unsupported device", "invalid encoder device",
        "invalid device", "device no longer exists", "invalid pointer", "invalid event",
        "invalid parameter", "invalid call", "out of memory", "encoder not initialised",
        "unsupported parameter", "lock busy", "not enough buffer", "invalid version",
        "map failed", "need more input", "encoder busy", "event not registered",
        "generic error", "incompatible client key", "unimplemented", "resource register failed",
        "resource not registered", "resource not mapped",
    };
    const char *more = e->enc && e->fn.nvEncGetLastErrorString
                           ? e->fn.nvEncGetLastErrorString(e->enc) : NULL;
    static _Thread_local char buf[256];

    snprintf(buf, sizeof(buf), "%s%s%s",
             (unsigned)st < sizeof(names) / sizeof(names[0]) ? names[st] : "unknown status",
             more && more[0] ? ": " : "", more && more[0] ? more : "");
    return buf;
}

static int cap(struct nve *e, GUID codec, NV_ENC_CAPS what)
{
    NV_ENC_CAPS_PARAM p;
    int val = 0;

    memset(&p, 0, sizeof(p));
    p.version = NV_ENC_CAPS_PARAM_VER;
    p.capsToQuery = what;
    if (e->fn.nvEncGetEncodeCaps(e->enc, codec, &p, &val) != NV_ENC_SUCCESS)
        return 0;
    return val;
}

/* Called with the context current. */
static NVENCSTATUS session_open(struct nve *e)
{
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sp;
    NVENCSTATUS st;

    memset(&sp, 0, sizeof(sp));
    sp.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    sp.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    sp.device = e->ctx;
    sp.apiVersion = NVENCAPI_VERSION;
    e->enc = NULL;
    st = e->fn.nvEncOpenEncodeSessionEx(&sp, &e->enc);
    if (st != NV_ENC_SUCCESS) {
        /* NVIDIA's rule: a session that failed to open is destroyed all
         * the same, or it stays counted against the card. */
        if (e->enc)
            e->fn.nvEncDestroyEncoder(e->enc);
        e->enc = NULL;
    }
    return st;
}

/* Pictures the card still holds (B-frames waiting for their reference)
 * must be brought out before anything is torn down: the driver crashed
 * inside nvEncDestroyEncoder on a Tesla P40 when they were not. Called
 * with the context current. */
static void drain_all(struct nve *e)
{
    if (!e->eos_sent) {
        NV_ENC_PIC_PARAMS pp;

        memset(&pp, 0, sizeof(pp));
        pp.version = NV_ENC_PIC_PARAMS_VER;
        pp.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        e->fn.nvEncEncodePicture(e->enc, &pp);
        e->eos_sent = true;
    }
    while (e->pend_n > 0) {
        struct slot *s = &e->slot[e->pend[e->pend_head]];
        NV_ENC_LOCK_BITSTREAM lb;

        e->pend_head = (e->pend_head + 1) % POOL;
        e->pend_n--;
        memset(&lb, 0, sizeof(lb));
        lb.version = NV_ENC_LOCK_BITSTREAM_VER;
        lb.outputBitstream = s->bs;
        if (e->fn.nvEncLockBitstream(e->enc, &lb) == NV_ENC_SUCCESS)
            e->fn.nvEncUnlockBitstream(e->enc, s->bs);
        if (s->mapped) {
            e->fn.nvEncUnmapInputResource(e->enc, s->mapped);
            s->mapped = NULL;
        }
        s->busy = false;
    }
    e->ready = 0;
}

static void destroy(struct nve *e)
{
    if (e->enc) {
        push_ctx(e);
        if (e->init_done)
            drain_all(e);
        for (int i = 0; i < POOL; i++) {
            struct slot *s = &e->slot[i];

            if (s->mapped)
                e->fn.nvEncUnmapInputResource(e->enc, s->mapped);
            if (s->reg)
                e->fn.nvEncUnregisterResource(e->enc, s->reg);
            if (s->bs)
                e->fn.nvEncDestroyBitstreamBuffer(e->enc, s->bs);
            if (s->mem)
                e->cu->cuMemFree(s->mem);
        }
        e->fn.nvEncDestroyEncoder(e->enc);
        pop_ctx(e);
    }
    if (e->own_ctx)
        nv_ctx_put(e->ctx);
    free(e->host);
    free(e->out);
    free(e);
}

static void set_vui(NV_ENC_CONFIG_H264_VUI_PARAMETERS *v, const struct venc_cfg *cfg)
{
    bool colour = (cfg->primaries > 0 && cfg->primaries != 2) ||
                  (cfg->transfer > 0 && cfg->transfer != 2) ||
                  (cfg->matrix > 0 && cfg->matrix != 2);

    if (!colour && !cfg->full_range)
        return;
    v->videoSignalTypePresentFlag = 1;
    v->videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
    v->videoFullRangeFlag = cfg->full_range ? 1 : 0;
    if (colour) {
        v->colourDescriptionPresentFlag = 1;
        v->colourPrimaries = (NV_ENC_VUI_COLOR_PRIMARIES)(cfg->primaries > 0 ? cfg->primaries : 2);
        v->transferCharacteristics =
            (NV_ENC_VUI_TRANSFER_CHARACTERISTIC)(cfg->transfer > 0 ? cfg->transfer : 2);
        v->colourMatrix = (NV_ENC_VUI_MATRIX_COEFFS)(cfg->matrix > 0 ? cfg->matrix : 2);
    }
}

static int gcd(int64_t a, int64_t b)
{
    while (b) {
        int64_t t = a % b;

        a = b;
        b = t;
    }
    return a ? (int)a : 1;
}

static void *nve_open(const struct venc_cfg *cfg, char *err, size_t errlen)
{
    static const GUID *const presets[7] = {
        &NV_ENC_PRESET_P1_GUID, &NV_ENC_PRESET_P2_GUID, &NV_ENC_PRESET_P3_GUID,
        &NV_ENC_PRESET_P4_GUID, &NV_ENC_PRESET_P5_GUID, &NV_ENC_PRESET_P6_GUID,
        &NV_ENC_PRESET_P7_GUID,
    };
    NV_ENC_PRESET_CONFIG pc;
    NV_ENC_INITIALIZE_PARAMS ip;
    NV_ENC_CONFIG conf;
    NvencFunctions *ne;
    NVENCSTATUS st;
    struct nve *e;
    GUID codec = cfg->codec == CODEC_HEVC ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID;
    uint32_t max_ver = 0;
    int preset = 4, gop, g;
    int64_t dar_w, dar_h;
    double fps = cfg->fps_den > 0 ? (double)cfg->fps_num / cfg->fps_den : 25.0;

    if (cfg->preset) {
        if (cfg->preset[0] != 'p' || cfg->preset[1] < '1' || cfg->preset[1] > '7' ||
            cfg->preset[2]) {
            snprintf(err, errlen, "nvenc presets are p1 (fastest) to p7 (best), not '%s'",
                     cfg->preset);
            return NULL;
        }
        preset = cfg->preset[1] - '0';
    }
    if (cfg->depth != 8 && cfg->depth != 10) {
        snprintf(err, errlen, "nvenc: %d-bit video is not supported", cfg->depth);
        return NULL;
    }
    if (cfg->depth == 10 && cfg->codec != CODEC_HEVC) {
        snprintf(err, errlen, "nvenc: 10-bit is only encoded as H.265");
        return NULL;
    }
    if (!nv_load(err, errlen))
        return NULL;
    ne = nv_ne();
    if (ne->NvEncodeAPIGetMaxSupportedVersion(&max_ver) != NV_ENC_SUCCESS ||
        max_ver < ((NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION)) {
        snprintf(err, errlen, "the NVIDIA driver is too old: it offers encode API %u.%u, "
                 "this program needs %d.%d (driver 530.41.03 or newer)",
                 max_ver >> 4, max_ver & 0xF, NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
        return NULL;
    }

    e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->cu = nv_cu();
    e->codec = cfg->codec;
    e->width = cfg->width;
    e->height = cfg->height;
    e->depth = cfg->depth;
    e->bps = cfg->depth > 8 ? 2 : 1;
    e->fmt = cfg->depth > 8 ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT : NV_ENC_BUFFER_FORMAT_NV12;
    e->pic_struct = NV_ENC_PIC_STRUCT_FRAME;
    if (cfg->cuda_ctx) {
        /* Where the decoder leaves its pictures. The decoder may go before
         * this encoder does (it is replaced when the card will not decode
         * a stream any more), so the context is held here too. */
        e->ctx = cfg->cuda_ctx;
        nv_ctx_ref(e->ctx);
        e->own_ctx = true;
    } else {
        e->ctx = nv_ctx_get(cfg->gpu, err, errlen);
        if (!e->ctx) {
            free(e);
            return NULL;
        }
        e->own_ctx = true;
    }

    e->fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    st = ne->NvEncodeAPICreateInstance(&e->fn);
    if (st != NV_ENC_SUCCESS) {
        snprintf(err, errlen, "NvEncodeAPICreateInstance: %s", nv_status(e, st));
        destroy(e);
        return NULL;
    }

    push_ctx(e);
    st = session_open(e);
    if (st != NV_ENC_SUCCESS) {
        snprintf(err, errlen, "no encode session: %s", nv_status(e, st));
        goto fail;
    }

    if (cfg->width > cap(e, codec, NV_ENC_CAPS_WIDTH_MAX) ||
        cfg->height > cap(e, codec, NV_ENC_CAPS_HEIGHT_MAX)) {
        snprintf(err, errlen, "%dx%d is more than this card encodes as %s", cfg->width,
                 cfg->height, codec_name(cfg->codec));
        goto fail;
    }
    if (cfg->depth == 10 && !cap(e, codec, NV_ENC_CAPS_SUPPORT_10BIT_ENCODE)) {
        snprintf(err, errlen, "this card does not encode 10-bit %s", codec_name(cfg->codec));
        goto fail;
    }

    /* Start from NVIDIA's own settings for the preset, then change only
     * what a broadcast stream needs. */
    memset(&pc, 0, sizeof(pc));
    pc.version = NV_ENC_PRESET_CONFIG_VER;
    pc.presetCfg.version = NV_ENC_CONFIG_VER;
    st = e->fn.nvEncGetEncodePresetConfigEx(e->enc, codec, *presets[preset - 1],
                                            NV_ENC_TUNING_INFO_HIGH_QUALITY, &pc);
    if (st != NV_ENC_SUCCESS) {
        snprintf(err, errlen, "preset p%d: %s", preset, nv_status(e, st));
        goto fail;
    }
    conf = pc.presetCfg;
    conf.version = NV_ENC_CONFIG_VER;
    /* What NVIDIA's preset makes the card do, said once per encoder: the
     * passes over each picture are most of what an encoder costs the card. */
    log_info("nvenc: %dx%d %s preset p%d (high quality): %s, adaptive quantisation %s/%s",
             cfg->width, cfg->height, cfg->codec == CODEC_H264 ? "H.264" : "HEVC", preset,
             conf.rcParams.multiPass == NV_ENC_TWO_PASS_FULL_RESOLUTION ? "two passes, full size"
             : conf.rcParams.multiPass == NV_ENC_TWO_PASS_QUARTER_RESOLUTION ? "two passes, the first at a quarter size"
             : "one pass",
             conf.rcParams.enableAQ ? "spatial" : "-", conf.rcParams.enableTemporalAQ ? "temporal" : "-");

    /* Key frames are asked for picture by picture (vframe.force_key); the
     * card's own interval is only a backstop, a little longer. */
    gop = (int)(fps * cfg->gop_ms / 1000.0) + 3;
    if (gop < 1)
        gop = 1;
    conf.gopLength = (uint32_t)gop;
    /* B-frames (H.264 only on this generation): see the top of this file. */
    e->bframes = 0;
    {
        int want = cfg->bframes >= 0 ? cfg->bframes : cfg->codec == CODEC_H264 ? BFRAMES_H264 : 0;

        if (cfg->profile && !strcmp(cfg->profile, "baseline"))
            want = 0;
        if (want > 0) {
            int most = cap(e, codec, NV_ENC_CAPS_NUM_MAX_BFRAMES);

            e->bframes = most < want ? most : want;
            if (e->bframes < want)
                log_info("nvenc: %d B-frame(s) asked for, the card does %d in %s", want, most > 0 ? most : 0,
                         cfg->codec == CODEC_H264 ? "H.264" : "HEVC");
        }
    }
    conf.frameIntervalP = (int32_t)(e->bframes + 1);
    e->frame90 = (int64_t)(90000.0 / fps + 0.5);
    conf.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    conf.rcParams.averageBitRate = (uint32_t)cfg->bitrate;
    conf.rcParams.maxBitRate = (uint32_t)cfg->bitrate;
    conf.rcParams.vbvBufferSize = (uint32_t)((int64_t)cfg->bitrate * cfg->vbv_ms / 1000);
    conf.rcParams.vbvInitialDelay = conf.rcParams.vbvBufferSize / 10 * 9;
    conf.rcParams.enableLookahead = 0;
    conf.rcParams.lookaheadDepth = 0;
    conf.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;

    if (cfg->codec == CODEC_H264) {
        NV_ENC_CONFIG_H264 *h = &conf.encodeCodecConfig.h264Config;

        conf.profileGUID = cfg->profile && !strcmp(cfg->profile, "baseline") ? NV_ENC_H264_PROFILE_BASELINE_GUID
                         : cfg->profile && !strcmp(cfg->profile, "main") ? NV_ENC_H264_PROFILE_MAIN_GUID
                         : NV_ENC_H264_PROFILE_HIGH_GUID;
        h->idrPeriod = (uint32_t)gop;
        h->repeatSPSPPS = 1;                /* parameter sets with every key frame */
        h->outputAUD = 1;                   /* a delimiter on every access unit */
        h->enableFillerDataInsertion = 0;
        h->chromaFormatIDC = 1;
        set_vui(&h->h264VUIParameters, cfg);
        if (cfg->interlaced) {
            if (cap(e, codec, NV_ENC_CAPS_SUPPORT_FIELD_ENCODING) >= 1) {
                conf.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FIELD;
                e->pic_struct = cfg->tff ? NV_ENC_PIC_STRUCT_FIELD_TOP_BOTTOM
                                         : NV_ENC_PIC_STRUCT_FIELD_BOTTOM_TOP;
            } else {
                log_warn("nvenc: this card does not encode interlaced H.264; the fields "
                         "are encoded together as frames");
            }
        }
    } else {
        NV_ENC_CONFIG_HEVC *h = &conf.encodeCodecConfig.hevcConfig;

        conf.profileGUID = cfg->depth > 8 ? NV_ENC_HEVC_PROFILE_MAIN10_GUID
                                          : NV_ENC_HEVC_PROFILE_MAIN_GUID;
        h->idrPeriod = (uint32_t)gop;
        h->repeatSPSPPS = 1;
        h->outputAUD = 1;
        h->enableFillerDataInsertion = 0;
        h->chromaFormatIDC = 1;
        h->pixelBitDepthMinus8 = (uint32_t)(cfg->depth - 8);
        set_vui(&h->hevcVUIParameters, cfg);
        if (cfg->interlaced)
            log_warn("nvenc: interlaced video is encoded as whole frames in H.265");
    }

    /* The card is told the shape of the picture, not of a pixel. */
    dar_w = (int64_t)cfg->width * (cfg->sar_num > 0 ? cfg->sar_num : 1);
    dar_h = (int64_t)cfg->height * (cfg->sar_den > 0 ? cfg->sar_den : 1);
    g = gcd(dar_w, dar_h);
    dar_w /= g;
    dar_h /= g;

    memset(&ip, 0, sizeof(ip));
    ip.version = NV_ENC_INITIALIZE_PARAMS_VER;
    ip.encodeGUID = codec;
    ip.presetGUID = *presets[preset - 1];
    ip.tuningInfo = NV_ENC_TUNING_INFO_HIGH_QUALITY;
    ip.encodeWidth = (uint32_t)cfg->width;
    ip.encodeHeight = (uint32_t)cfg->height;
    ip.maxEncodeWidth = (uint32_t)cfg->width;
    ip.maxEncodeHeight = (uint32_t)cfg->height;
    ip.darWidth = (uint32_t)dar_w;
    ip.darHeight = (uint32_t)dar_h;
    ip.frameRateNum = (uint32_t)(cfg->fps_num > 0 ? cfg->fps_num : 25);
    ip.frameRateDen = (uint32_t)(cfg->fps_den > 0 ? cfg->fps_den : 1);
    ip.enableEncodeAsync = 0;
    ip.enablePTD = 1;                       /* the card decides picture types */
    ip.encodeConfig = &conf;
    st = e->fn.nvEncInitializeEncoder(e->enc, &ip);
    /* The card said it encodes fields and then would not. Rather than
     * lose the card for this stream: first fields without B-frames (the
     * combination has not been tried on a card), then the two fields
     * together as a frame. Each try in a session that has not seen the
     * failure. */
    if (st != NV_ENC_SUCCESS && conf.frameFieldMode == NV_ENC_PARAMS_FRAME_FIELD_MODE_FIELD &&
        e->bframes > 0) {
        log_warn("nvenc: interlaced encoding with B-frames was refused (%s); trying "
                 "without B-frames", nv_status(e, st));
        e->fn.nvEncDestroyEncoder(e->enc);
        st = session_open(e);
        if (st == NV_ENC_SUCCESS) {
            e->bframes = 0;
            conf.frameIntervalP = 1;
            st = e->fn.nvEncInitializeEncoder(e->enc, &ip);
        }
    }
    if (st != NV_ENC_SUCCESS && conf.frameFieldMode == NV_ENC_PARAMS_FRAME_FIELD_MODE_FIELD) {
        log_warn("nvenc: interlaced encoding was refused (%s); the fields are encoded "
                 "together as frames", nv_status(e, st));
        e->fn.nvEncDestroyEncoder(e->enc);
        st = session_open(e);
        if (st == NV_ENC_SUCCESS) {
            conf.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
            e->pic_struct = NV_ENC_PIC_STRUCT_FRAME;
            st = e->fn.nvEncInitializeEncoder(e->enc, &ip);
        }
    }
    if (st != NV_ENC_SUCCESS) {
        snprintf(err, errlen, "nvEncInitializeEncoder (%dx%d, %d-bit, %d kb/s, p%d): %s",
                 cfg->width, cfg->height, cfg->depth, cfg->bitrate / 1000, preset,
                 nv_status(e, st));
        goto fail;
    }

    e->init_done = true;
    for (int i = 0; i < POOL; i++) {
        struct slot *s = &e->slot[i];
        NV_ENC_CREATE_BITSTREAM_BUFFER bb;
        NV_ENC_REGISTER_RESOURCE rr;
        CUresult r;

        r = e->cu->cuMemAllocPitch(&s->mem, &s->pitch, (size_t)cfg->width * (size_t)e->bps,
                                   (size_t)cfg->height * 3 / 2, 16);
        if (r != CUDA_SUCCESS) {
            s->mem = 0;
            snprintf(err, errlen, "no memory on the card for input pictures: %s",
                     nv_cu_err(r));
            goto fail;
        }
        memset(&rr, 0, sizeof(rr));
        rr.version = NV_ENC_REGISTER_RESOURCE_VER;
        rr.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
        rr.width = (uint32_t)cfg->width;
        rr.height = (uint32_t)cfg->height;
        rr.pitch = (uint32_t)s->pitch;
        rr.resourceToRegister = (void *)(uintptr_t)s->mem;
        rr.bufferFormat = e->fmt;
        rr.bufferUsage = NV_ENC_INPUT_IMAGE;
        st = e->fn.nvEncRegisterResource(e->enc, &rr);
        if (st != NV_ENC_SUCCESS) {
            snprintf(err, errlen, "nvEncRegisterResource: %s", nv_status(e, st));
            goto fail;
        }
        s->reg = rr.registeredResource;

        memset(&bb, 0, sizeof(bb));
        bb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        st = e->fn.nvEncCreateBitstreamBuffer(e->enc, &bb);
        if (st != NV_ENC_SUCCESS) {
            snprintf(err, errlen, "nvEncCreateBitstreamBuffer: %s", nv_status(e, st));
            goto fail;
        }
        s->bs = bb.bitstreamBuffer;
    }
    pop_ctx(e);
    return e;

fail:
    pop_ctx(e);
    destroy(e);
    return NULL;
}

/* Copies a picture into slot memory. Called with the context current. */
static bool load(struct nve *e, struct slot *s, const struct vframe *f)
{
    CUDA_MEMCPY2D c;
    size_t row = (size_t)e->width * (size_t)e->bps;
    int cw = e->width / 2, ch = e->height / 2;

    memset(&c, 0, sizeof(c));
    c.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    c.dstDevice = s->mem;
    c.dstPitch = s->pitch;
    c.WidthInBytes = row;

    if (f->mem == VMEM_CUDA) {
        /* Luma and, right below it, interleaved chroma: one copy, card
         * to card. */
        c.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        c.srcDevice = (CUdeviceptr)(uintptr_t)f->plane[0];
        c.srcPitch = (size_t)f->stride[0];
        c.Height = (size_t)e->height * 3 / 2;
        return e->cu->cuMemcpy2D(&c) == CUDA_SUCCESS;
    }

    /* From ordinary memory: three planes in, two out. The chroma planes
     * are interleaved, and 10-bit samples moved to the top of their 16
     * bits, in a staging buffer; then two uploads. */
    {
        size_t need = row * (size_t)e->height + row * (size_t)ch;
        uint8_t *y, *uv;

        if (need > e->host_cap) {
            uint8_t *n = realloc(e->host, need);

            if (!n)
                return false;
            e->host = n;
            e->host_cap = need;
        }
        y = e->host;
        uv = e->host + row * (size_t)e->height;
        if (e->bps == 1) {
            for (int r = 0; r < e->height; r++)
                memcpy(y + (size_t)r * row, f->plane[0] + (size_t)r * f->stride[0], row);
            for (int r = 0; r < ch; r++) {
                const uint8_t *u = f->plane[1] + (size_t)r * f->stride[1];
                const uint8_t *v = f->plane[2] + (size_t)r * f->stride[2];
                uint8_t *d = uv + (size_t)r * row;

                for (int x = 0; x < cw; x++) {
                    d[2 * x] = u[x];
                    d[2 * x + 1] = v[x];
                }
            }
        } else {
            for (int r = 0; r < e->height; r++) {
                const uint8_t *src = f->plane[0] + (size_t)r * f->stride[0];
                uint8_t *d = y + (size_t)r * row;

                for (int x = 0; x < e->width; x++) {
                    unsigned v = ((unsigned)src[2 * x] | ((unsigned)src[2 * x + 1] << 8)) << 6;

                    d[2 * x] = (uint8_t)v;
                    d[2 * x + 1] = (uint8_t)(v >> 8);
                }
            }
            for (int r = 0; r < ch; r++) {
                const uint8_t *u = f->plane[1] + (size_t)r * f->stride[1];
                const uint8_t *v = f->plane[2] + (size_t)r * f->stride[2];
                uint8_t *d = uv + (size_t)r * row;

                for (int x = 0; x < cw; x++) {
                    unsigned a = ((unsigned)u[2 * x] | ((unsigned)u[2 * x + 1] << 8)) << 6;
                    unsigned b = ((unsigned)v[2 * x] | ((unsigned)v[2 * x + 1] << 8)) << 6;

                    d[4 * x] = (uint8_t)a;
                    d[4 * x + 1] = (uint8_t)(a >> 8);
                    d[4 * x + 2] = (uint8_t)b;
                    d[4 * x + 3] = (uint8_t)(b >> 8);
                }
            }
        }
        c.srcMemoryType = CU_MEMORYTYPE_HOST;
        c.srcHost = e->host;
        c.srcPitch = row;
        c.Height = (size_t)e->height + (size_t)ch;
        return e->cu->cuMemcpy2D(&c) == CUDA_SUCCESS;
    }
}

static int nve_push(void *ctx, const struct vframe *f)
{
    struct nve *e = ctx;
    NV_ENC_MAP_INPUT_RESOURCE mp;
    NV_ENC_PIC_PARAMS pp;
    NVENCSTATUS st;
    struct slot *s = NULL;
    int idx = -1;

    if (!f) {
        e->flushing = true;
        return 0;
    }
    if (f->width != e->width || f->height != e->height || f->depth != e->depth)
        return -1;
    for (int i = 0; i < POOL; i++)
        if (!e->slot[i].busy) {
            idx = i;
            break;
        }
    if (idx < 0 || e->pend_n == POOL)
        return -1;                          /* output was not collected */
    s = &e->slot[idx];

    push_ctx(e);
    if (!load(e, s, f)) {
        pop_ctx(e);
        log_dbg("nvenc: could not copy a picture to the card");
        return -1;
    }
    memset(&mp, 0, sizeof(mp));
    mp.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mp.registeredResource = s->reg;
    st = e->fn.nvEncMapInputResource(e->enc, &mp);
    if (st != NV_ENC_SUCCESS) {
        pop_ctx(e);
        log_dbg("nvenc: nvEncMapInputResource: %s", nv_status(e, st));
        return -1;
    }
    s->mapped = mp.mappedResource;

    memset(&pp, 0, sizeof(pp));
    pp.version = NV_ENC_PIC_PARAMS_VER;
    pp.inputWidth = (uint32_t)e->width;
    pp.inputHeight = (uint32_t)e->height;
    pp.inputPitch = (uint32_t)s->pitch;
    pp.inputBuffer = s->mapped;
    pp.outputBitstream = s->bs;
    pp.bufferFmt = mp.mappedBufferFmt;
    pp.pictureStruct = e->pic_struct;
    pp.inputTimeStamp = (uint64_t)(f->pts / STC_PER_90K);
    if (f->force_key)
        pp.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;
    st = e->fn.nvEncEncodePicture(e->enc, &pp);
    if (st != NV_ENC_SUCCESS && st != NV_ENC_ERR_NEED_MORE_INPUT) {
        e->fn.nvEncUnmapInputResource(e->enc, s->mapped);
        s->mapped = NULL;
        pop_ctx(e);
        log_dbg("nvenc: nvEncEncodePicture: %s", nv_status(e, st));
        return -1;
    }
    pop_ctx(e);

    s->busy = true;
    e->pend[(e->pend_head + e->pend_n) % POOL] = idx;
    e->pend_n++;
    /* (counted only once the card has taken it: a refused picture must
     * not put the decode times of all that follow one picture off) */
    e->in_pts[e->n_in % PTS_RING] = f->pts / STC_PER_90K;
    e->n_in++;
    if (st == NV_ENC_SUCCESS)
        e->ready = e->pend_n;               /* everything submitted so far is done */
    return 0;
}

static int nve_pull(void *ctx, struct vpacket *p)
{
    struct nve *e = ctx;
    NV_ENC_LOCK_BITSTREAM lb;
    NVENCSTATUS st;
    struct slot *s;

    if (e->ready == 0 && e->flushing && !e->eos_sent) {
        NV_ENC_PIC_PARAMS pp;

        memset(&pp, 0, sizeof(pp));
        pp.version = NV_ENC_PIC_PARAMS_VER;
        pp.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        push_ctx(e);
        e->fn.nvEncEncodePicture(e->enc, &pp);
        pop_ctx(e);
        e->eos_sent = true;
        e->ready = e->pend_n;
    }
    if (e->ready == 0)
        return 0;

    s = &e->slot[e->pend[e->pend_head]];
    e->pend_head = (e->pend_head + 1) % POOL;
    e->pend_n--;
    e->ready--;

    push_ctx(e);
    memset(&lb, 0, sizeof(lb));
    lb.version = NV_ENC_LOCK_BITSTREAM_VER;
    lb.outputBitstream = s->bs;
    lb.doNotWait = 0;
    st = e->fn.nvEncLockBitstream(e->enc, &lb);
    if (st == NV_ENC_SUCCESS) {
        if (lb.bitstreamSizeInBytes > e->out_cap) {
            uint8_t *n = realloc(e->out, lb.bitstreamSizeInBytes + 4096);

            if (n) {
                e->out = n;
                e->out_cap = lb.bitstreamSizeInBytes + 4096;
            }
        }
        if (lb.bitstreamSizeInBytes <= e->out_cap) {
            memcpy(e->out, lb.bitstreamBufferPtr, lb.bitstreamSizeInBytes);
            p->data = e->out;
            p->len = lb.bitstreamSizeInBytes;
            p->pts = (stc_t)lb.outputTimeStamp * STC_PER_90K;
            /* Decode time: the k-th picture out is decoded B pictures
             * before the k-th picture given is shown (the rule ffmpeg
             * uses). At a steady frame rate that is the time of the
             * picture given B earlier; after a gap in the input it does
             * not reach back across the gap, as that would. Never after
             * its own presentation time. Without B-frames: its own time. */
            {
                int64_t dts = e->in_pts[e->n_out % PTS_RING] - e->bframes * e->frame90;

                if (dts > (int64_t)lb.outputTimeStamp)
                    dts = (int64_t)lb.outputTimeStamp;
                p->dts = (stc_t)dts * STC_PER_90K;
                e->n_out++;
            }
            /* The driver's label is not relied on: on a Tesla P40 in
             * field mode it did not say IDR for any picture. The bytes
             * say what the picture is. */
            p->key = lb.pictureType == NV_ENC_PIC_TYPE_IDR ||
                     codec_es_has_key(e->codec, p->data, p->len, false);
            if (!e->types_logged && lb.pictureType != NV_ENC_PIC_TYPE_P &&
                lb.pictureType != NV_ENC_PIC_TYPE_IDR) {
                e->types_logged = true;
                log_dbg("nvenc: the driver labels a picture type %d (key by its bytes: %s)",
                        (int)lb.pictureType, p->key ? "yes" : "no");
            }
        } else {
            st = NV_ENC_ERR_OUT_OF_MEMORY;
        }
        e->fn.nvEncUnlockBitstream(e->enc, s->bs);
    } else {
        log_dbg("nvenc: nvEncLockBitstream: %s", nv_status(e, st));
    }
    if (s->mapped) {
        e->fn.nvEncUnmapInputResource(e->enc, s->mapped);
        s->mapped = NULL;
    }
    pop_ctx(e);
    s->busy = false;
    return st == NV_ENC_SUCCESS && p->len > 0 ? 1 : nve_pull(ctx, p);
}

static int nve_delay(void *ctx)
{
    struct nve *e = ctx;

    return e->bframes;
}

static void nve_close(void *ctx)
{
    destroy(ctx);
}

static bool nve_handles(enum codec_id c)
{
    return c == CODEC_H264 || c == CODEC_HEVC;
}

static const struct venc_ops nve_ops = { nve_open, nve_push, nve_pull, nve_delay, nve_close };

const struct codec_backend backend_nvenc = {
    .name = "nvenc", .codecs = "h264, hevc",
    .media = CODEC_VIDEO, .role = CODEC_ENCODER, .where = WHERE_NVIDIA,
    .priority = 20, .handles = nve_handles, .probe = nv_probe, .venc = &nve_ops,
};

#endif /* HAVE_NVIDIA */
