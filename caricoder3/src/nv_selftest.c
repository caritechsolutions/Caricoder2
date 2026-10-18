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
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "codec.h"
#include "log.h"

#ifdef HAVE_NVIDIA
#include "nv_common.h"

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
