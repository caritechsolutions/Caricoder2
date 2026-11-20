/*
 * caricoder3 - a self-test for the NVIDIA path
 * Copyright (c) 2026 CariTech Solutions
 *
 *   caricoder3 --gpu-test [--gpu N]
 *
 * Needs no input stream. It makes pictures itself and puts them through
 * exactly the code a channel uses, one step at a time, and says after each
 * whether it worked and, if not, what the driver answered:
 *
 *   1. the libraries load, the card is there
 *   2. pictures from memory are encoded on the card   (CPU decode -> NVENC)
 *   3. that result is decoded on the card             (NVDEC)
 *   4. the decoded pictures are encoded again without
 *      leaving the card                               (NVDEC -> NVENC)
 *
 * for H.264, H.265 and 10-bit H.265, and it times step 4 at 1080p.
 *
 * Then what the card's encoder offers (temporal AQ, look-ahead, weighted
 * prediction, ...), and each picture-quality option of struct venc_tune
 * tried on the card: does the encoder take it, and how many pictures a
 * second does decode + encode at 1080p manage with it, next to without.
 * The speed is of one stream alone on an idle card and of a made-up
 * picture: for comparing the options with each other, not a capacity.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "codec.h"
#include "log.h"

#ifdef HAVE_NVIDIA
#include "nv_common.h"
#include "nv_scale.h"

struct blob {
    uint8_t *data;
    size_t len, cap;
    size_t off[1024];               /* where each access unit starts */
    size_t size[1024];
    stc_t pts[1024];
    int n, keys;
};

static bool blob_add(struct blob *b, const struct vpacket *p)
{
    if (b->n == 1024)
        return false;
    if (b->len + p->len > b->cap) {
        size_t cap = (b->len + p->len) * 2 + 65536;
        uint8_t *n = realloc(b->data, cap);

        if (!n)
            return false;
        b->data = n;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p->data, p->len);
    b->off[b->n] = b->len;
    b->size[b->n] = p->len;
    b->pts[b->n] = p->pts;
    b->len += p->len;
    b->keys += p->key;
    b->n++;
    return true;
}

/* A picture that moves, so that the encoder has something to do. */
static void make_picture(struct vframe *f, uint8_t *mem, int w, int h, int depth, int i)
{
    int bps = depth > 8 ? 2 : 1;
    int cw = w / 2, ch = h / 2;
    uint8_t *y = mem, *u = mem + (size_t)w * h * bps, *v = u + (size_t)cw * ch * bps;

    memset(f, 0, sizeof(*f));
    f->mem = VMEM_CPU;
    f->width = w;
    f->height = h;
    f->depth = depth;
    f->plane[0] = y;
    f->plane[1] = u;
    f->plane[2] = v;
    f->stride[0] = w * bps;
    f->stride[1] = f->stride[2] = cw * bps;
    f->fps_num = 25;
    f->fps_den = 1;
    f->pts = (stc_t)(i + 250) * (STC_HZ / 25);
    for (int r = 0; r < h; r++)
        for (int x = 0; x < w; x++) {
            unsigned val = (unsigned)(((x + 3 * i) ^ (r + i)) & 0xFF);

            if (bps == 1) {
                y[(size_t)r * w + x] = (uint8_t)(16 + val * 219 / 255);
            } else {
                val = 64 + val * 876 / 255;
                y[((size_t)r * w + x) * 2] = (uint8_t)val;
                y[((size_t)r * w + x) * 2 + 1] = (uint8_t)(val >> 8);
            }
        }
    for (int r = 0; r < ch; r++)
        for (int x = 0; x < cw; x++) {
            unsigned a = (unsigned)((x + i) & 0xFF), b = (unsigned)((r + 2 * i) & 0xFF);

            if (bps == 1) {
                u[(size_t)r * cw + x] = (uint8_t)a;
                v[(size_t)r * cw + x] = (uint8_t)b;
            } else {
                u[((size_t)r * cw + x) * 2] = (uint8_t)(a << 2);
                u[((size_t)r * cw + x) * 2 + 1] = (uint8_t)(a >> 6);
                v[((size_t)r * cw + x) * 2] = (uint8_t)(b << 2);
                v[((size_t)r * cw + x) * 2 + 1] = (uint8_t)(b >> 6);
            }
        }
}

