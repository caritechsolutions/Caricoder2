/*
 * caricoder3 - codec backends: the registry
 * Copyright (c) 2026 CariTech Solutions
 */
#include "codec.h"

#include <sched.h>
#include <string.h>

/* Each backend lives in its own file and exists only when the Makefile
 * found its library. */
#ifdef HAVE_FDK_AAC
extern const struct codec_backend backend_fdk_dec, backend_fdk_enc;
#endif
#ifdef HAVE_MPG123
extern const struct codec_backend backend_mpg123;
#endif
#ifdef HAVE_TWOLAME
extern const struct codec_backend backend_twolame;
#endif
#ifdef HAVE_LAME
extern const struct codec_backend backend_lame;
#endif
#ifdef HAVE_A52
extern const struct codec_backend backend_a52;
#endif
#ifdef HAVE_AVCODEC
extern const struct codec_backend backend_avcodec;
#endif
#ifdef HAVE_X264
extern const struct codec_backend backend_x264;
#endif
#ifdef HAVE_X265
extern const struct codec_backend backend_x265;
#endif
#ifdef HAVE_NVIDIA
extern const struct codec_backend backend_nvdec, backend_nvenc;
#endif

static const struct codec_backend *const backends[] = {
#ifdef HAVE_NVIDIA
    &backend_nvdec,
    &backend_nvenc,
#endif
#ifdef HAVE_AVCODEC
    &backend_avcodec,
#endif
#ifdef HAVE_X264
    &backend_x264,
#endif
#ifdef HAVE_X265
    &backend_x265,
#endif
#ifdef HAVE_FDK_AAC
    &backend_fdk_dec,
    &backend_fdk_enc,
#endif
#ifdef HAVE_MPG123
    &backend_mpg123,
#endif
#ifdef HAVE_A52
    &backend_a52,
#endif
#ifdef HAVE_TWOLAME
    &backend_twolame,
#endif
#ifdef HAVE_LAME
    &backend_lame,
#endif
    NULL
};

int codec_threads(int most)
{
    cpu_set_t set;
    int n = 1;

    /* The processors this process may run on, which is what counts when
     * it has been confined to some of them. */
    if (sched_getaffinity(0, sizeof(set), &set) == 0)
        n = CPU_COUNT(&set);
    if (n < 1)
        n = 1;
    return n < most ? n : most;
}

bool codec_es_has_key(enum codec_id codec, const uint8_t *d, size_t len, bool or_params)
{
    for (size_t i = 0; i + 4 < len; i++) {
        unsigned type;

        if (d[i] != 0 || d[i + 1] != 0 || d[i + 2] != 1)
            continue;
        if (codec == CODEC_MPEG2) {
            /* a sequence header, or a picture header saying I-picture */
            if (or_params && d[i + 3] == 0xB3)
                return true;
            if (d[i + 3] == 0x00 && i + 5 < len && ((d[i + 5] >> 3) & 7) == 1)
                return true;
        } else if (codec == CODEC_H264) {
            type = d[i + 3] & 0x1F;
            if (type == 5 || (or_params && type == 7))
                return true;
        } else {
            type = (d[i + 3] >> 1) & 0x3F;
            if ((type >= 16 && type <= 21) || (or_params && (type == 32 || type == 33)))
                return true;
        }
        i += 2;
    }
    return false;
}

const char *codec_name(enum codec_id id)
{
    switch (id) {
    case CODEC_MPA:      return "mpeg-audio";
    case CODEC_MP2:      return "mp2";
    case CODEC_MP3:      return "mp3";
    case CODEC_AAC:      return "aac";
    case CODEC_AAC_LATM: return "aac-latm";
    case CODEC_AC3:      return "ac3";
    case CODEC_H264:     return "h264";
    case CODEC_HEVC:     return "hevc";
    case CODEC_MPEG2:    return "mpeg2";
    default:             return "none";
    }
}

bool codec_is_video(enum codec_id id)
{
    return id == CODEC_H264 || id == CODEC_HEVC || id == CODEC_MPEG2;
}

const struct codec_backend *const *codec_backends(int *count)
{
    int n = 0;

    while (backends[n])
        n++;
    *count = n;
    return backends;
}

const struct codec_backend *codec_find(enum codec_media media, enum codec_role role,
                                       enum codec_id codec, enum codec_where where,
                                       int skip)
{
    const struct codec_backend *cand[16];
    int n, m = 0;
    const struct codec_backend *const *list = codec_backends(&n);

    for (int i = 0; i < n && m < 16; i++) {
        const struct codec_backend *b = list[i];
        int k;

        if (b->media != media || b->role != role || !b->handles(codec))
            continue;
        if (where != WHERE_AUTO && b->where != where)
            continue;
        if (b->probe && !b->probe(NULL, 0))
            continue;
        /* insertion sort, highest priority first, table order among equals */
        for (k = m; k > 0 && cand[k - 1]->priority < b->priority; k--)
            cand[k] = cand[k - 1];
        cand[k] = b;
        m++;
    }
    return skip < m ? cand[skip] : NULL;
}

void codec_list(FILE *f)
{
    int n;
    const struct codec_backend *const *list = codec_backends(&n);

    if (n == 0) {
        fprintf(f, "No codec backends are built in: every stream is copied.\n"
                   "Install the codec libraries and build again (see README).\n");
        return;
    }
    for (int i = 0; i < n; i++) {
        char why[160] = "";
        bool ok = list[i]->probe ? list[i]->probe(why, sizeof(why)) : true;

        char codecs[80];

        snprintf(codecs, sizeof(codecs), "%s%s", list[i]->codecs,
                 list[i]->detail ? list[i]->detail() : "");
        fprintf(f, "%-10s %-6s %-7s %-7s %-28s %s%s%s\n", list[i]->name,
                list[i]->media == CODEC_VIDEO ? "video" : "audio",
                list[i]->role == CODEC_DECODER ? "decode" : "encode",
                list[i]->where == WHERE_NVIDIA ? "nvidia" : "cpu",
                codecs, ok ? "available" : "unavailable",
                why[0] ? ": " : "", why);
    }
}
