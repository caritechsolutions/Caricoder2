/*
 * caricoder3 - an Upcom UC-IRD+'s own tables: ird_upcom.h
 * Copyright (c) 2026 CariTech Solutions
 */
#include "ird_upcom.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTION_MAX 1024

/* After `"key":` at the top of the answer's innermost object; NULL if not there. */
static const char *after_key(const char *body, const char *key)
{
    char k[40];
    const char *p;

    snprintf(k, sizeof(k), "\"%s\"", key);
    p = strstr(body, k);
    if (!p)
        return NULL;
    p += strlen(k);
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != ':')
        return NULL;
    p++;
    while (*p && isspace((unsigned char)*p))
        p++;
    return p;
}

static void skip_ws(const char **p)
{
    while (**p && (isspace((unsigned char)**p) || **p == ','))
        (*p)++;
}

/* [n, n, ...] at *p: up to `max` into out; how many (-1: not a list of numbers) */
static int ints(const char **p, int *out, int max)
{
    int n = 0;

    skip_ws(p);
    if (**p != '[')
        return -1;
    (*p)++;
    for (;;) {
        char *end;
        long v;

        skip_ws(p);
        if (**p == ']') {
            (*p)++;
            return n;
        }
        v = strtol(*p, &end, 10);
        if (end == *p)
            return -1;
        if (n < max)
            out[n] = (int)v;
        n++;
        *p = end;
    }
}

static const char *TYPE_NAMES[256] = {
    [1] = "MPEG-1 video", [2] = "MPEG-2 video", [3] = "MPEG audio", [4] = "MPEG audio", [5] = "private sections",
    [6] = "private data", [11] = "DSM-CC", [13] = "DSM-CC", [15] = "AAC", [17] = "AAC LATM", [27] = "H.264",
    [36] = "H.265", [129] = "AC-3", [134] = "SCTE-35", [135] = "E-AC-3",
};

static bool is_audio(int t)
{
    return t == 3 || t == 4 || t == 15 || t == 17 || t == 129 || t == 135;
}

static int canon_type(int t)
{
    return t == 4 ? 3 : t;          /* MPEG-1 and MPEG-2 audio: one */
}

static void parse_pmt(const int *b, int len, struct upcom_svc *s)
{
    int slen, il, i, end;

    memset(s, 0, sizeof(*s));
    if (len < 16 || b[0] != 2)
        return;
    slen = ((b[1] & 15) << 8) | b[2];
    end = 3 + slen - 4;
    if (end > len)
        end = len;
    s->n = (b[3] << 8) | b[4];
    il = ((b[10] & 15) << 8) | b[11];
    for (i = 12 + il; i + 5 <= end && s->ns < UPCOM_ES_MAX;) {
        int l = ((b[i + 3] & 15) << 8) | b[i + 4];

        s->s[s->ns].type = b[i];
        s->s[s->ns].pid = ((b[i + 1] & 31) << 8) | b[i + 2];
        for (int j = i + 5; j + 1 < i + 5 + l && j + 1 < end; j += 2 + b[j + 1]) {
            if (b[j] == 0x0a && b[j + 1] >= 3 && j + 4 < end) {
                for (int c = 0; c < 3; c++)
                    s->s[s->ns].lang[c] = (char)(isprint(b[j + 2 + c]) ? tolower(b[j + 2 + c]) : '?');
                s->s[s->ns].lang[3] = 0;
            }
        }
        s->ns++;
        i += 5 + l;
    }
}

static void parse_sdt(const int *b, int len, struct upcom_tables *t)
{
    int slen, i, end;

    if (len < 15 || (b[0] != 0x42 && b[0] != 0x46))
        return;
    slen = ((b[1] & 15) << 8) | b[2];
    end = 3 + slen - 4;
    if (end > len)
        end = len;
    for (i = 11; i + 5 <= end;) {
        int sid = (b[i] << 8) | b[i + 1], dl = ((b[i + 3] & 15) << 8) | b[i + 4];

        for (int j = i + 5; j + 1 < i + 5 + dl && j + 1 < end; j += 2 + b[j + 1]) {
            if (b[j] == 0x48 && j + 3 < end) {
                int pl = b[j + 3], nl = j + 4 + pl < end ? b[j + 4 + pl] : 0, at = j + 5 + pl, o = 0;
                struct upcom_svc *s = NULL;

                for (int k = 0; k < t->n; k++)
                    if (t->svc[k].n == sid)
                        s = &t->svc[k];
                if (!s)
                    continue;
                for (int c = 0; c < nl && at + c < end && o + 1 < (int)sizeof(s->name); c++)
                    if (b[at + c] >= 32 && b[at + c] < 127)
                        s->name[o++] = (char)b[at + c];         /* the character table byte and the like: left out */
                s->name[o] = 0;
            }
        }
        i += 5 + dl;
    }
}

