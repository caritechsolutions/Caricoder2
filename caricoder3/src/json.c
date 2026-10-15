/*
 * caricoder3 - a small JSON reader
 * Copyright (c) 2026 CariTech Solutions
 */
#include "json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct parser {
    struct json_doc *d;
    char *p;                    /* read position in d->text */
    char *err;
    size_t errlen;
};

static int fail(struct parser *ps, const char *what)
{
    snprintf(ps->err, ps->errlen, "JSON: %s at byte %ld", what, (long)(ps->p - ps->d->text));
    return -1;
}

static void skip_ws(struct parser *ps)
{
    while (*ps->p && isspace((unsigned char)*ps->p))
        ps->p++;
}

static int new_node(struct parser *ps, enum json_type t)
{
    struct json_node *n;

    if (ps->d->n == JSON_NODES_MAX)
        return fail(ps, "too many values");
    n = &ps->d->node[ps->d->n];
    memset(n, 0, sizeof(*n));
    n->type = t;
    n->first_child = n->next = -1;
    return ps->d->n++;
}

/* Unescapes a string in place; returns its start, leaves p after the
 * closing quote. */
static char *parse_string(struct parser *ps)
{
    char *out, *start;

    if (*ps->p != '"') {
        fail(ps, "string expected");
        return NULL;
    }
    ps->p++;
    start = out = ps->p;
    while (*ps->p && *ps->p != '"') {
        if (*ps->p == '\\') {
            ps->p++;
            switch (*ps->p) {
            case '"': case '\\': case '/': *out++ = *ps->p; break;
            case 'n': *out++ = '\n'; break;
            case 't': *out++ = '\t'; break;
            case 'r': *out++ = '\r'; break;
            case 'b': *out++ = '\b'; break;
            case 'f': *out++ = '\f'; break;
            case 'u': {
                unsigned v = 0;

                for (int i = 1; i <= 4; i++) {
                    char c = ps->p[i];

                    if (!isxdigit((unsigned char)c)) {
                        fail(ps, "bad \\u escape");
                        return NULL;
                    }
                    v = v * 16 + (unsigned)(isdigit((unsigned char)c) ? c - '0'
                                            : (tolower((unsigned char)c) - 'a' + 10));
                }
                ps->p += 4;
                if (v < 0x80) {
                    *out++ = (char)v;
                } else if (v < 0x800) {
                    *out++ = (char)(0xC0 | (v >> 6));
                    *out++ = (char)(0x80 | (v & 0x3F));
                } else {
                    *out++ = (char)(0xE0 | (v >> 12));
                    *out++ = (char)(0x80 | ((v >> 6) & 0x3F));
                    *out++ = (char)(0x80 | (v & 0x3F));
                }
                break;
            }
            default:
                fail(ps, "bad escape");
                return NULL;
            }
            ps->p++;
        } else {
            *out++ = *ps->p++;
        }
    }
    if (*ps->p != '"') {
        fail(ps, "unterminated string");
        return NULL;
    }
    ps->p++;
    *out = 0;
    return start;
}

static int parse_value(struct parser *ps);

static int parse_object(struct parser *ps)
{
    int obj = new_node(ps, JSON_OBJECT), last = -1;

    if (obj < 0)
        return -1;
    ps->p++;
    skip_ws(ps);
    if (*ps->p == '}') {
        ps->p++;
        return obj;
    }
    for (;;) {
        char *key;
        int v;

        skip_ws(ps);
        key = parse_string(ps);
        if (!key)
            return -1;
        skip_ws(ps);
        if (*ps->p != ':')
            return fail(ps, "':' expected");
        ps->p++;
        skip_ws(ps);
        v = parse_value(ps);
        if (v < 0)
            return -1;
        ps->d->node[v].key = key;
        if (last < 0)
            ps->d->node[obj].first_child = v;
        else
            ps->d->node[last].next = v;
        last = v;
        skip_ws(ps);
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == '}') {
            ps->p++;
            return obj;
        }
        return fail(ps, "',' or '}' expected");
    }
}