static void fill_cfg(struct venc_cfg *c, enum codec_id codec, int w, int h, int depth,
                     int gpu, void *cuda_ctx, bool interlaced)
{
    memset(c, 0, sizeof(*c));
    c->bframes = -1;
    c->codec = codec;
    c->width = w;
    c->height = h;
    c->depth = depth;
    c->fps_num = 25;
    c->fps_den = 1;
    c->bitrate = w >= 1920 ? 6000000 : 3000000;
    c->vbv_ms = 1000;
    c->gop_ms = 1000;
    c->gpu = gpu;
    c->cuda_ctx = cuda_ctx;
    c->interlaced = interlaced;
    c->tff = interlaced;
}

static double now_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* One round: `codec` at `depth`, re-encoded on the card as `codec2`. */
static bool round_trip(FILE *out, int gpu, enum codec_id codec, int depth,
                       enum codec_id codec2, int w, int h, int frames, bool timed,
                       bool interlaced)
{
    const struct codec_backend *eb = codec_find(CODEC_VIDEO, CODEC_ENCODER, codec, WHERE_NVIDIA, 0);
    const struct codec_backend *db = codec_find(CODEC_VIDEO, CODEC_DECODER, codec, WHERE_NVIDIA, 0);
    struct vdec_cfg dc = { .codec = codec, .gpu = gpu, .want_cuda = true };
    struct venc_cfg ec;
    struct blob first = { 0 }, second = { 0 };
    struct vframe f;
    struct vpacket p;
    char err[256] = "";
    void *enc = NULL, *dec = NULL, *enc2 = NULL;
    uint8_t *mem = malloc((size_t)w * h * 3 * (depth > 8 ? 2 : 1));
    int decoded = 0, fields = 0;
    double t0 = 0, t1 = 0;
    bool ok = false;

    fprintf(out, "\n%s, %d-bit, %dx%d%s, %d pictures\n", codec_name(codec), depth, w, h,
            interlaced ? " interlaced" : "", frames);
    if (!eb || !db || !mem) {
        fprintf(out, "  FAIL  the NVIDIA backends are not available\n");
        goto done;
    }

    /* 2. pictures from memory, encoded on the card */
    fill_cfg(&ec, codec, w, h, depth, gpu, NULL, interlaced);
    enc = eb->venc->open(&ec, err, sizeof(err));
    if (!enc) {
        fprintf(out, "  FAIL  encode from memory: could not open: %s\n", err);
        goto done;
    }
    for (int i = 0; i < frames; i++) {
        make_picture(&f, mem, w, h, depth, i);
        f.interlaced = f.tff = interlaced;
        if (eb->venc->push(enc, &f) < 0) {
            fprintf(out, "  FAIL  encode from memory: picture %d was refused "
                         "(run with -v for the driver's message)\n", i);
            goto done;
        }
        while (eb->venc->pull(enc, &p) == 1)
            blob_add(&first, &p);
    }
    eb->venc->push(enc, NULL);
    while (eb->venc->pull(enc, &p) == 1)
        blob_add(&first, &p);
    if (first.n != frames || first.keys < 1) {
        fprintf(out, "  FAIL  encode from memory: %d of %d pictures came out, %d key frames\n",
                first.n, frames, first.keys);
        goto done;
    }
    fprintf(out, "  ok    encode from memory: %d pictures, %zu kB, %d key frames\n",
            first.n, first.len / 1000, first.keys);

    /* 3. decoded on the card, and 4. encoded again without leaving it */
    dec = db->vdec->open(&dc, err, sizeof(err));
    if (!dec) {
        fprintf(out, "  FAIL  decode on the card: could not open: %s\n", err);
        goto done;
    }
    t0 = now_s();
    for (int i = 0; i <= first.n; i++) {
        if (i < first.n)
            db->vdec->push(dec, first.data + first.off[i], first.size[i],
                           first.pts[i] / STC_PER_90K, NOPTS);
        else
            db->vdec->drain(dec);
        while (db->vdec->pull(dec, &f) == 1) {
            decoded++;
            fields += f.interlaced;
            if (f.mem != VMEM_CUDA || f.width != w || f.height != h || f.depth != depth) {
                fprintf(out, "  FAIL  decode on the card: picture is %dx%d, %d-bit, %s\n",
                        f.width, f.height, f.depth, f.mem == VMEM_CUDA ? "on the card" : "in memory");
                goto done;
            }
            if (!enc2) {
                fill_cfg(&ec, codec2, w, h, depth, gpu, f.cuda_ctx, interlaced);
                enc2 = eb->venc->open(&ec, err, sizeof(err));
                if (!enc2) {
                    fprintf(out, "  ok    decode on the card: first picture arrived\n");
                    fprintf(out, "  FAIL  card-to-card encode to %s: could not open: %s\n",
                            codec_name(codec2), err);
                    goto done;
                }
            }
            if (eb->venc->push(enc2, &f) < 0) {
                fprintf(out, "  FAIL  card-to-card encode: picture %d was refused "
                             "(run with -v for the driver's message)\n", decoded);
                goto done;
            }
            while (eb->venc->pull(enc2, &p) == 1)
                blob_add(&second, &p);
        }
    }
    if (enc2) {
        eb->venc->push(enc2, NULL);
        while (eb->venc->pull(enc2, &p) == 1)
            blob_add(&second, &p);
    }
    t1 = now_s();
    if (decoded < frames - 2) {
        fprintf(out, "  FAIL  decode on the card: %d of %d pictures came out\n", decoded, frames);
        goto done;
    }
    fprintf(out, "  ok    decode on the card: %d pictures\n", decoded);
    if (interlaced)
        fprintf(out, "        %d of them came back marked as two fields%s\n", fields,
                fields == 0 ? " (the encoder coded whole frames: see any warning above)" : "");
    if (second.n != decoded || second.keys < 1) {
        fprintf(out, "  FAIL  card-to-card encode to %s: %d of %d pictures came out\n",
                codec_name(codec2), second.n, decoded);
        goto done;
    }
    fprintf(out, "  ok    card-to-card encode to %s: %d pictures, %zu kB\n",
            codec_name(codec2), second.n, second.len / 1000);
    if (timed && t1 > t0)
        fprintf(out, "        decode + encode on the card: %.0f pictures a second "
                     "(%.1f times real time at 25 a second)\n",
                decoded / (t1 - t0), decoded / (t1 - t0) / 25.0);
    ok = true;

done:
    if (enc2)
        eb->venc->close(enc2);
    if (dec)
        db->vdec->close(dec);
    if (enc)
        eb->venc->close(enc);
    free(first.data);
    free(second.data);
    free(mem);
    return ok;
}

