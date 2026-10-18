/*
 * caricoder3 - the link inputs that are compiled in
 * Copyright (c) 2026 CariTech Solutions
 */
#include <string.h>

#include "input.h"

#ifdef HAVE_SRT
extern const struct input_ops input_srt_ops;
#endif
#ifdef HAVE_RIST
extern const struct input_ops input_rist_ops;
#endif

extern const struct input_ops input_http_ops;

static const struct input_ops *const link_inputs[] = {
    &input_http_ops,
#ifdef HAVE_SRT
    &input_srt_ops,
#endif
#ifdef HAVE_RIST
    &input_rist_ops,
#endif
    NULL
};

const struct input_ops *input_ops_for(const char *url)
{
    for (int i = 0; link_inputs[i]; i++)
        if (link_inputs[i]->takes(url))
            return link_inputs[i];
    return NULL;
}

bool input_is_live(const char *url)
{
    return !strncmp(url, "udp://", 6) || !strncmp(url, "rtp://", 6) ||
           !strncmp(url, "srt://", 6) || !strncmp(url, "rist://", 7) || !strncmp(url, "http://", 7);
}

const char *input_transports(void)
{
    return "udp, rtp, http"
#ifdef HAVE_SRT
           ", srt"
#endif
#ifdef HAVE_RIST
           ", rist"
#endif
           ;
}
