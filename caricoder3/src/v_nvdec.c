/*
 * caricoder3 - H.264 / H.265 decode on an NVIDIA card (NVDEC)
 * Copyright (c) 2026 CariTech Solutions
 *
 * The card's own parser takes the elementary stream in pieces, as PES
 * packets deliver it, and calls back three times: when it knows the
 * picture format (then the decoder is created), when a picture is ready
 * to decode, and when one is ready to show. Shown pictures are queued
 * here and handed out one at a time, still on the card.
 *
 * Written on a machine without a card; first run on a Tesla P40 (driver
 * 535) on 2026-10-14, where it decoded H.264, H.265, 10-bit and fields as
 * expected. `caricoder3 --gpu-test` exercises it.
 */
#ifdef HAVE_NVIDIA

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec.h"
#include "log.h"
#include "nv_common.h"

#define DISP_MAX 32

struct nvd {
    CudaFunctions *cu;
    CuvidFunctions *cv;
    CUcontext ctx;
    CUvideoparser parser;
    CUvideodecoder dec;
    enum codec_id codec;
    bool only_8bit;

    CUVIDEOFORMAT fmt;
    unsigned coded_w, coded_h;
    int width, height, depth;       /* of the pictures handed out */
    int surfaces;

    CUVIDPARSERDISPINFO disp[DISP_MAX];
    int head, count;
    bool mapped;                    /* a picture is mapped and must be unmapped ... */
    unsigned long long map_ptr;     /* ... this one */
    bool seen_ts;
    bool disc;
    bool failed;
    char err[160];
};

static void push_ctx(struct nvd *d)
{
    d->cu->cuCtxPushCurrent(d->ctx);
}

static void pop_ctx(struct nvd *d)
{
    CUcontext dummy;

    d->cu->cuCtxPopCurrent(&dummy);
}

static int gcd(int a, int b)
{
    while (b) {
        int t = a % b;

        a = b;
        b = t;
    }
    return a ? a : 1;
}

/* ---- callbacks from the parser (inside cuvidParseVideoData) --------- */

static int CUDAAPI on_sequence(void *user, CUVIDEOFORMAT *f)
{
    struct nvd *d = user;
    CUVIDDECODECREATEINFO ci;
    CUresult r;
    int w = f->display_area.right - f->display_area.left;
    int h = f->display_area.bottom - f->display_area.top;
    int surfaces = f->min_num_decode_surfaces + 3;
    int depth = d->only_8bit ? 8 : 8 + f->bit_depth_luma_minus8;

    if (f->chroma_format != cudaVideoChromaFormat_420 ||
        (f->bit_depth_luma_minus8 != 0 && f->bit_depth_luma_minus8 != 2)) {
        snprintf(d->err, sizeof(d->err), "video is not 4:2:0 at 8 or 10 bits");
        d->failed = true;
        return 0;
    }
    w &= ~1;
    h &= ~1;
    if (w <= 0 || h <= 0 || f->coded_width == 0 || f->coded_height == 0) {
        d->failed = true;
        snprintf(d->err, sizeof(d->err), "the stream announces a %dx%d picture", w, h);
        return 0;
    }

    /* The same format again (parameter sets are repeated with every key
     * frame): nothing to do. */
    if (d->dec && d->coded_w == f->coded_width && d->coded_h == f->coded_height &&
        d->width == w && d->height == h && d->depth == depth &&
        surfaces <= d->surfaces) {
        d->fmt = *f;
        d->failed = false;
        return d->surfaces;
    }

    /* A new format. Pictures of the old one still waiting to be shown
     * belong to a decoder that is about to go. */
    if (d->dec) {
        /* (No picture is mapped at this point: nvd_push() let go of the
         * last one before calling the parser.) */
        d->cv->cuvidDestroyDecoder(d->dec);
        d->dec = NULL;
        d->head = d->count = 0;
    }

    memset(&ci, 0, sizeof(ci));
    ci.CodecType = f->codec;
    ci.ChromaFormat = f->chroma_format;
    ci.bitDepthMinus8 = f->bit_depth_luma_minus8;
    /* Asked for 8-bit pictures from 10-bit video, the card converts. */
    ci.OutputFormat = depth > 8 ? cudaVideoSurfaceFormat_P016 : cudaVideoSurfaceFormat_NV12;
    /* Fields stay woven together in the frame, as they came. */
    ci.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
    ci.ulCreationFlags = cudaVideoCreate_PreferCUVID;
    ci.ulNumDecodeSurfaces = (tcu_ulong)surfaces;
    ci.ulNumOutputSurfaces = 2;
    ci.ulWidth = f->coded_width;
    ci.ulHeight = f->coded_height;
    ci.ulMaxWidth = f->coded_width;
    ci.ulMaxHeight = f->coded_height;
    ci.display_area.left = (short)f->display_area.left;
    ci.display_area.top = (short)f->display_area.top;
    ci.display_area.right = (short)(f->display_area.left + w);
    ci.display_area.bottom = (short)(f->display_area.top + h);
    ci.ulTargetWidth = (tcu_ulong)w;
    ci.ulTargetHeight = (tcu_ulong)h;

    r = d->cv->cuvidCreateDecoder(&d->dec, &ci);
    if (r != CUDA_SUCCESS) {
        d->dec = NULL;
        d->failed = true;
        snprintf(d->err, sizeof(d->err), "cuvidCreateDecoder (%ux%u, %d-bit): %s",
                 f->coded_width, f->coded_height, 8 + f->bit_depth_luma_minus8,
                 nv_cu_err(r));
        log_err("nvdec: %s", d->err);
        return 0;
    }
    d->fmt = *f;
    d->coded_w = f->coded_width;
    d->coded_h = f->coded_height;
    d->width = w;
    d->height = h;
    d->depth = depth;
    d->surfaces = surfaces;
    d->failed = false;
    log_dbg("nvdec: decoder created, %dx%d (coded %ux%u), %d-bit, %d surfaces", w, h,
            f->coded_width, f->coded_height, d->depth, surfaces);
    return surfaces;                        /* more than 1: "use this many" */
}