static const char *yesno(int v)
{
    return v > 0 ? "yes" : "no";
}

static void show_caps(FILE *out, int gpu)
{
    static const int codecs[2] = { CODEC_H264, CODEC_HEVC };
    struct nvenc_caps c[2];
    char why[200] = "";

    fprintf(out, "\nWhat this card's encoder offers          H.264   H.265\n");
    for (int i = 0; i < 2; i++)
        if (!nvenc_query_caps(gpu, codecs[i], &c[i], why, sizeof(why))) {
            fprintf(out, "  could not ask the card: %s\n", why);
            return;
        }
    fprintf(out, "  B-frames (most)                         %-7d %d\n", c[0].bframes, c[1].bframes);
    fprintf(out, "  temporal AQ                             %-7s %s\n", yesno(c[0].temporal_aq), yesno(c[1].temporal_aq));
    fprintf(out, "  look-ahead                              %-7s %s\n", yesno(c[0].lookahead), yesno(c[1].lookahead));
    fprintf(out, "  weighted prediction                     %-7s %s\n", yesno(c[0].weighted_pred), yesno(c[1].weighted_pred));
    fprintf(out, "  10-bit                                  %-7s %s\n", yesno(c[0].ten_bit), yesno(c[1].ten_bit));
    fprintf(out, "  interlaced (fields)                     %-7s %s\n", yesno(c[0].fields), yesno(c[1].fields));
    fprintf(out, "  emphasis map (ROI)                      %-7s %s\n", yesno(c[0].emphasis_map), yesno(c[1].emphasis_map));
    fprintf(out, "  largest picture                         %dx%d  %dx%d\n", c[0].width_max, c[0].height_max,
            c[1].width_max, c[1].height_max);
    fprintf(out, "  (spatial AQ and two passes have no capability bit: every NVENC card has them)\n");
}

