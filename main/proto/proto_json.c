/* Proto — proto_json.c — minimal JSON value extraction and response building.
 *
 * The protocol request payloads are small and flat, so a full JSON parser is
 * overkill.  These getters locate a top-level-ish key by name (keys are unique
 * per request) and parse the following scalar.
 */
#include "proto_internal.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *find_value_start(const char *json, const char *key) {
    if (json == NULL || key == NULL) {
        return NULL;
    }

    char pattern[strlen(key) + 3];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *p = strstr(json, pattern);
    if (p == NULL) {
        return NULL;
    }

    const char *colon = strchr(p, ':');
    if (colon == NULL) {
        return NULL;
    }
    colon++;

    while (*colon == ' ' || *colon == '\t' || *colon == '\n' || *colon == '\r') {
        colon++;
    }
    return colon;
}

bool proto_json_has_key(const char *json, const char *key) {
    return find_value_start(json, key) != NULL;
}

bool proto_json_get_string(const char *json, const char *key, char *out, size_t cap) {
    if (out == NULL || cap == 0) {
        return false;
    }

    const char *v = find_value_start(json, key);
    if (v == NULL || *v != '"') {
        return false;
    }
    v++;

    size_t i = 0;
    while (*v != '\0' && *v != '"' && i < cap - 1) {
        out[i++] = *v++;
    }
    out[i] = '\0';
    return *v == '"';
}

static bool scalar_tail_is_valid(const char *end) {
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') {
        end++;
    }
    return *end == '\0' || *end == ',' || *end == '}' || *end == ']';
}

bool proto_json_get_float(const char *json, const char *key, float *out) {
    if (out == NULL) {
        return false;
    }

    const char *v = find_value_start(json, key);
    if (v == NULL) {
        return false;
    }

    char *end = NULL;
    float f = strtof(v, &end);
    if (end == v || !isfinite(f) || !scalar_tail_is_valid(end)) {
        return false;
    }
    *out = f;
    return true;
}

bool proto_json_get_int(const char *json, const char *key, int *out) {
    if (out == NULL) {
        return false;
    }

    const char *v = find_value_start(json, key);
    if (v == NULL) {
        return false;
    }

    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v || !scalar_tail_is_valid(end)) {
        return false;
    }
    *out = (int) n;
    return true;
}

bool proto_json_get_u32(const char *json, const char *key, uint32_t *out) {
    if (out == NULL) {
        return false;
    }

    const char *v = find_value_start(json, key);
    if (v == NULL) {
        return false;
    }

    char *end = NULL;
    unsigned long n = strtoul(v, &end, 10);
    if (end == v || !scalar_tail_is_valid(end)) {
        return false;
    }
    *out = (uint32_t) n;
    return true;
}

bool proto_buf_append(char *buf, size_t cap, size_t *len, const char *fmt, ...) {
    if (buf == NULL || len == NULL || *len >= cap) {
        return false;
    }

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *len, cap - *len, fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t) n >= cap - *len) {
        return false;   /* truncated */
    }
    *len += (size_t) n;
    return true;
}
