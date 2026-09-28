#pragma once

#include <stdbool.h>

/* JSON utilities for parsing small request bodies. */

#define JSON_STRING_RESULT_MAX_LENGTH 128

typedef struct {
    bool ok;
    char value[JSON_STRING_RESULT_MAX_LENGTH];
} JsonStringResult;

typedef struct {
    bool ok;
    float value;
} JsonFloatResult;

typedef struct {
    bool ok;
    int value;
} JsonIntResult;

JsonStringResult json_get_string(const char *json, const char *key);

JsonFloatResult json_get_float(const char *json, const char *key);

JsonIntResult json_get_int(const char *json, const char *key);