/* A source for the option runs: `frames` pictures encoded on the card. */
static bool make_source(int gpu, enum codec_id codec, int w, int h, int frames, struct blob *b,
                        char *err, size_t errlen)
{
    const struct codec_backend *eb = codec_find(CODEC_VIDEO, CODEC_ENCODER, codec, WHERE_NVIDIA, 0);
    uint8_t *mem = malloc((size_t)w * h * 3);
    struct venc_cfg ec;
    struct vframe f;
    struct vpacket p;
    void *enc = NULL;
    bool ok = false;

    if (!eb || !mem) {
        snprintf(err, errlen, "no NVIDIA encoder");
        goto done;
    }
    fill_cfg(&ec, codec, w, h, 8, gpu, NULL, false);
    enc = eb->venc->open(&ec, err, errlen);
    if (!enc)
        goto done;
    for (int i = 0; i < frames; i++) {
        make_picture(&f, mem, w, h, 8, i);
        if (eb->venc->push(enc, &f) < 0) {
            snprintf(err, errlen, "picture %d was refused", i);
            goto done;
        }
        while (eb->venc->pull(enc, &p) == 1)
            blob_add(b, &p);
    }
    eb->venc->push(enc, NULL);
    while (eb->venc->pull(enc, &p) == 1)
        blob_add(b, &p);
    ok = b->n == frames;
    if (!ok)
        snprintf(err, errlen, "%d of %d pictures came out", b->n, frames);
done:
    if (enc)
        eb->venc->close(enc);
    free(mem);
    return ok;
}

/* `src` decoded on the card and encoded as `codec` with `tune` (and made
 * 10-bit on the card first, for Main 10). Pictures a second, or 0. */
static double option_run(FILE *out, int gpu, const struct blob *src, enum codec_id in, enum codec_id codec,
                         const struct venc_tune *tune, bool main10, int bframes, const char *label,
                         double plain)
{
    const struct codec_backend *eb = codec_find(CODEC_VIDEO, CODEC_ENCODER, codec, WHERE_NVIDIA, 0);
    const struct codec_backend *db = codec_find(CODEC_VIDEO, CODEC_DECODER, in, WHERE_NVIDIA, 0);
    struct vdec_cfg dc = { .codec = in, .gpu = gpu, .want_cuda = true };
    struct nv_scaler *scaler = NULL;
    struct blob res = { 0 };
    struct venc_cfg ec;
    struct vframe f, w10;
    struct vpacket p;
    char err[256] = "";
    void *dec = NULL, *enc = NULL;
    int decoded = 0;
    double t0, t1, fps = 0;

    if (!eb || !db) {
        fprintf(out, "  FAIL  %-36s the NVIDIA backends are not available\n", label);
        return 0;
    }
    dec = db->vdec->open(&dc, err, sizeof(err));
    if (!dec) {
        fprintf(out, "  FAIL  %-36s decoder: %s\n", label, err);
        return 0;
    }
    t0 = now_s();
    for (int i = 0; i <= src->n; i++) {
        if (i < src->n)
            db->vdec->push(dec, src->data + src->off[i], src->size[i], src->pts[i] / STC_PER_90K, NOPTS);
        else
            db->vdec->drain(dec);
        while (db->vdec->pull(dec, &f) == 1) {
            decoded++;
            if (main10) {
                if (!nv_scale_to(&scaler, &f, f.width, f.height, 10, &w10, err, sizeof(err))) {
                    fprintf(out, "  FAIL  %-36s 8 to 10 bits on the card: %s\n", label, err);
                    goto done;
                }
                f = w10;
            }
            if (!enc) {
                fill_cfg(&ec, codec, f.width, f.height, f.depth, gpu, f.cuda_ctx, false);
                ec.tune = *tune;
                ec.bframes = bframes;
                if (main10)
                    ec.profile = "main10";
                enc = eb->venc->open(&ec, err, sizeof(err));
                if (!enc) {
                    fprintf(out, "  FAIL  %-36s the encoder would not open: %s\n", label, err);
                    goto done;
                }
            }
            if (eb->venc->push(enc, &f) < 0) {
                fprintf(out, "  FAIL  %-36s picture %d was refused (-v for the driver's message)\n",
                        label, decoded);
                goto done;
            }
            while (eb->venc->pull(enc, &p) == 1)
                blob_add(&res, &p);
        }
    }
    if (enc) {
        eb->venc->push(enc, NULL);
        while (eb->venc->pull(enc, &p) == 1)
            blob_add(&res, &p);
    }
    t1 = now_s();
    if (decoded < src->n - 2 || res.n != decoded || res.keys < 1) {
        fprintf(out, "  FAIL  %-36s %d pictures decoded, %d encoded, %d key frames\n", label,
                decoded, res.n, res.keys);
        goto done;
    }
    fps = t1 > t0 ? decoded / (t1 - t0) : 1;
    if (plain > 0)
        fprintf(out, "  ok    %-36s %4.0f pictures/s  (%3.0f %% of plain)\n", label, fps, fps * 100 / plain);
    else
        fprintf(out, "  ok    %-36s %4.0f pictures/s\n", label, fps);
done:
    if (enc)
        eb->venc->close(enc);
    if (dec)
        db->vdec->close(dec);
    nv_scaler_free(scaler);
    free(res.data);
    return fps;
}

