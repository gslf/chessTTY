/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef JSON_H
#define JSON_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define JSON_TOKENS 512
typedef struct { int start, end, next; char type; } JsonToken;
typedef struct { const char *s; JsonToken t[JSON_TOKENS]; int count; } Json;
bool json_parse(Json *j, const char *text);
int json_get(const Json *j, int object, const char *key);
bool json_string(const Json *j, int token, char *out, size_t size);
int64_t json_number(const Json *j, int token, int64_t fallback);
bool json_is(const Json *j, int token, const char *literal);
#endif
