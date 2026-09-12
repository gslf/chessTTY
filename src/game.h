/* game.h — the game: move history, navigation, result */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GAME_H
#define GAME_H

#include "chess.h"

#define MAXPLY 1024

/* Annotations.  Text notes are sparse, so they live in a flat arena inside the
   Game instead of one fixed buffer per ply: an empty game costs nothing and the
   whole structure stays trivially copyable and free of heap ownership. */
#define NOTE_POOL_BYTES 16384    /* total annotation text per game */
#define NOTE_MAX 256             /* longest single annotation */

/* Move glyphs, stored as the standard PGN numeric annotation glyph values. */
enum { NAG_NONE = 0, NAG_GOOD = 1, NAG_MISTAKE = 2, NAG_BRILLIANT = 3,
       NAG_BLUNDER = 4, NAG_INTERESTING = 5, NAG_DUBIOUS = 6 };

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
    uint16_t opening[MAXPLY + 1]; /* last named position at each ply; O(1) redraw */
    int64_t clocks[MAXPLY + 1][2]; /* recorded milliseconds; -1 = unavailable */
    int64_t initial_ms, increment_ms; /* PGN TimeControl; 0 = unspecified */
    int n;                   /* plies played */
    int view;                /* ply currently displayed (0..n) */

    int result;
    char reason[80];

    /* Annotations.  note_off[i] is the offset in note_pool of the text attached
       to the position after i plies (0 = none); nag[i] is the glyph on move i. */
    uint32_t note_off[MAXPLY + 1];
    uint32_t note_used;
    uint8_t  nag[MAXPLY];
    char     note_pool[NOTE_POOL_BYTES];

    /* PGN tags (for loaded games or for saving) */
    char tag_event[80], tag_site[80], tag_date[16], tag_round[16];
    char tag_white[80], tag_black[80], tag_result[8];
    char tag_eco[8];
    uint16_t tag_white_elo, tag_black_elo;
} Game;

void game_reset(Game *g, const char *fen /* NULL = initial position */);
bool game_push(Game *g, Move m);          /* appends at the end, updates the result */
void game_truncate(Game *g, int ply);     /* drops everything after `ply` */
void game_update_result(Game *g);         /* mate/stalemate/automatic draws */
int  game_repetitions(const Game *g);     /* repetitions of the current position */

/* "position startpos moves e2e4 ..." (or "position fen ...") up to `upto` plies */
void game_uci_position(const Game *g, int upto, char *out, size_t n);

/* ---- annotations ---- */
const char *game_note(const Game *g, int ply);            /* "" when there is none */
bool game_set_note(Game *g, int ply, const char *text);   /* "" clears; false = full */
int  game_note_count(const Game *g);
const char *nag_glyph(int nag);                           /* "!", "??", … or "" */
int  nag_cycle(int nag);                                  /* none→!→!!→!?→?!→?→??→none */

const char *result_str(int result);       /* "1-0" etc. */

#endif