/* ---- resizing on the card, measured ----------------------------------- *
 * A smooth picture (ramps in Y, U and V) is encoded at 1920x1080, decoded
 * on the card and made 1280x720 by each route a channel can take; every
 * sample of the result is compared with what the ramps say it should be.
 * A colour plane read from the wrong place, or turned over, is far off. */

static double ramp(int plane, double x, double y)
{
    /* 0..1 over the picture; x, y in 0..1 */
    return plane == 0 ? 0.15 + 0.7 * x : plane == 1 ? 0.2 + 0.6 * y : 0.8 - 0.6 * x;
}

static void make_smooth(struct vframe *f, uint8_t *mem, int w, int h, int depth, int i)
{
    int bps = depth > 8 ? 2 : 1, cw = w / 2, ch = h / 2;
    uint8_t *pl[3] = { mem, mem + (size_t)w * h * bps, mem + (size_t)w * h * bps + (size_t)cw * ch * bps };
    double full = depth > 8 ? 1023.0 : 255.0;

    memset(f, 0, sizeof(*f));
    f->mem = VMEM_CPU;
    f->width = w;
    f->height = h;
    f->depth = depth;
    f->fps_num = 25;
    f->fps_den = 1;
    f->pts = (stc_t)(i + 250) * (STC_HZ / 25);
    for (int p = 0; p < 3; p++) {
        int pw = p ? cw : w, ph = p ? ch : h;

        f->plane[p] = pl[p];
        f->stride[p] = pw * bps;
        for (int y = 0; y < ph; y++)
            for (int x = 0; x < pw; x++) {
                unsigned v = (unsigned)(ramp(p, (x + 0.5) / pw, (y + 0.5) / ph) * full + 0.5);

                if (bps == 1) {
                    pl[p][(size_t)y * pw + x] = (uint8_t)v;
                } else {
                    pl[p][((size_t)y * pw + x) * 2] = (uint8_t)v;
                    pl[p][((size_t)y * pw + x) * 2 + 1] = (uint8_t)(v >> 8);
                }
            }
    }
}

/* Mean distance (0..1) of a picture on the card from the ramps, worst plane. */
static double ramp_error(const struct vframe *f, char *which, size_t wl)
{
    CudaFunctions *cu = nv_cu();
    int bps = f->depth > 8 ? 2 : 1;
    size_t row = (size_t)f->width * bps;
    uint8_t *buf = malloc(row * (size_t)f->height * 3 / 2);
    CUDA_MEMCPY2D c;
    CUcontext dummy;
    double worst = -1, err[3] = { 0, 0, 0 };
    const char *names[3] = { "Y", "U", "V" };
    int wi = 0;

    if (!buf)
        return 1;
    memset(&c, 0, sizeof(c));
    c.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    c.srcDevice = (CUdeviceptr)(uintptr_t)f->plane[0];
    c.srcPitch = (size_t)f->stride[0];
    c.dstMemoryType = CU_MEMORYTYPE_HOST;
    c.dstHost = buf;
    c.dstPitch = row;
    c.WidthInBytes = row;
    c.Height = (size_t)f->height * 3 / 2;
    if (f->cuda_ctx)
        cu->cuCtxPushCurrent(f->cuda_ctx);
    if (cu->cuMemcpy2D(&c) != CUDA_SUCCESS) {
        if (f->cuda_ctx)
            cu->cuCtxPopCurrent(&dummy);
        free(buf);
        snprintf(which, wl, "could not copy the picture off the card");
        return 1;
    }
    if (f->cuda_ctx)
        cu->cuCtxPopCurrent(&dummy);
    for (int p = 0; p < 3; p++) {
        int pw = p ? f->width / 2 : f->width, ph = p ? f->height / 2 : f->height;
        const uint8_t *base = p ? buf + row * (size_t)f->height : buf;
        double sum = 0;
        long n = 0;

        /* away from the edges, where a scaler may treat borders its own way */
        for (int y = ph / 10; y < ph - ph / 10; y += 3)
            for (int x = pw / 10; x < pw - pw / 10; x += 3) {
                size_t at = (size_t)y * row + ((size_t)x * (p ? 2 : 1) + (p == 2 ? 1 : 0)) * bps;
                double v = bps == 1 ? base[at] / 255.0
                                    : (double)(base[at] | (base[at + 1] << 8)) / 65535.0;

                sum += fabs(v - ramp(p, (x + 0.5) / pw, (y + 0.5) / ph));
                n++;
            }
        err[p] = n ? sum / n : 1;
        if (err[p] > worst) {
            worst = err[p];
            wi = p;
        }
    }
    snprintf(which, wl, "off by Y %.1f %%, U %.1f %%, V %.1f %% (worst %s)", err[0] * 100, err[1] * 100,
             err[2] * 100, names[wi]);
    free(buf);
    return worst;
}

