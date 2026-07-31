/* game.c — game state and history */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game.h"
#include <stdio.h>
#include <string.h>

void game_reset(Game *g, const char *fen) {
    memset(g, 0, sizeof *g);
    if (fen && *fen) {
        if (!pos_from_fen(&g->start, fen)) pos_start(&g->start);
        else g->custom_start = strcmp(fen, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1") != 0;
    } else pos_start(&g->start);
    pos_to_fen(&g->start, g->start_fen, sizeof g->start_fen);
    g->pos[0] = g->start;
    pos_key(&g->pos[0], &g->keys[0]);
    g->n = 0;
    g->view = 0;
    g->result = RES_ONGOING;
    strcpy(g->tag_event, "ChessTUI game");
    strcpy(g->tag_site, "ChessTUI");
    strcpy(g->tag_round, "-");
    strcpy(g->tag_white, "White");
    strcpy(g->tag_black, "Black");
    strcpy(g->tag_result, "*");
}

int game_repetitions(const Game *g) {
    int count = 0;
    for (int i = 0; i <= g->n; i++)
        if (poskey_eq(&g->keys[i], &g->keys[g->n])) count++;
    return count;
}

void game_update_result(Game *g) {
    const Pos *p = &g->pos[g->n];
    GameState st = pos_state(p);
    switch (st) {
        case GS_CHECKMATE:
            g->result = p->stm > 0 ? RES_BLACK : RES_WHITE;
            strcpy(g->reason, "Checkmate");
            break;
        case GS_STALEMATE:
            g->result = RES_DRAW;
            strcpy(g->reason, "Stalemate");
            break;
        case GS_DRAW_MATERIAL:
            g->result = RES_DRAW;
            strcpy(g->reason, "Insufficient material");
            break;
        case GS_DRAW_FIFTY:
            g->result = RES_DRAW;
            strcpy(g->reason, "Fifty-move rule");
            break;
        default:
            if (game_repetitions(g) >= 3) {
                g->result = RES_DRAW;
                strcpy(g->reason, "Threefold repetition");
            } else {
                g->result = RES_ONGOING;
                g->reason[0] = 0;
            }
    }
    strcpy(g->tag_result, result_str(g->result));
}

bool game_push(Game *g, Move m) {
    if (g->n >= MAXPLY) return false;
    Pos *p = &g->pos[g->n];
    move_to_san(p, m, g->san[g->n]);
    move_to_uci(m, g->uci[g->n]);
    g->moves[g->n] = m;
    g->pos[g->n + 1] = *p;
    make_move(&g->pos[g->n + 1], m);
    g->n++;
    pos_key(&g->pos[g->n], &g->keys[g->n]);
    game_update_result(g);
    return true;
}

void game_uci_position(const Game *g, int upto, char *out, size_t n) {
    size_t off;
    if (g->custom_start)
        off = (size_t)snprintf(out, n, "position fen %s", g->start_fen);
    else
        off = (size_t)snprintf(out, n, "position startpos");
    if (upto > 0 && off < n) {
        off += (size_t)snprintf(out + off, n - off, " moves");
        for (int i = 0; i < upto && off < n - 8; i++)
            off += (size_t)snprintf(out + off, n - off, " %s", g->uci[i]);
    }
}

const char *result_str(int result) {
    switch (result) {
        case RES_WHITE: return "1-0";
        case RES_BLACK: return "0-1";
        case RES_DRAW: return "1/2-1/2";
        default: return "*";
    }
}