static int CUDAAPI on_decode(void *user, CUVIDPICPARAMS *pp)
{
    struct nvd *d = user;

    if (!d->dec)
        return 0;
    return d->cv->cuvidDecodePicture(d->dec, pp) == CUDA_SUCCESS;
}

static int CUDAAPI on_display(void *user, CUVIDPARSERDISPINFO *di)
{
    struct nvd *d = user;

    if (!di)
        return 1;                           /* end of stream marker */
    if (d->count == DISP_MAX) {             /* nobody is taking them away */
        d->head = (d->head + 1) % DISP_MAX;
        d->count--;
    }
    d->disp[(d->head + d->count) % DISP_MAX] = *di;
    d->count++;
    return 1;
}

/* ---- the interface --------------------------------------------------- */

/* The picture last handed out stays mapped until the caller comes back. */
static void release_frame(struct nvd *d)
{
    if (d->mapped && d->dec)
        d->cv->cuvidUnmapVideoFrame(d->dec, d->map_ptr);
    d->mapped = false;
}

static void *nvd_open(const struct vdec_cfg *cfg, char *err, size_t errlen)
{
    struct nvd *d;
    CUVIDPARSERPARAMS pp;
    CUresult r;

    if (!cfg->want_cuda) {
        snprintf(err, errlen, "decoding on the card for an encoder on the CPU is not "
                 "built yet (use --decoder cpu, or --encoder nvidia)");
        return NULL;
    }
    d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->codec = cfg->codec;
    d->only_8bit = cfg->only_8bit;
    d->ctx = nv_ctx_get(cfg->gpu, err, errlen);
    if (!d->ctx) {
        free(d);
        return NULL;
    }
    d->cu = nv_cu();
    d->cv = nv_cv();

    memset(&pp, 0, sizeof(pp));
    pp.CodecType = cfg->codec == CODEC_HEVC ? cudaVideoCodec_HEVC : cudaVideoCodec_H264;
    pp.ulMaxNumDecodeSurfaces = 1;          /* the sequence callback says how many */
    pp.ulClockRate = 90000;                 /* our time stamps are 90 kHz */
    pp.ulErrorThreshold = 100;              /* decode damaged pictures too */
    pp.ulMaxDisplayDelay = 2;
    pp.pUserData = d;
    pp.pfnSequenceCallback = on_sequence;
    pp.pfnDecodePicture = on_decode;
    pp.pfnDisplayPicture = on_display;

    push_ctx(d);
    r = d->cv->cuvidCreateVideoParser(&d->parser, &pp);
    pop_ctx(d);
    if (r != CUDA_SUCCESS) {
        snprintf(err, errlen, "cuvidCreateVideoParser: %s", nv_cu_err(r));
        nv_ctx_put(d->ctx);
        free(d);
        return NULL;
    }
    return d;
}

static int nvd_push(void *ctx, const uint8_t *es, size_t len, int64_t pts90, int64_t dts90)
{
    struct nvd *d = ctx;
    CUVIDSOURCEDATAPACKET pkt;
    CUresult r;

    (void)dts90;
    memset(&pkt, 0, sizeof(pkt));
    pkt.payload = es;
    pkt.payload_size = (tcu_ulong)len;
    if (pts90 != NOPTS) {
        pkt.flags |= CUVID_PKT_TIMESTAMP;
        pkt.timestamp = pts90;
        d->seen_ts = true;
    }
    if (d->disc) {
        pkt.flags |= CUVID_PKT_DISCONTINUITY;
        d->disc = false;
    }
    push_ctx(d);
    release_frame(d);
    r = d->cv->cuvidParseVideoData(d->parser, &pkt);
    pop_ctx(d);
    /* A format this card or this code does not take, or no decoder to be
     * had: not this unit's fault, and the next will fare no better. */
    if (d->failed)
        return VDEC_CANNOT;
    return r == CUDA_SUCCESS ? 0 : -1;
}

