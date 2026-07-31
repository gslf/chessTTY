/* game.h — the game: move history, navigation, result */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GAME_H
#define GAME_H

#include "chess.h"

#define MAXPLY 1024

/* Result */
enum { RES_ONGOING = 0, RES_WHITE = 1, RES_BLACK = 2, RES_DRAW = 3 };

typedef struct {
    Pos start;
    char start_fen[128];
    bool custom_start;

    Pos pos[MAXPLY + 1];     /* pos[i] = position after i plies */
    Move moves[MAXPLY];
    char san[MAXPLY][16];
    char uci[MAXPLY][8];
    PosKey keys[MAXPLY + 1];
    int n;                   /* plies played */
    int view;                /* ply currently displayed (0..n) */

    int result;
    char reason[80];

    /* PGN tags (for loaded games or for saving) */
    char tag_event[80], tag_site[80], tag_date[16], tag_round[16];
    char tag_white[80], tag_black[80], tag_result[8];
} Game;

void game_reset(Game *g, const char *fen /* NULL = initial position */);
bool game_push(Game *g, Move m);          /* appends at the end, updates the result */
void game_update_result(Game *g);         /* mate/stalemate/automatic draws */
int  game_repetitions(const Game *g);     /* repetitions of the current position */

/* "position startpos moves e2e4 ..." (or "position fen ...") up to `upto` plies */
void game_uci_position(const Game *g, int upto, char *out, size_t n);

const char *result_str(int result);       /* "1-0" etc. */

#endif