bool upcom_parse(const char *body, struct upcom_tables *t)
{
    static int buf[SECTION_MAX];
    const char *p;
    int n;

    memset(t, 0, sizeof(*t));
    if (!body || !after_key(body, "PAT"))
        return false;
    if ((p = after_key(body, "PMT"))) {
        skip_ws(&p);
        if (*p == '[') {
            p++;
            for (;;) {
                skip_ws(&p);
                if (*p != '[')
                    break;
                n = ints(&p, buf, SECTION_MAX);
                if (n < 0)
                    break;
                if (t->n < UPCOM_SVC_MAX) {
                    parse_pmt(buf, n < SECTION_MAX ? n : SECTION_MAX, &t->svc[t->n]);
                    if (t->svc[t->n].n)
                        t->n++;
                }
            }
        }
    }
    if ((p = after_key(body, "SDT"))) {
        skip_ws(&p);
        if (*p == '[') {
            p++;
            skip_ws(&p);
            while (*p == '[') {
                n = ints(&p, buf, SECTION_MAX);
                if (n < 0)
                    break;
                parse_sdt(buf, n < SECTION_MAX ? n : SECTION_MAX, t);
                skip_ws(&p);
            }
        }
    }
    if ((p = after_key(body, "ServiceIdTable"))) {
        skip_ws(&p);
        if (*p == '[') {
            int row[4];

            p++;
            for (skip_ws(&p); *p == '[' && t->n_src < UPCOM_MAP_MAX; skip_ws(&p))
                if (ints(&p, row, 4) >= 2) {
                    t->src[t->n_src][0] = row[0];
                    t->src[t->n_src][1] = row[1];
                    t->n_src++;
                } else {
                    break;
                }
        }
    }
    if ((p = after_key(body, "PidTable"))) {
        skip_ws(&p);
        if (*p == '[') {
            int row[4];

            p++;
            for (skip_ws(&p); *p == '[' && t->n_pid < UPCOM_MAP_MAX; skip_ws(&p))
                if (ints(&p, row, 4) == 4) {
                    memcpy(t->pid[t->n_pid], row, sizeof(row));
                    t->n_pid++;
                } else {
                    break;
                }
        }
    }
    return true;
}

const struct upcom_svc *upcom_find(const struct upcom_tables *t, int n)
{
    for (int i = 0; i < t->n; i++)
        if (t->svc[i].n == n)
            return &t->svc[i];
    return NULL;
}

int upcom_source_of(const struct upcom_tables *remux, int n)
{
    for (int i = 0; i < remux->n_src; i++)
        if (remux->src[i][1] == n)
            return remux->src[i][0];
    return -1;
}

static int map_pid(const struct upcom_tables *remux, int source, int program, int pid)
{
    for (int i = 0; i < remux->n_pid; i++)
        if (remux->pid[i][0] == source && (remux->pid[i][1] == program || remux->pid[i][1] == 0) &&
            remux->pid[i][2] == pid)
            return remux->pid[i][3];
    return pid;
}

void upcom_streams_text(const struct upcom_svc *s, char *out, size_t len)
{
    size_t at = 0;

    out[0] = 0;
    for (int i = 0; i < s->ns && at + 30 < len; i++) {
        const char *tn = TYPE_NAMES[s->s[i].type & 255];
        char tb[16];

        if (!tn) {
            snprintf(tb, sizeof(tb), "type %d", s->s[i].type);
            tn = tb;
        }
        at += (size_t)snprintf(out + at, len - at, "%s%d %s%s%s", i ? ", " : "", s->s[i].pid, tn,
                               s->s[i].lang[0] ? " " : "", s->s[i].lang);
    }
    if (!s->ns)
        snprintf(out, len, "no streams");
}

bool upcom_same(const struct upcom_tables *remux, const struct upcom_svc *out, int source,
                const struct upcom_svc *in, char *what, size_t len)
{
    char a[400], b[400];
    bool same = out->ns == in->ns;

    for (int i = 0; same && i < in->ns; i++) {
        int want = map_pid(remux, source, in->n, in->s[i].pid);
        bool found = false;

        for (int j = 0; j < out->ns; j++)
            if (out->s[j].pid == want && canon_type(out->s[j].type) == canon_type(in->s[i].type))
                found = true;
        same = found;
    }
    if (!same) {
        upcom_streams_text(out, a, sizeof(a));
        upcom_streams_text(in, b, sizeof(b));
        snprintf(what, len, "remux %s; satellite %s", a, b);
    }
    return same;
}

bool upcom_labels_differ(const struct upcom_svc *out, const struct upcom_svc *in, char *what, size_t len)
{
    char a[200] = "", b[200] = "";
    size_t aa = 0, bb = 0;
    bool differ = false;

    for (int i = 0; i < out->ns; i++) {
        if (!is_audio(out->s[i].type))
            continue;
        for (int j = 0; j < in->ns; j++)
            if (in->s[j].pid == out->s[i].pid && strcmp(in->s[j].lang, out->s[i].lang))
                differ = true;
    }
    if (!differ)
        return false;
    for (int i = 0; i < out->ns; i++)
        if (is_audio(out->s[i].type) && aa + 20 < sizeof(a))
            aa += (size_t)snprintf(a + aa, sizeof(a) - aa, "%s%d %s", aa ? " / " : "", out->s[i].pid,
                                   out->s[i].lang[0] ? out->s[i].lang : "(none)");
    for (int i = 0; i < in->ns; i++)
        if (is_audio(in->s[i].type) && bb + 20 < sizeof(b))
            bb += (size_t)snprintf(b + bb, sizeof(b) - bb, "%s%d %s", bb ? " / " : "", in->s[i].pid,
                                   in->s[i].lang[0] ? in->s[i].lang : "(none)");
    snprintf(what, len, "labelled %s in the remux, %s on the satellite", a, b);
    return true;
}

bool upcom_changed(const struct upcom_svc *was, const struct upcom_svc *now, char *what, size_t len)
{
    char a[400], b[400];
    bool same = was->ns == now->ns;

    for (int i = 0; same && i < was->ns; i++)
        same = was->s[i].pid == now->s[i].pid && was->s[i].type == now->s[i].type &&
               !strcmp(was->s[i].lang, now->s[i].lang);
    if (same)
        return false;
    upcom_streams_text(was, a, sizeof(a));
    upcom_streams_text(now, b, sizeof(b));
    snprintf(what, len, "was %s; now %s", a, b);
    return true;
}