/* One route: `src` decoded on the card (asked for dw x dh, 8-bit only if
 * `only8`), then, if `scale_to_w`, resized by nv_scale_to to that size and
 * `depth_to` bits. Checks the 20th picture. */
static bool scale_route(FILE *out, int gpu, const struct blob *src, enum codec_id codec, int dw, int dh,
                        bool only8, int scale_to_w, int scale_to_h, int depth_to, const char *label)
{
    const struct codec_backend *db = codec_find(CODEC_VIDEO, CODEC_DECODER, codec, WHERE_NVIDIA, 0);
    struct vdec_cfg dc = { .codec = codec, .gpu = gpu, .want_cuda = true, .width = dw, .height = dh,
                           .only_8bit = only8 };
    struct nv_scaler *sc = NULL;
    struct vframe f, g;
    char err[200] = "", which[160] = "";
    void *dec;
    int n = 0;
    bool ok = false, done = false;

    if (!db || !(dec = db->vdec->open(&dc, err, sizeof(err)))) {
        fprintf(out, "  FAIL  %-44s decoder: %s\n", label, err);
        return false;
    }
    for (int i = 0; i <= src->n && !done; i++) {
        if (i < src->n)
            db->vdec->push(dec, src->data + src->off[i], src->size[i], src->pts[i] / STC_PER_90K, NOPTS);
        else
            db->vdec->drain(dec);
        while (!done && db->vdec->pull(dec, &f) == 1) {
            if (++n < 20 && i < src->n)
                continue;
            done = true;
            /* the decoder did not make the size (an interlaced source):
             * resized after, as a channel does (xc_video.c) */
            if (!scale_to_w && dw && (f.width != dw || f.height != dh)) {
                scale_to_w = dw;
                scale_to_h = dh;
                depth_to = f.depth;
            }
            if (scale_to_w) {
                if (!nv_scale_to(&sc, &f, scale_to_w, scale_to_h, depth_to, &g, err, sizeof(err))) {
                    fprintf(out, "  FAIL  %-44s %s\n", label, err);
                    break;
                }
                f = g;
            }
            {
                double e = ramp_error(&f, which, sizeof(which));

                ok = e < 0.03;
                fprintf(out, "  %s  %-44s %dx%d %d-bit, %s\n", ok ? "ok  " : "FAIL", label, f.width, f.height,
                        f.depth, which);
            }
        }
    }
    if (!done)
        fprintf(out, "  FAIL  %-44s no picture came out\n", label);
    nv_scaler_free(sc);
    db->vdec->close(dec);
    return ok;
}

