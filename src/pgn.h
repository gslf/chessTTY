/* pgn.h — PGN reading and writing */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PGN_H
#define PGN_H

#include "game.h"

typedef struct {
    char white[80], black[80], event[80], site[80], round[16], date[16], result[8];
    char eco[8], timecontrol[32];
    char fen[128];           /* empty = initial position */
    long rec_start;          /* offset of the whole record (first tag) */
    long mt_start, mt_end;   /* movetext offsets within the buffer */
    int  plies;              /* token count, good enough for listings */
    int  white_elo, black_elo;
} PgnRef;

typedef struct {
    char *buf;
    size_t len;
    PgnRef *games;
    int n;
} PgnList;

/* Scans the file and indexes its games. NULL on error (message in err). */
PgnList *pgn_scan_file(const char *path, char *err, size_t errn);
/* Same over a buffer the list takes ownership of (must come from malloc). */
PgnList *pgn_scan_mem(char *buf, size_t len, char *err, size_t errn);
void pgn_list_free(PgnList *L);

/* Loads game idx into the Game structure. */
bool pgn_load_game(const PgnList *L, int idx, Game *g, char *err, size_t errn);

/* Loads the single game stored in [off, off+len) of `path`.  Reads only that
   byte range, so opening one game out of a huge collection stays O(game). */
bool pgn_load_at(const char *path, long off, long len, Game *g, char *err, size_t errn);

/* Saves the game to a file (overwriting it). */
bool pgn_save(const Game *g, const char *path, char *err, size_t errn);

/* Appends the game to a file, creating it when missing, and reports the byte
   range the record occupies so an index can be updated without re-reading. */
bool pgn_append(const Game *g, const char *path, long *off, long *len,
                char *err, size_t errn);

#endif
