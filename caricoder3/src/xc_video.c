/*
 * caricoder3 - video transcoding
 * Copyright (c) 2026 CariTech Solutions
 */
#include <stdlib.h>

#include "xcode.h"

struct vxc {
    int unused;
};

struct vxc *vxc_new(const struct xcode_cfg *cfg, xcode_emit_fn emit, void *opaque,
                    struct xcode_stats *st, char *err, size_t errlen)
{
    (void)cfg; (void)emit; (void)opaque; (void)st;
    snprintf(err, errlen, "video transcoding is not built yet");
    return NULL;
}

void vxc_free(struct vxc *v) { free(v); }
void vxc_unit(struct vxc *v, const struct es_unit *u) { (void)v; (void)u; }
void vxc_finish(struct vxc *v) { (void)v; }
