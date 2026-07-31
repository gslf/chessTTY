/* chess.h — chess core: position, legal moves, FEN, SAN */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CHESS_H
#define CHESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pieces: positive = white, negative = black. abs() = type. */
enum { EMPTY = 0, PAWN = 1, KNIGHT = 2, BISHOP = 3, ROOK = 4, QUEEN = 5, KING = 6 };

/* Castling rights (bitmask) */
enum { CR_WK = 1, CR_WQ = 2, CR_BK = 4, CR_BQ = 8 };

/* Square: 0 = a1 ... 63 = h8. file = sq & 7, rank = sq >> 3 */
#define SQ(f, r) ((r) * 8 + (f))
#define FILE_OF(s) ((s) & 7)
#define RANK_OF(s) ((s) >> 3)

typedef struct {
    int8_t sq[64];
    int8_t stm;      /* side to move: +1 white, -1 black */
    uint8_t castle;  /* CR_* bitmask */
    int8_t ep;       /* en-passant square or -1 */
    int16_t halfmove;
    int16_t fullmove;
} Pos;

typedef struct {
    uint8_t from, to;
    int8_t promo;    /* 0 or KNIGHT..QUEEN */
} Move;

#define MAX_MOVES 256

void pos_start(Pos *p);
bool pos_from_fen(Pos *p, const char *fen);
void pos_to_fen(const Pos *p, char *buf, size_t n);

int  gen_legal(const Pos *p, Move *out);          /* max MAX_MOVES */
void make_move(Pos *p, Move m);
bool in_check(const Pos *p, int side);            /* is `side`'s king attacked? */
bool square_attacked(const Pos *p, int sq, int by);
int  king_square(const Pos *p, int side);
bool move_is_capture(const Pos *p, Move m);

/* SAN: move_to_san must be called on the position BEFORE the move */
void move_to_san(const Pos *p, Move m, char *out /* >=16 */);
bool san_to_move(const Pos *p, const char *san, Move *out);

/* UCI: "e2e4", "e7e8q" */
void move_to_uci(Move m, char *out /* >=8 */);
bool uci_to_move(const Pos *p, const char *s, Move *out);

/* State of the game in the given position */
typedef enum {
    GS_ONGOING = 0,
    GS_CHECKMATE,       /* the side to move is mated */
    GS_STALEMATE,
    GS_DRAW_MATERIAL,
    GS_DRAW_FIFTY,
} GameState;
GameState pos_state(const Pos *p);

/* Compact key for repetition counting (ignores the clocks) */
typedef struct { uint8_t k[34]; } PosKey;
void pos_key(const Pos *p, PosKey *out);
bool poskey_eq(const PosKey *a, const PosKey *b);

unsigned long long perft(const Pos *p, int depth);

#endif