static int parse_array(struct parser *ps)
{
    int arr = new_node(ps, JSON_ARRAY), last = -1;

    if (arr < 0)
        return -1;
    ps->p++;
    skip_ws(ps);
    if (*ps->p == ']') {
        ps->p++;
        return arr;
    }
    for (;;) {
        int v;

        skip_ws(ps);
        v = parse_value(ps);
        if (v < 0)
            return -1;
        if (last < 0)
            ps->d->node[arr].first_child = v;
        else
            ps->d->node[last].next = v;
        last = v;
        skip_ws(ps);
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == ']') {
            ps->p++;
            return arr;
        }
        return fail(ps, "',' or ']' expected");
    }
}

static int parse_value(struct parser *ps)
{
    int n;

    skip_ws(ps);
    switch (*ps->p) {
    case '{': return parse_object(ps);
    case '[': return parse_array(ps);
    case '"': {
        char *s = parse_string(ps);

        if (!s)
            return -1;
        n = new_node(ps, JSON_STRING);
        if (n >= 0)
            ps->d->node[n].str = s;
        return n;
    }
    case 't': case 'f':
        if (!strncmp(ps->p, "true", 4) || !strncmp(ps->p, "false", 5)) {
            n = new_node(ps, JSON_BOOL);
            if (n >= 0)
                ps->d->node[n].boolean = *ps->p == 't';
            ps->p += *ps->p == 't' ? 4 : 5;
            return n;
        }
        return fail(ps, "bad value");
    case 'n':
        if (!strncmp(ps->p, "null", 4)) {
            ps->p += 4;
            return new_node(ps, JSON_NULL);
        }
        return fail(ps, "bad value");
    default: {
        char *end;
        double v = strtod(ps->p, &end);

        if (end == ps->p)
            return fail(ps, "value expected");
        n = new_node(ps, JSON_NUMBER);
        if (n >= 0)
            ps->d->node[n].num = v;
        ps->p = end;
        return n;
    }
    }
}

int json_parse(struct json_doc *d, const char *text, char *err, size_t errlen)
{
    struct parser ps = { d, d->text, err, errlen };
    int root;

    if (strlen(text) >= JSON_TEXT_MAX) {
        snprintf(err, errlen, "JSON: the text is too long");
        return -1;
    }
    strcpy(d->text, text);
    d->n = 0;
    root = parse_value(&ps);
    if (root < 0)
        return -1;
    skip_ws(&ps);
    if (*ps.p)
        return fail(&ps, "trailing text");
    return root;
}

int json_get(const struct json_doc *d, int obj, const char *key)
{
    if (obj < 0 || obj >= d->n || d->node[obj].type != JSON_OBJECT)
        return -1;
    for (int i = d->node[obj].first_child; i >= 0; i = d->node[i].next)
        if (d->node[i].key && !strcmp(d->node[i].key, key))
            return i;
    return -1;
}

const char *json_get_str(const struct json_doc *d, int obj, const char *key, const char *dflt)
{
    int i = json_get(d, obj, key);

    return i >= 0 && d->node[i].type == JSON_STRING ? d->node[i].str : dflt;
}

double json_get_num(const struct json_doc *d, int obj, const char *key, double dflt)
{
    int i = json_get(d, obj, key);

    return i >= 0 && d->node[i].type == JSON_NUMBER ? d->node[i].num : dflt;
}

bool json_get_bool(const struct json_doc *d, int obj, const char *key, bool dflt)
{
    int i = json_get(d, obj, key);

    return i >= 0 && d->node[i].type == JSON_BOOL ? d->node[i].boolean : dflt;
}

char *json_quote(const char *s, char *buf, size_t len)
{
    size_t o = 0;

    if (len < 3)
        return buf;
    buf[o++] = '"';
    for (; *s && o + 7 < len; s++) {
        if (*s == '"' || *s == '\\') {
            buf[o++] = '\\';
            buf[o++] = *s;
        } else if ((unsigned char)*s < 0x20) {
            o += (size_t)snprintf(buf + o, len - o, "\\u%04x", (unsigned char)*s);
        } else {
            buf[o++] = *s;
        }
    }
    buf[o++] = '"';
    buf[o] = 0;
    return buf;
}
