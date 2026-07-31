/* pgn.h — PGN reading and writing */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PGN_H
#define PGN_H

#include "game.h"

typedef struct {
    char white[80], black[80], event[80], date[16], result[8];
    char fen[128];           /* empty = initial position */
    long mt_start, mt_end;   /* movetext offsets within the buffer */
} PgnRef;

typedef struct {
    char *buf;
    size_t len;
    PgnRef *games;
    int n;
} PgnList;

/* Scans the file and indexes its games. NULL on error (message in err). */
PgnList *pgn_scan_file(const char *path, char *err, size_t errn);
void pgn_list_free(PgnList *L);

/* Loads game idx into the Game structure. */
bool pgn_load_game(const PgnList *L, int idx, Game *g, char *err, size_t errn);

/* Saves the game to a file. */
bool pgn_save(const Game *g, const char *path, char *err, size_t errn);

#endif
