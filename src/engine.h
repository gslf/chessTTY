/* engine.h — UCI communication with Stockfish */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ENGINE_H
#define ENGINE_H

#include "chess.h"
#include "platform.h"
#include <stdbool.h>

typedef struct {
    Proc proc;
    char rbuf[65536];
    int rlen;
    bool ok;          /* UCI handshake succeeded */
    char name[64];
} Engine;

typedef struct {
    int depth;
    bool mate;        /* true: score = moves to mate (sign = side) */
    int score;        /* centipawns (or moves to mate), from WHITE's point of view */
    char pv_san[200]; /* principal variation converted to SAN with move numbers */
    char first_uci[8];
    bool valid;
} EngineLine;

bool engine_start(Engine *e, const char *path);      /* spawn + "uci" handshake */
void engine_send(Engine *e, const char *fmt, ...);   /* appends \n */
bool engine_poll_line(Engine *e, char *out, size_t n);
bool engine_wait_for(Engine *e, const char *prefix, int timeout_ms);
void engine_quit(Engine *e);

/* Parses an "info ... multipv N ... pv ..." line into the right slot of lines[3].
   `base` is the position being analysed (used to convert the PV to SAN). */
bool engine_parse_info(const char *line, const Pos *base, EngineLine lines[3]);

#endif
