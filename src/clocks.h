/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CLOCKS_H
#define CLOCKS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {
    int64_t remaining[2], increment[2], anchor;
    int side;
    bool running, enabled;
} ChessClock;
int clock_index(int side);
void clock_start(ChessClock *c, int64_t initial, int64_t increment, int side, int64_t now);
int64_t clock_remaining(const ChessClock *c, int side, int64_t now);
bool clock_move(ChessClock *c, int side, int64_t now);
void clock_stop(ChessClock *c, int64_t now);
void clock_sync(ChessClock *c, int64_t white, int64_t black, int side, bool running, int64_t now);
void clock_format(int64_t ms, char *out, size_t n);
bool clock_parse(const char *text, int64_t *ms); /* PGN H:MM:SS[.mmm] */
#endif
