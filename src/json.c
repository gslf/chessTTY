/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "json.h"
#include <ctype.h>
#include <limits.h>
#include <string.h>
static void whitespace(const char **s) { while (**s && isspace((unsigned char)**s)) (*s)++; }
static int value(Json *j, const char **p, int depth) {
    whitespace(p);
    if (depth > 32 || j->count >= JSON_TOKENS || !**p) return -1;
    int index = j->count++;
    JsonToken *t = &j->t[index];
    t->start = (int)(*p - j->s); t->type = **p;
    if (**p == '"') {
        (*p)++;
        while (**p && **p != '"') {
            if ((unsigned char)**p < 32) return -1;
            if (**p == '\\') {
                (*p)++;
                if (**p == 'u') {
                    for (int i = 0; i < 4; i++) { (*p)++; if (!isxdigit((unsigned char)**p)) return -1; }
                } else if (!**p || !strchr("\"\\/bfnrt", **p)) return -1;
            }
            (*p)++;
        }
        if (**p != '"') return -1;
        (*p)++;
    } else if (**p == '{' || **p == '[') {
        char close = **p == '{' ? '}' : ']';
        (*p)++; whitespace(p);
        if (**p != close) for (;;) {
            if (close == '}') {
                if (**p != '"' || value(j, p, depth + 1) < 0) return -1;
                whitespace(p); if (**p != ':') return -1; (*p)++;
            }
            if (value(j, p, depth + 1) < 0) return -1;
            whitespace(p);
            if (**p == close) break;
            if (**p != ',') return -1;
            (*p)++; whitespace(p);
        }
        (*p)++;
    } else {
        const char *start = *p;
        while (**p && !strchr(",]} \n\r\t", **p)) (*p)++;
        size_t n = (size_t)(*p - start);
        bool literal = (n == 4 && (!memcmp(start, "true", 4) || !memcmp(start, "null", 4))) ||
                       (n == 5 && !memcmp(start, "false", 5));
        if (!literal) {
            const char *s = start;
            if (*s == '-') s++;
            if (*s == '0') s++; else { if (*s < '1' || *s > '9') return -1; while (*s >= '0' && *s <= '9') s++; }
            if (*s == '.') { s++; if (*s < '0' || *s > '9') return -1; while (*s >= '0' && *s <= '9') s++; }
            if (*s == 'e' || *s == 'E') { s++; if (*s == '+' || *s == '-') s++; if (*s < '0' || *s > '9') return -1; while (*s >= '0' && *s <= '9') s++; }
            if (s != *p) return -1;
        }
    }
    t->end = (int)(*p - j->s); t->next = j->count;
    return index;
}
bool json_parse(Json *j, const char *text) {
    j->s = text; j->count = 0;
    if (strlen(text) > 65535) return false;
    const char *p = text;
    if (value(j, &p, 0) != 0) return false;
    whitespace(&p); return !*p;
}
bool json_is(const Json *j, int t, const char *literal) {
    if (t < 0 || t >= j->count) return false;
    int start = j->t[t].start, end = j->t[t].end;
    if (j->t[t].type == '"') { start++; end--; }
    return (size_t)(end - start) == strlen(literal) && !memcmp(j->s + start, literal, (size_t)(end - start));
}
int json_get(const Json *j, int object, const char *key) {
    if (object < 0 || object >= j->count || j->t[object].type != '{') return -1;
    for (int i = object + 1; i < j->t[object].next;) {
        int v = i + 1;
        if (json_is(j, i, key)) return v;
        i = j->t[v].next;
    }
    return -1;
}
static unsigned hex4(const char *s) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) v = v * 16 + (unsigned)(s[i] <= '9' ? s[i] - '0' : tolower((unsigned char)s[i]) - 'a' + 10);
    return v;
}
bool json_string(const Json *j, int token, char *out, size_t size) {
    if (!size) return false;
    *out = 0;
    if (token < 0 || token >= j->count || j->t[token].type != '"') return false;
    size_t n = 0;
    const char *s = j->s + j->t[token].start + 1, *end = j->s + j->t[token].end - 1;
    while (s < end) {
        unsigned c = (unsigned char)*s++;
        if (c == '\\') {
            c = (unsigned char)*s++;
            if (c == 'u') {
                c = hex4(s); s += 4;
                if (c >= 0xD800 && c <= 0xDBFF && end - s >= 6 && s[0] == '\\' && s[1] == 'u') {
                    unsigned low = hex4(s + 2);
                    if (low < 0xDC00 || low > 0xDFFF) { *out = 0; return false; }
                    c = 0x10000 + ((c - 0xD800) << 10) + low - 0xDC00; s += 6;
                } else if (c >= 0xD800 && c <= 0xDFFF) { *out = 0; return false; }
            } else if (strchr("bfnrt", (int)c)) c = ' ';
        }
        if (c < 32 || c == 127) c = ' '; /* API strings never inject terminal escapes */
        unsigned char bytes[4]; int k;
        if (c < 128) { bytes[0] = (unsigned char)c; k = 1; }
        else if (c < 256 && (unsigned char)s[-1] == c) { bytes[0] = (unsigned char)c; k = 1; }
        else if (c < 2048) { bytes[0] = 0xC0 | (c >> 6); bytes[1] = 0x80 | (c & 63); k = 2; }
        else if (c < 65536) { bytes[0] = 0xE0 | (c >> 12); bytes[1] = 0x80 | ((c >> 6) & 63); bytes[2] = 0x80 | (c & 63); k = 3; }
        else { bytes[0] = 0xF0 | (c >> 18); bytes[1] = 0x80 | ((c >> 12) & 63); bytes[2] = 0x80 | ((c >> 6) & 63); bytes[3] = 0x80 | (c & 63); k = 4; }
        if (n + (size_t)k >= size) { *out = 0; return false; }
        memcpy(out + n, bytes, (size_t)k); n += (size_t)k;
    }
    out[n] = 0; return true;
}
int64_t json_number(const Json *j, int t, int64_t fallback) {
    if (t < 0 || t >= j->count) return fallback;
    const char *s = j->s + j->t[t].start, *end = j->s + j->t[t].end;
    bool neg = *s == '-'; if (neg) s++;
    int64_t n = 0;
    if (s == end) return fallback;
    while (s < end) {
        if (*s < '0' || *s > '9' || n > (INT64_MAX - (*s - '0')) / 10) return fallback;
        n = n * 10 + *s++ - '0';
    }
    return neg ? -n : n;
}