static int nvd_pull(void *ctx, struct vframe *f)
{
    struct nvd *d = ctx;
    CUVIDPARSERDISPINFO di;
    CUVIDPROCPARAMS vpp;
    unsigned int pitch = 0;
    CUresult r;
    int dar_w, dar_h, g;

    push_ctx(d);
    release_frame(d);
    if (d->count == 0 || !d->dec) {
        pop_ctx(d);
        return 0;
    }
    di = d->disp[d->head];
    d->head = (d->head + 1) % DISP_MAX;
    d->count--;

    memset(&vpp, 0, sizeof(vpp));
    vpp.progressive_frame = di.progressive_frame;
    vpp.top_field_first = di.top_field_first;
    vpp.unpaired_field = di.repeat_first_field < 0;
    r = d->cv->cuvidMapVideoFrame(d->dec, di.picture_index, &d->map_ptr, &pitch, &vpp);
    pop_ctx(d);
    if (r != CUDA_SUCCESS) {
        log_dbg("nvdec: cuvidMapVideoFrame: %s", nv_cu_err(r));
        return 0;
    }
    d->mapped = true;

    memset(f, 0, sizeof(*f));
    f->mem = VMEM_CUDA;
    f->width = d->width;
    f->height = d->height;
    f->depth = d->depth;
    /* One surface: the luma rows, and directly below them the rows of
     * interleaved chroma, all at the same pitch. */
    f->plane[0] = (uint8_t *)(uintptr_t)d->map_ptr;
    f->stride[0] = (int)pitch;
    /* A picture whose access unit came without a time stamp reports 0. */
    f->pts = (di.timestamp == 0 && d->seen_ts) ? NOPTS : (stc_t)di.timestamp * STC_PER_90K;
    f->interlaced = !di.progressive_frame;
    f->tff = di.top_field_first != 0;
    f->fps_num = (int)d->fmt.frame_rate.numerator;
    f->fps_den = (int)d->fmt.frame_rate.denominator;
    /* The card gives the shape of the picture; the encoder wants the
     * shape of a pixel. */
    dar_w = d->fmt.display_aspect_ratio.x;
    dar_h = d->fmt.display_aspect_ratio.y;
    if (dar_w > 0 && dar_h > 0) {
        int64_t sn = (int64_t)dar_w * d->height, sd = (int64_t)dar_h * d->width;

        g = gcd((int)(sn % 2000000000), (int)(sd % 2000000000));
        if (sn < 2000000000 && sd < 2000000000) {
            f->sar_num = (int)sn / g;
            f->sar_den = (int)sd / g;
        }
    }
    f->primaries = d->fmt.video_signal_description.color_primaries;
    f->transfer = d->fmt.video_signal_description.transfer_characteristics;
    f->matrix = d->fmt.video_signal_description.matrix_coefficients;
    f->full_range = d->fmt.video_signal_description.video_full_range_flag;
    f->cuda_ctx = d->ctx;
    return 1;
}

static void nvd_drain(void *ctx)
{
    struct nvd *d = ctx;
    CUVIDSOURCEDATAPACKET pkt;

    memset(&pkt, 0, sizeof(pkt));
    pkt.flags = CUVID_PKT_ENDOFSTREAM;
    push_ctx(d);
    release_frame(d);
    d->cv->cuvidParseVideoData(d->parser, &pkt);
    pop_ctx(d);
}

static void nvd_reset(void *ctx)
{
    struct nvd *d = ctx;

    d->disc = true;
}

static void nvd_close(void *ctx)
{
    struct nvd *d = ctx;

    push_ctx(d);
    release_frame(d);
    if (d->parser)
        d->cv->cuvidDestroyVideoParser(d->parser);
    if (d->dec)
        d->cv->cuvidDestroyDecoder(d->dec);
    pop_ctx(d);
    nv_ctx_put(d->ctx);
    free(d);
}

static bool nvd_handles(enum codec_id c)
{
    return c == CODEC_H264 || c == CODEC_HEVC;
}

static const struct vdec_ops nvd_ops = {
    nvd_open, nvd_push, nvd_pull, nvd_drain, nvd_reset, nvd_close,
};

const struct codec_backend backend_nvdec = {
    .name = "nvdec", .codecs = "h264, hevc",
    .media = CODEC_VIDEO, .role = CODEC_DECODER, .where = WHERE_NVIDIA,
    .priority = 20, .handles = nvd_handles, .probe = nv_probe, .vdec = &nvd_ops,
};

#endif /* HAVE_NVIDIA */