static int scale_test(FILE *out, int gpu)
{
    static const struct { enum codec_id codec; int depth; bool interlaced; } srcs[] = {
        { CODEC_HEVC, 10, false }, { CODEC_HEVC, 8, false }, { CODEC_H264, 8, false },
        { CODEC_H264, 8, true },            /* 1080i, as broadcast HD is */
    };
    char name[128] = "";
    int bad = 0;

    fprintf(out, "\nResizing on the card, measured: a smooth 1920x1080 picture made 1280x720 by each\n"
                 "route, every sample compared with what it should be (under 3 %% off is right)\n");
    if (nv_device_name(gpu, name, sizeof(name), NULL) && strstr(name, "Stand-in")) {
        fprintf(out, "  skipped: the stand-in card's pictures are not real pictures\n");
        return 0;
    }
    for (size_t k = 0; k < sizeof(srcs) / sizeof(srcs[0]); k++) {
        const struct codec_backend *eb = codec_find(CODEC_VIDEO, CODEC_ENCODER, srcs[k].codec, WHERE_NVIDIA, 0);
        uint8_t *mem = malloc((size_t)1920 * 1080 * 3 * 2);
        struct blob src = { 0 };
        struct venc_cfg ec;
        struct vframe f;
        struct vpacket p;
        char err[200] = "", lab[96];
        void *enc;
        int d = srcs[k].depth;
        const char *cn = srcs[k].interlaced ? "H.264i" : srcs[k].codec == CODEC_HEVC ? "H.265" : "H.264";

        fill_cfg(&ec, srcs[k].codec, 1920, 1080, d, gpu, NULL, srcs[k].interlaced);
        ec.bitrate = 20000000;
        if (!eb || !mem || !(enc = eb->venc->open(&ec, err, sizeof(err)))) {
            fprintf(out, "  FAIL  %s %d-bit source: could not encode it: %s\n", cn, d, err);
            free(mem);
            bad++;
            continue;
        }
        for (int i = 0; i < 40; i++) {
            make_smooth(&f, mem, 1920, 1080, d, i);
            f.interlaced = f.tff = srcs[k].interlaced;
            eb->venc->push(enc, &f);
            while (eb->venc->pull(enc, &p) == 1)
                blob_add(&src, &p);
        }
        eb->venc->push(enc, NULL);
        while (eb->venc->pull(enc, &p) == 1)
            blob_add(&src, &p);
        eb->venc->close(enc);
        free(mem);
        snprintf(lab, sizeof(lab), "%s %d-bit: decoded, as it is", cn, d);
        bad += !scale_route(out, gpu, &src, srcs[k].codec, 0, 0, false, 0, 0, 0, lab);
        snprintf(lab, sizeof(lab), srcs[k].interlaced ? "%s %d-bit: as a channel resizes it" : "%s %d-bit: the decoder resizes", cn, d);
        bad += !scale_route(out, gpu, &src, srcs[k].codec, 1280, 720, false, 0, 0, 0, lab);
        snprintf(lab, sizeof(lab), "%s %d-bit: resized after (renditions)", cn, d);
        bad += !scale_route(out, gpu, &src, srcs[k].codec, 0, 0, false, 1280, 720, d, lab);
        if (d > 8) {
            snprintf(lab, sizeof(lab), "%s 10-bit: decoder resizes, makes 8-bit", cn);
            bad += !scale_route(out, gpu, &src, srcs[k].codec, 1280, 720, true, 0, 0, 0, lab);
        } else {
            snprintf(lab, sizeof(lab), "%s 8-bit: resized and made 10-bit", cn);
            bad += !scale_route(out, gpu, &src, srcs[k].codec, 0, 0, false, 1280, 720, 10, lab);
        }
        free(src.data);
    }
    return bad;
}

