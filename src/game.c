/* game.c — game state and history */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game.h"
#include "openings.h"
#include <stdio.h>
#include <string.h>

void game_reset(Game *g, const char *fen) {
    memset(g, 0, sizeof *g);
    for (int i = 0; i <= MAXPLY; i++) g->clocks[i][0] = g->clocks[i][1] = -1;
    if (fen && *fen) {
        if (!pos_from_fen(&g->start, fen)) pos_start(&g->start);
        else g->custom_start = strcmp(fen, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1") != 0;
    } else pos_start(&g->start);
    pos_to_fen(&g->start, g->start_fen, sizeof g->start_fen);
    g->pos[0] = g->start;
    pos_key(&g->pos[0], &g->keys[0]);
    g->opening[0] = opening_lookup(&g->keys[0]);
    g->n = 0;
    g->view = 0;
    g->note_used = 1;        /* offset 0 is the "no annotation" sentinel */
    g->note_pool[0] = 0;
    g->result = RES_ONGOING;
    strcpy(g->tag_event, "ChessTTY game");
    strcpy(g->tag_site, "ChessTTY");
    strcpy(g->tag_round, "-");
    strcpy(g->tag_white, "White");
    strcpy(g->tag_black, "Black");
    game_update_result(g);
}

int game_repetitions(const Game *g) {
    int count = 0;
    int first = g->n - g->pos[g->n].halfmove;
    if (first < 0) first = 0;
    for (int i = g->n; i >= first; i -= 2)
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

/* Rewrites the arena keeping only the notes still attached to a live ply.
   Called after every edit: the arena is small enough that a full copy is
   cheaper and far simpler than a free list. */
static void notes_compact(Game *g) {
    char tmp[NOTE_POOL_BYTES];
    uint32_t used = 1;
    tmp[0] = 0;
    for (int i = 0; i <= g->n; i++) {
        if (!g->note_off[i]) continue;
        const char *src = g->note_pool + g->note_off[i];
        size_t len = strlen(src) + 1;
        if (used + len > sizeof tmp) { g->note_off[i] = 0; continue; }
        memcpy(tmp + used, src, len);
        g->note_off[i] = used;
        used += (uint32_t)len;
    }
    memcpy(g->note_pool, tmp, used);
    g->note_used = used;
}

const char *game_note(const Game *g, int ply) {
    if (ply < 0 || ply > g->n || !g->note_off[ply]) return "";
    return g->note_pool + g->note_off[ply];
}

int game_note_count(const Game *g) {
    int c = 0;
    for (int i = 0; i <= g->n; i++) if (g->note_off[i]) c++;
    return c;
}

/* A PGN comment is brace-delimited and single-run, so braces and control
   characters cannot survive a save.  They are folded here, on the way in, so
   that what the editor shows is exactly what the file will hold. */
static char note_safe_char(char c) {
    if (c == '{') return '(';
    if (c == '}') return ')';
    if ((unsigned char)c < 0x20 || c == 0x7f) return ' ';
    return c;
}

bool game_set_note(Game *g, int ply, const char *text) {
    if (ply < 0 || ply > g->n) return false;
    /* Copy first: text can refer to this game's arena. Check capacity before
       changing anything so a failed edit preserves the existing annotation. */
    char safe[NOTE_MAX];
    size_t len = 0;
    if (text) while (text[len] && len < sizeof safe - 1) {
        safe[len] = note_safe_char(text[len]);
        len++;
    }
    safe[len] = 0;
    size_t old = g->note_off[ply] ? strlen(game_note(g, ply)) + 1 : 0;
    if (g->note_used - old + (len ? len + 1 : 0) > NOTE_POOL_BYTES) return false;
    if (old) { g->note_off[ply] = 0; notes_compact(g); }
    if (!len) return true;
    memcpy(g->note_pool + g->note_used, safe, len + 1);
    g->note_off[ply] = g->note_used;
    g->note_used += (uint32_t)len + 1;
    return true;
}

const char *nag_glyph(int nag) {
    switch (nag) {
        case NAG_GOOD: return "!";
        case NAG_MISTAKE: return "?";
        case NAG_BRILLIANT: return "!!";
        case NAG_BLUNDER: return "??";
        case NAG_INTERESTING: return "!?";
        case NAG_DUBIOUS: return "?!";
        default: return "";
    }
}

int nag_cycle(int nag) {
    switch (nag) {
        case NAG_NONE: return NAG_GOOD;
        case NAG_GOOD: return NAG_BRILLIANT;
        case NAG_BRILLIANT: return NAG_INTERESTING;
        case NAG_INTERESTING: return NAG_DUBIOUS;
        case NAG_DUBIOUS: return NAG_MISTAKE;
        case NAG_MISTAKE: return NAG_BLUNDER;
        default: return NAG_NONE;
    }
}

void game_truncate(Game *g, int ply) {
    if (ply < 0) ply = 0;
    if (ply >= g->n) return;
    for (int i = ply + 1; i <= g->n; i++) g->note_off[i] = 0;
    for (int i = ply; i < g->n; i++) g->nag[i] = 0;
    g->n = ply;
    if (g->view > g->n) g->view = g->n;
    notes_compact(g);
    game_update_result(g);
}

bool game_push(Game *g, Move m) {
    if (g->n >= MAXPLY) return false;
    Pos *p = &g->pos[g->n];
    move_to_san(p, m, g->san[g->n]);
    move_to_uci(m, g->uci[g->n]);
    g->moves[g->n] = m;
    g->clocks[g->n + 1][0] = g->clocks[g->n][0];
    g->clocks[g->n + 1][1] = g->clocks[g->n][1];
    g->clocks[g->n + 1][p->stm > 0 ? 0 : 1] = -1;
    g->pos[g->n + 1] = *p;
    make_move(&g->pos[g->n + 1], m);
    g->n++;
    g->nag[g->n - 1] = NAG_NONE;   /* the slot may hold a replaced variation */
    g->note_off[g->n] = 0;
    pos_key(&g->pos[g->n], &g->keys[g->n]);
    uint16_t label = opening_lookup(&g->keys[g->n]);
    g->opening[g->n] = label ? label : g->opening[g->n - 1];
    game_update_result(g);
    return true;
}

void game_uci_position(const Game *g, int upto, char *out, size_t n) {
    if (upto < 0) upto = 0;
    if (upto > g->n) upto = g->n;
    size_t off;
    if (g->custom_start)
        off = (size_t)snprintf(out, n, "position fen %s", g->start_fen);
    else
        off = (size_t)snprintf(out, n, "position startpos");
    if (upto > 0 && off < n) {
        off += (size_t)snprintf(out + off, n - off, " moves");
        for (int i = 0; i < upto && off < n && n - off > 8; i++)
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
