/*
 * caricoder3 - codec backends
 * Copyright (c) 2026 CariTech Solutions
 */
#include "codec.h"

/* Backends register themselves here as they are written, each guarded by
 * the HAVE_xxx macro the Makefile sets when it finds the library. */
static const struct codec_backend *const backends[] = {
    NULL
};

const struct codec_backend *const *codec_backends(int *count)
{
    int n = 0;

    while (backends[n])
        n++;
    *count = n;
    return backends;
}

void codec_list(FILE *f)
{
    int n;
    const struct codec_backend *const *list = codec_backends(&n);

    if (n == 0) {
        fprintf(f, "No codec backends are built in (stage 1: every stream is copied).\n");
        return;
    }
    for (int i = 0; i < n; i++) {
        char why[128] = "";
        bool ok = list[i]->probe ? list[i]->probe(why, sizeof(why)) : true;

        fprintf(f, "%-10s %-7s %-7s %-16s %s%s%s\n", list[i]->name,
                list[i]->media == CODEC_VIDEO ? "video" : "audio",
                list[i]->role == CODEC_DECODER ? "decode" : "encode",
                list[i]->codecs, ok ? "available" : "unavailable",
                why[0] ? ": " : "", why);
    }
}