static int options_test(FILE *out, int gpu)
{
    static const struct {
        const char *label;
        enum codec_id codec;
        struct venc_tune t;
        bool main10;
        int bframes;
    } runs[] = {
        { "H.265 plain (as the preset has it)", CODEC_HEVC, { 0 }, false, -1 },
        { "H.265 spatial AQ, driver's strength", CODEC_HEVC, { .aq = 2 }, false, -1 },
        { "H.265 spatial AQ, strength 8", CODEC_HEVC, { .aq = 2, .aq_strength = 8 }, false, -1 },
        { "H.265 temporal AQ", CODEC_HEVC, { .taq = 2 }, false, -1 },
        { "H.265 look-ahead 20", CODEC_HEVC, { .lookahead = 20 }, false, -1 },
        { "H.265 weighted prediction", CODEC_HEVC, { .wp = 1 }, false, -1 },
        { "H.265 two passes, quarter size", CODEC_HEVC, { .multipass = 2 }, false, -1 },
        { "H.265 two passes, full size", CODEC_HEVC, { .multipass = 3 }, false, -1 },
        { "H.265 Main 10 from 8-bit", CODEC_HEVC, { 0 }, true, -1 },
        { "H.265 AQ + look-ahead 20 + WP", CODEC_HEVC, { .aq = 2, .lookahead = 20, .wp = 1 }, false, -1 },
        { "H.264 plain (B-frames)", CODEC_H264, { 0 }, false, -1 },
        { "H.264 spatial + temporal AQ", CODEC_H264, { .aq = 2, .taq = 2 }, false, -1 },
        { "H.264 look-ahead 20 (B-frames)", CODEC_H264, { .lookahead = 20 }, false, -1 },
        { "H.264 weighted prediction, no B", CODEC_H264, { .wp = 1 }, false, 0 },
    };
    struct blob src = { 0 };
    char err[256] = "";
    double plain[2] = { 0, 0 };
    int bad = 0;

    fprintf(out, "\nPicture-quality options: 1080p H.264 decoded and encoded again on the card,\n"
                 "250 pictures each (one stream on an idle card; for comparing the options)\n");
    fflush(out);
    if (!make_source(gpu, CODEC_H264, 1920, 1080, 250, &src, err, sizeof(err))) {
        fprintf(out, "  FAIL  making the source: %s\n", err);
        free(src.data);
        return 1;
    }
    for (size_t i = 0; i < sizeof(runs) / sizeof(runs[0]); i++) {
        int k = runs[i].codec == CODEC_HEVC ? 1 : 0;
        double fps;

        fflush(out);
        fps = option_run(out, gpu, &src, CODEC_H264, runs[i].codec, &runs[i].t, runs[i].main10,
                         runs[i].bframes, runs[i].label, plain[k]);
        if (fps <= 0)
            bad++;
        else if (plain[k] == 0)
            plain[k] = fps;
    }
    fprintf(out, "  (an option the card does not have is left off, with a warning just above\n"
                 "   its line; its speed is then that of plain)\n");
    free(src.data);
    return bad;
}

int nv_selftest(FILE *out, int gpu)
{
    char why[200] = "", name[128] = "";
    uint32_t ver = 0;
    int bad = 0;

    fprintf(out, "caricoder3 %s: NVIDIA self-test, card %d\n", CC3_VERSION, gpu);
    if (!nv_load(why, sizeof(why))) {
        fprintf(out, "  FAIL  %s\n", why);
        return 1;
    }
    fprintf(out, "  ok    libraries loaded, %d card(s)\n", nv_device_count());
    for (int i = 0; i < nv_device_count(); i++)
        if (nv_device_name(i, name, sizeof(name), NULL))
            fprintf(out, "        card %d: %s%s\n", i, name, i == gpu ? "   <- testing this one" : "");
    if (gpu >= nv_device_count()) {
        fprintf(out, "  FAIL  there is no card %d\n", gpu);
        return 1;
    }
    if (nv_ne()->NvEncodeAPIGetMaxSupportedVersion(&ver) == NV_ENC_SUCCESS)
        fprintf(out, "        the driver offers encode API %u.%u; this program uses %d.%d\n",
                ver >> 4, ver & 0xF, NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);

    bad += !round_trip(out, gpu, CODEC_H264, 8, CODEC_HEVC, 1280, 720, 75, false, false);
    bad += !round_trip(out, gpu, CODEC_HEVC, 8, CODEC_H264, 1280, 720, 75, false, false);
    bad += !round_trip(out, gpu, CODEC_HEVC, 10, CODEC_HEVC, 1280, 720, 75, false, false);
    bad += !round_trip(out, gpu, CODEC_H264, 8, CODEC_H264, 1920, 1080, 250, true, false);
    /* Interlaced H.264, as most broadcast HD and SD is: coded as fields. */
    bad += !round_trip(out, gpu, CODEC_H264, 8, CODEC_H264, 1920, 1080, 50, false, true);
    show_caps(out, gpu);
    bad += scale_test(out, gpu);
    bad += options_test(out, gpu);

    fprintf(out, "\n%s\n", bad ? "RESULT: FAILED (see above)" : "RESULT: all steps passed");
    return bad ? 1 : 0;
}

#else

int nv_selftest(FILE *out, int gpu)
{
    (void)gpu;
    fprintf(out, "This build has no NVIDIA support (built with NVIDIA=0 or CODECS=none).\n");
    return 1;
}

#endif
