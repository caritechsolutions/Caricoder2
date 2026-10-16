/*
 * caricoder3 - H.264 / H.265 decode on the CPU through libavcodec
 * Copyright (c) 2026 CariTech Solutions
 *
 * libavcodec is the decoder library inside FFmpeg. Only that library is
 * used, and only here: bytes in, pictures out. Demultiplexing, time
 * stamps, clocks and multiplexing are all our own.
 */
#ifdef HAVE_AVCODEC

#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

#include "codec.h"
#include "log.h"

#define FIFO_MAX 64

struct av_dec {
    AVCodecContext *ctx;
    AVCodecParserContext *parser;
    AVPacket *pkt;
    AVFrame *fifo[FIFO_MAX];        /* decoded, not yet handed out */
    int head, count;
    AVFrame *cur;                   /* the one the caller is looking at */
    uint8_t *in;                    /* input copied here, with padding after it */
    size_t in_cap;
    bool warned_format;
    uint64_t errors;
};

static void *avd_open(const struct vdec_cfg *cfg, char *err, size_t errlen)
{
    enum AVCodecID id = cfg->codec == CODEC_HEVC ? AV_CODEC_ID_HEVC :
                        cfg->codec == CODEC_MPEG2 ? AV_CODEC_ID_MPEG2VIDEO : AV_CODEC_ID_H264;
    const AVCodec *codec = avcodec_find_decoder(id);
    struct av_dec *d;

    if (!codec) {
        snprintf(err, errlen, "this libavcodec has no %s decoder", codec_name(cfg->codec));
        return NULL;
    }
    d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    av_log_set_level(AV_LOG_QUIET);         /* errors are counted, not printed */
    d->ctx = avcodec_alloc_context3(codec);
    d->parser = av_parser_init(id);
    d->pkt = av_packet_alloc();
    d->cur = av_frame_alloc();
    if (!d->ctx || !d->parser || !d->pkt || !d->cur)
        goto fail;
    d->ctx->pkt_timebase = (AVRational){ 1, 90000 };
    /* Each thread is one more picture held back, so not "as many as the
     * machine has". */
    d->ctx->thread_count = codec_threads(CODEC_THREADS_DECODE);
    d->ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    if (avcodec_open2(d->ctx, codec, NULL) < 0) {
        snprintf(err, errlen, "libavcodec would not open its %s decoder",
                 codec_name(cfg->codec));
        goto fail;
    }
    return d;

fail:
    av_frame_free(&d->cur);
    av_packet_free(&d->pkt);
    if (d->parser)
        av_parser_close(d->parser);
    avcodec_free_context(&d->ctx);
    free(d);
    return NULL;
}

/* Everything the decoder has ready moves into our queue. */
static void collect(struct av_dec *d)
{
    for (;;) {
        AVFrame *f;

        if (d->count == FIFO_MAX) {
            /* Nobody is taking pictures away. Let the oldest go. */
            av_frame_free(&d->fifo[d->head]);
            d->head = (d->head + 1) % FIFO_MAX;
            d->count--;
        }
        f = av_frame_alloc();
        if (!f)
            return;
        if (avcodec_receive_frame(d->ctx, f) < 0) {
            av_frame_free(&f);
            return;
        }
        d->fifo[(d->head + d->count) % FIFO_MAX] = f;
        d->count++;
    }
}

static void send(struct av_dec *d, const uint8_t *data, int size)
{
    int rc;

    d->pkt->data = (uint8_t *)(uintptr_t)data;
    d->pkt->size = size;
    d->pkt->pts = d->parser->pts;
    d->pkt->dts = d->parser->dts;
    d->pkt->flags = d->parser->key_frame == 1 ? AV_PKT_FLAG_KEY : 0;
    rc = avcodec_send_packet(d->ctx, d->pkt);
    if (rc == AVERROR(EAGAIN)) {
        collect(d);
        rc = avcodec_send_packet(d->ctx, d->pkt);
    }
    if (rc < 0)
        d->errors++;
    collect(d);
}

