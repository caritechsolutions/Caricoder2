/*
 * caricoder3 - a small JSON reader for the control socket and the config
 * file
 * Copyright (c) 2026 CariTech Solutions
 *
 * Parses one document into a tree of nodes held in a fixed arena (no
 * allocation per node). Strings are unescaped in place into the arena.
 * Enough for flat requests and small config files; not a general
 * library.
 */
#ifndef CC3_JSON_H
#define CC3_JSON_H

#include <stdbool.h>
#include <stddef.h>

enum json_type { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT };

struct json_node {
    enum json_type type;
    const char *key;            /* when a member of an object */
    const char *str;            /* string value */
    double num;
    bool boolean;
    int first_child, next;      /* indexes into the arena, -1 for none */
};

#define JSON_NODES_MAX 256
#define JSON_TEXT_MAX  16384

struct json_doc {
    struct json_node node[JSON_NODES_MAX];
    int n;
    char text[JSON_TEXT_MAX];   /* copy of the input, unescaped in place */
};

/* Parses `text` (any length up to JSON_TEXT_MAX - 1). Returns the root
 * index (0) or -1 with a reason. */
int json_parse(struct json_doc *d, const char *text, char *err, size_t errlen);

/* Member `key` of object `obj`, or -1. */
int json_get(const struct json_doc *d, int obj, const char *key);
const char *json_get_str(const struct json_doc *d, int obj, const char *key, const char *dflt);
double json_get_num(const struct json_doc *d, int obj, const char *key, double dflt);
bool json_get_bool(const struct json_doc *d, int obj, const char *key, bool dflt);

/* Writes `s` as a JSON string literal into buf. */
char *json_quote(const char *s, char *buf, size_t len);

#endif
