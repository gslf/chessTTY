/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "clocks.h"
#include <stdio.h>
#include <string.h>

int clock_index(int side) { return side > 0 ? 0 : 1; }
void clock_start(ChessClock *c, int64_t initial, int64_t increment, int side, int64_t now) {
    memset(c, 0, sizeof *c);
    c->remaining[0] = c->remaining[1] = initial;
    c->increment[0] = c->increment[1] = increment;
    c->side = side;
    c->anchor = now;
    c->enabled = c->running = initial > 0;
}
int64_t clock_remaining(const ChessClock *c, int side, int64_t now) {
    if (!c->enabled) return -1;
    int64_t value = c->remaining[clock_index(side)];
    if (c->running && c->side == side && now > c->anchor) value -= now - c->anchor;
    return value > 0 ? value : 0;
}
bool clock_move(ChessClock *c, int side, int64_t now) {
    if (!c->enabled) return true;
    if (!c->running || c->side != side) return false;
    int64_t left = clock_remaining(c, side, now);
    if (left <= 0) return false; /* no increment can rescue a flag */
    c->remaining[clock_index(side)] = left + c->increment[clock_index(side)];
    c->side = -side;
    c->anchor = now;
    return true;
}
void clock_stop(ChessClock *c, int64_t now) {
    if (c->enabled && c->running)
        c->remaining[clock_index(c->side)] = clock_remaining(c, c->side, now);
    c->running = false;
}
void clock_sync(ChessClock *c, int64_t white, int64_t black, int side, bool running, int64_t now) {
    c->remaining[0] = white;
    c->remaining[1] = black;
    c->side = side;
    c->anchor = now;
    c->enabled = white >= 0 && black >= 0;
    c->running = running && c->enabled;
}
void clock_format(int64_t ms, char *out, size_t n) {
    if (ms < 0) { snprintf(out, n, "--:--"); return; }
    /* Round up to the display unit: 0.0 is shown only after actual expiry. */
    if (ms < 10000) {
        int64_t tenths = (ms + 99) / 100;
        snprintf(out, n, "0:%02lld.%lld", (long long)(tenths / 10), (long long)(tenths % 10));
    } else {
        int64_t seconds = (ms + 999) / 1000;
        snprintf(out, n, "%02lld:%02lld", (long long)(seconds / 60), (long long)(seconds % 60));
    }
}
bool clock_parse(const char *s, int64_t *ms) {
    int fields[3] = {0};
    for (int i = 0; i < 3; i++) {
        if (*s < '0' || *s > '9') return false;
        while (*s >= '0' && *s <= '9') {
            if (fields[i] > 10000) return false;
            fields[i] = fields[i] * 10 + *s++ - '0';
        }
        if (i < 2 && *s++ != ':') return false;
    }
    if (fields[1] >= 60 || fields[2] >= 60) return false;
    int fraction = 0, scale = 100;
    if (*s == '.') {
        s++;
        if (*s < '0' || *s > '9') return false;
        while (*s >= '0' && *s <= '9') {
            if (scale) { fraction += (*s - '0') * scale; scale /= 10; }
            s++;
        }
    }
    while (*s == ' ') s++;
    if (*s && *s != ']') return false;
    *ms = ((int64_t)fields[0] * 3600 + fields[1] * 60 + fields[2]) * 1000 + fraction;
    return true;
}