static int avd_push(void *ctx, const uint8_t *es, size_t len, int64_t pts90, int64_t dts90)
{
    struct av_dec *d = ctx;
    int64_t pts = pts90 == NOPTS ? AV_NOPTS_VALUE : pts90;
    int64_t dts = dts90 == NOPTS ? AV_NOPTS_VALUE : dts90;
    uint64_t before = d->errors;

    /* The library reads a little past the end of what it is given, for
     * speed, and requires that there be zeroed room there. A PES packet
     * has none, so the data is copied to a buffer that does. */
    if (len + AV_INPUT_BUFFER_PADDING_SIZE > d->in_cap) {
        size_t cap = len + AV_INPUT_BUFFER_PADDING_SIZE + 65536;
        uint8_t *n = realloc(d->in, cap);

        if (!n)
            return -1;
        d->in = n;
        d->in_cap = cap;
    }
    memcpy(d->in, es, len);
    memset(d->in + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    es = d->in;

    while (len > 0) {
        uint8_t *out = NULL;
        int out_size = 0;
        int chunk = len > (1u << 30) ? (1 << 30) : (int)len;
        int used = av_parser_parse2(d->parser, d->ctx, &out, &out_size, es, chunk,
                                    pts, dts, 0);

        if (used < 0)
            return -1;
        es += used;
        len -= (size_t)used;
        pts = dts = AV_NOPTS_VALUE;         /* they belonged to the first bytes */
        if (out_size > 0)
            send(d, out, out_size);
        else if (used == 0)
            break;
    }
    return d->errors == before ? 0 : -1;
}

static int avd_pull(void *ctx, struct vframe *f)
{
    struct av_dec *d = ctx;

    for (;;) {
        AVFrame *a;
        int depth;

        if (d->count == 0)
            return 0;
        av_frame_unref(d->cur);
        av_frame_move_ref(d->cur, d->fifo[d->head]);
        av_frame_free(&d->fifo[d->head]);
        d->head = (d->head + 1) % FIFO_MAX;
        d->count--;
        a = d->cur;

        switch (a->format) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P:
            depth = 8;
            break;
        case AV_PIX_FMT_YUV420P10LE:
            depth = 10;
            break;
        default:
            if (!d->warned_format) {
                d->warned_format = true;
                log_err("video is not 4:2:0 at 8 or 10 bits (libavcodec pixel format %d): "
                        "it cannot be transcoded", a->format);
            }
            continue;
        }
        memset(f, 0, sizeof(*f));
        f->mem = VMEM_CPU;
        f->width = a->width;
        f->height = a->height;
        f->depth = depth;
        for (int i = 0; i < 3; i++) {
            f->plane[i] = a->data[i];
            f->stride[i] = a->linesize[i];
        }
        f->pts = a->best_effort_timestamp == AV_NOPTS_VALUE
                     ? NOPTS : a->best_effort_timestamp * STC_PER_90K;
        f->key = (a->flags & AV_FRAME_FLAG_KEY) != 0;
        f->interlaced = (a->flags & AV_FRAME_FLAG_INTERLACED) != 0;
        f->tff = (a->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) != 0;
        f->sar_num = a->sample_aspect_ratio.num;
        f->sar_den = a->sample_aspect_ratio.den;
        f->fps_num = d->ctx->framerate.num;
        f->fps_den = d->ctx->framerate.den;
        f->primaries = a->color_primaries;
        f->transfer = a->color_trc;
        f->matrix = a->colorspace;
        f->full_range = a->color_range == AVCOL_RANGE_JPEG || a->format == AV_PIX_FMT_YUVJ420P;
        if (f->width <= 0 || f->height <= 0 || !f->plane[0] || !f->plane[1] || !f->plane[2])
            continue;
        return 1;
    }
}

static void avd_drain(void *ctx)
{
    struct av_dec *d = ctx;
    uint8_t *out = NULL;
    int out_size = 0;

    /* The parser holds the last access unit until it sees the next. */
    av_parser_parse2(d->parser, d->ctx, &out, &out_size, NULL, 0,
                     AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
    if (out_size > 0)
        send(d, out, out_size);
    avcodec_send_packet(d->ctx, NULL);
    collect(d);
}

static void avd_reset(void *ctx)
{
    struct av_dec *d = ctx;

    avcodec_flush_buffers(d->ctx);
    while (d->count) {
        av_frame_free(&d->fifo[d->head]);
        d->head = (d->head + 1) % FIFO_MAX;
        d->count--;
    }
}

static void avd_close(void *ctx)
{
    struct av_dec *d = ctx;

    while (d->count) {
        av_frame_free(&d->fifo[d->head]);
        d->head = (d->head + 1) % FIFO_MAX;
        d->count--;
    }
    av_frame_free(&d->cur);
    av_packet_free(&d->pkt);
    av_parser_close(d->parser);
    avcodec_free_context(&d->ctx);
    free(d->in);
    free(d);
}

static bool avd_handles(enum codec_id c)
{
    return c == CODEC_H264 || c == CODEC_HEVC || c == CODEC_MPEG2;
}

static const struct vdec_ops avd_ops = {
    avd_open, avd_push, avd_pull, avd_drain, avd_reset, avd_close,
};

const struct codec_backend backend_avcodec = {
    .name = "libavcodec", .codecs = "h264, hevc, mpeg2",
    .media = CODEC_VIDEO, .role = CODEC_DECODER, .where = WHERE_CPU,
    .priority = 10, .handles = avd_handles, .vdec = &avd_ops,
};

#endif /* HAVE_AVCODEC */
