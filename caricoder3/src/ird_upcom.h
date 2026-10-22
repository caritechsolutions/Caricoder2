/*
 * caricoder3 - an Upcom UC-IRD+'s own tables, from its web interface
 * Copyright (c) 2026 CariTech Solutions
 *
 * The IRD's remux page asks /goform/remux.cgi for the PSI it holds, as
 * raw sections (lists of bytes): request 3 for an input source (the tuner
 * and its CI card paths, by source number), request 8 for the remux,
 * which also says which source each of its services comes from
 * (ServiceIdTable) and any PID remapping (PidTable). Read every minute,
 * these say without probing anything whether the remux still carries each
 * service as the satellite sends it, and when the satellite changes one.
 *
 * Kept apart from ird.c so that another kind of IRD can have its own.
 */
#ifndef CC3_IRD_UPCOM_H
#define CC3_IRD_UPCOM_H

#include <stdbool.h>
#include <stddef.h>

#define UPCOM_SVC_MAX 80
#define UPCOM_ES_MAX 16
#define UPCOM_MAP_MAX 256

struct upcom_svc {
    int n;                      /* program number */
    char name[48];              /* from the SDT, if there is one */
    int ns;
    struct { int pid, type; char lang[4]; } s[UPCOM_ES_MAX];
};

struct upcom_tables {
    int n;
    struct upcom_svc svc[UPCOM_SVC_MAX];
    /* the remux's only: where each service comes from, and PID remapping */
    int n_src;
    int src[UPCOM_MAP_MAX][2];  /* {source, program} */
    int n_pid;
    int pid[UPCOM_MAP_MAX][4];  /* {source, program (0: any), PID in, PID out} */
};

/* One answer of remux.cgi (request 3 or 8): false if it has no PAT (an
 * input with nothing on it). */
bool upcom_parse(const char *body, struct upcom_tables *t);

const struct upcom_svc *upcom_find(const struct upcom_tables *t, int n);
/* The source the remux takes service `n` from; -1 when not said. */
int upcom_source_of(const struct upcom_tables *remux, int n);

/* A service of the remux against the same one from its source: true when
 * they carry the same streams (PIDs through the remux's remapping, and
 * stream types, MPEG-1 and MPEG-2 audio taken as one). `what` says how
 * they differ. Language labels are not compared here (see upcom_labels). */
bool upcom_same(const struct upcom_tables *remux, const struct upcom_svc *out, int source,
                const struct upcom_svc *in, char *what, size_t len);
/* Their audio language labels differ: says how ("2162 eng / 2163 spa
 * where the source has 2162 spa / 2163 eng"). */
bool upcom_labels_differ(const struct upcom_svc *out, const struct upcom_svc *in, char *what, size_t len);
/* A service as it was and as it is now, on one source: false when
 * unchanged; `what` says what changed (streams or labels). */
bool upcom_changed(const struct upcom_svc *was, const struct upcom_svc *now, char *what, size_t len);

/* "2161 H.265, 2162 AAC spa, ..." */
void upcom_streams_text(const struct upcom_svc *s, char *out, size_t len);

#endif
