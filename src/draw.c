/* draw.c — TUI rendering */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "app.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------ palette ------------------------------ */
#define CL_LSQ     0xC8AD7F   /* light square (light walnut) */
#define CL_DSQ     0x86634B   /* dark square */
#define CL_LSQ_HL  0xD6C475   /* last move */
#define CL_DSQ_HL  0xA89B4E
#define CL_SQ_CK   0xC85F4B   /* king in check */
#define CL_LSQ_BST 0xA9C2A4   /* engine suggestion */
#define CL_DSQ_BST 0x6F9377
#define CL_WP      0xFBFBF4   /* white pieces */
#define CL_BP      0x201E1B   /* black pieces */
#define CL_FRAME   0x8C7B62
#define CL_DIM     0x8F8F8F
#define CL_TXT     0xD8D8D8
#define CL_ACC     0xE0B15E   /* gold */
#define CL_ERR     0xE06C60
#define CL_OK      0x97C078
#define CL_PANEL   0x6C6C6C
#define CL_SEL_BG  0xE0B15E
#define CL_SEL_FG  0x201E1B
#define CL_BAR_W   0xE9E9E2
#define CL_BAR_B   0x3A3833

static const char *G_FILLED[7]  = { "", "♟", "♞", "♝", "♜", "♛", "♚" };
static const char *G_OUTLINE[7] = { "", "♙", "♘", "♗", "♖", "♕", "♔" };
static const char G_ASCII[8]    = " PNBRQK";

/* board geometry */
#define BL 4   /* column of the left border */
#define BT 2   /* row of the top border */

/* board sizes: cell width × height per size step */
#define N_SIZES 4
static const int CELL_W[N_SIZES] = { 3, 5, 7, 9 };
static const int CELL_H[N_SIZES] = { 1, 2, 3, 4 };

/* Piece sprites drawn with half-block characters (fg = piece, bg = square).
   Sizes 2 and 3 use them; smaller boards fall back to the single glyph. */
static const char *SPR5[7][3] = {
    { "", "", "" },
    { " ▄█▄ ", "  █  ", "▄███▄" },   /* pawn */
    { "▄█▀█ ", "  ██ ", "▄███▄" },   /* knight */
    { " ▄▀▄ ", " ▀█▀ ", "▄███▄" },   /* bishop */
    { "█▄█▄█", " ███ ", "▄███▄" },   /* rook */
    { "▀▄█▄▀", " ███ ", "▄███▄" },   /* queen */
    { " ▄█▄ ", " ███ ", "█████" },   /* king */
};
static const char *SPR7[7][4] = {
    { "", "", "", "" },
    { "  ▄█▄  ", "  ███  ", "   █   ", "▄█████▄" },   /* pawn */
    { " ▄███▄ ", "█▀▀▄██ ", "  ▄███ ", "▄█████▄" },   /* knight */
    { "  ▄▀▄  ", " ▄███▄ ", "  ▀█▀  ", "▄█████▄" },   /* bishop */
    { "█▄█▄█▄█", " ▀███▀ ", "  ███  ", "▄█████▄" },   /* rook */
    { "█▄▄█▄▄█", " █████ ", "  ███  ", "▄█████▄" },   /* queen */
    { "  ▄█▄  ", " ▄▄█▄▄ ", " ▀███▀ ", "▄█████▄" },   /* king */
};

int board_max_size(const App *a) {
    int best = 0;
    for (int s = 0; s < N_SIZES; s++) {
        bool fits_w = BL + 8 * CELL_W[s] + 4 + 24 <= a->tw - 1; /* board + panel minimum */
        bool fits_h = 8 * CELL_H[s] <= a->th - 8;               /* board + top/bottom rows */
        if (fits_w && fits_h) best = s;
    }
    return best;
}

int board_eff_size(const App *a) {
    int m = board_max_size(a);
    return a->board_size < m ? a->board_size : m;
}

/* ------------------------------ helpers ------------------------------ */
static int utf8_cols(const char *s) {
    int n = 0;
    for (; *s; s++)
        if (((unsigned char)*s & 0xC0) != 0x80) n++;
    return n;
}

/* writes at most `width` columns, then pads with spaces */
static void put_padded(SB *b, const char *s, int width) {
    int cols = 0;
    const char *p = s;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if ((c & 0xC0) != 0x80) {
            if (cols + 1 > width) break;
            cols++;
        }
        sb_putn(b, p, 1);
        p++;
    }
    /* finish any continuation bytes of the last character */
    while (((unsigned char)*p & 0xC0) == 0x80) { sb_putn(b, p, 1); p++; }
    for (; cols < width; cols++) sb_put(b, " ");
}

static void hline(SB *b, int n) { for (int i = 0; i < n; i++) sb_put(b, "─"); }

static void draw_box(App *a, int r0, int c0, int r1, int c1, const char *title, uint32_t col) {
    SB *b = &a->fb;
    int iw = c1 - c0 - 1;
    t_moveto(b, r0, c0);
    t_style_fg(b, col, 0);
    sb_put(b, "╭");
    if (title && *title) {
        sb_put(b, "╴");
        t_style_fg(b, CL_ACC, A_BOLD);
        sb_put(b, title);
        t_style_fg(b, col, 0);
        sb_put(b, "╶");
        int used = 2 + utf8_cols(title);
        hline(b, iw - used > 0 ? iw - used : 0);
    } else hline(b, iw);
    sb_put(b, "╮");
    for (int r = r0 + 1; r < r1; r++) {
        t_moveto(b, r, c0);
        t_style_fg(b, col, 0);
        sb_put(b, "│");
        t_moveto(b, r, c1);
        sb_put(b, "│");
    }
    t_moveto(b, r1, c0);
    t_style_fg(b, col, 0);
    sb_put(b, "╰");
    hline(b, iw);
    sb_put(b, "╯");
}

static void score_str(const EngineLine *L, char *out, size_t n) {
    if (!L->valid) { snprintf(out, n, "  ..."); return; }
    if (L->mate) {
        if (L->score > 0) snprintf(out, n, "#%d", L->score);
        else snprintf(out, n, "#-%d", -L->score);
    } else {
        double v = L->score / 100.0;
        if (v >= 100) v = 99.9;
        if (v <= -100) v = -99.9;
        snprintf(out, n, "%+.2f", v);
    }
}

/* ------------------------------ board ------------------------------ */
static void draw_player_line(App *a, int row, int side) {
    SB *b = &a->fb;
    Game *g = &a->game;
    char nm[sizeof g->tag_white];   /* holds the longest PGN name tag */
    if (a->mode == MODE_PLAY) {
        if (side == a->opp.side)
            snprintf(nm, sizeof nm, "Stockfish L%d", a->opp.difficulty);
        else snprintf(nm, sizeof nm, "You");
    } else
        snprintf(nm, sizeof nm, "%s", side > 0 ? g->tag_white : g->tag_black);
    bool turn = g->pos[g->view].stm == side && g->result == RES_ONGOING;
    t_moveto(b, row, 2);
    /* chip in the player's colour */
    t_style(b, side > 0 ? CL_WP : 0xD8D8D8, side > 0 ? 0x4A4640 : 0x26241F, A_BOLD);
    sb_printf(b, " %s ", side > 0 ? "♔" : "♚");
    t_reset(b);
    sb_put(b, " ");
    t_style_fg(b, turn ? CL_ACC : CL_TXT, turn ? A_BOLD : 0);
    put_padded(b, nm, 15);
    sb_put(b, " ");
    int64_t time_ms = -1;
    bool live = g->view == g->n && (a->mode == MODE_PLAY || a->mode == MODE_LOCAL || a->mode == MODE_ONLINE);
    if (live) time_ms = clock_remaining(a->mode == MODE_ONLINE ? &a->online.clock : &a->clock, side, now_ms());
    else time_ms = g->clocks[g->view][clock_index(side)];
    if (a->mode != MODE_OPENINGS) {
        char clock[24], display[32]; clock_format(time_ms, clock, sizeof clock);
        snprintf(display, sizeof display, "%s%s", live && a->mode == MODE_ONLINE && !a->online.connected && !a->online.ended ? "~" : "", clock);
        t_style_fg(b, time_ms >= 0 && time_ms < 10000 ? CL_ERR : turn ? CL_ACC : CL_TXT, A_BOLD);
        put_padded(b, display, 11);
    } else put_padded(b, turn ? "● to move" : "", 11);
    t_reset(b);
}

static void spaces(SB *b, int n) { for (int i = 0; i < n; i++) sb_put(b, " "); }

static void draw_board(App *a) {
    SB *b = &a->fb;
    Game *g = &a->game;
    const Pos *p = &g->pos[g->view];
    int bs = board_eff_size(a);
    int cw = CELL_W[bs], ch = CELL_H[bs];
    int bw = 8 * cw;                 /* inner board width */
    int pad_l = cw / 2, pad_r = cw - pad_l - 1;
    int glyph_row = (ch - 1) / 2;    /* row of the cell holding the piece */

    int lm_from = -1, lm_to = -1;
    if (g->view > 0) { lm_from = g->moves[g->view - 1].from; lm_to = g->moves[g->view - 1].to; }
    int ck_sq = -1;
    if (in_check(p, p->stm)) ck_sq = king_square(p, p->stm);
    int bst_from = -1, bst_to = -1;
    if (a->ana.want && a->ana.lines[0].valid && a->ana.lines[0].first_uci[0]) {
        const char *u = a->ana.lines[0].first_uci;
        bst_from = SQ(u[0] - 'a', u[1] - '1');
        bst_to = SQ(u[2] - 'a', u[3] - '1');
    }

    draw_player_line(a, BT - 1, a->flip ? 1 : -1);
    draw_player_line(a, BT + 8 * ch + 3, a->flip ? -1 : 1);

    /* top border */
    t_moveto(b, BT, BL);
    t_style_fg(b, CL_FRAME, 0);
    sb_put(b, "╭");
    hline(b, bw);
    sb_put(b, "╮");

    for (int dr = 0; dr < 8; dr++) {
        int rank = a->flip ? dr : 7 - dr;
        for (int irow = 0; irow < ch; irow++) {
            int row = BT + 1 + dr * ch + irow;
            if (irow == glyph_row) {
                t_moveto(b, row, BL - 2);
                t_style_fg(b, CL_DIM, 0);
                sb_printf(b, "%c", '1' + rank);
            }
            t_moveto(b, row, BL);
            t_style_fg(b, CL_FRAME, 0);
            sb_put(b, "│");
            for (int df = 0; df < 8; df++) {
                int file = a->flip ? 7 - df : df;
                int sq = SQ(file, rank);
                bool light = ((file + rank) & 1) != 0;
                uint32_t bg = light ? CL_LSQ : CL_DSQ;
                if (sq == lm_from || sq == lm_to) bg = light ? CL_LSQ_HL : CL_DSQ_HL;
                if (sq == bst_from || sq == bst_to) bg = light ? CL_LSQ_BST : CL_DSQ_BST;
                if (sq == ck_sq) bg = CL_SQ_CK;
                int8_t pc = p->sq[sq];
                int t = pc > 0 ? pc : -pc;
                bool sprite = a->piece_style == 0 && ch >= 3 && pc != EMPTY;
                if (sprite) {
                    /* multi-row half-block sprite filling the square */
                    const char *art = ch == 3 ? SPR5[t][irow] : SPR7[t][irow];
                    int sw = ch == 3 ? 5 : 7;
                    int lp = (cw - sw) / 2;
                    t_style(b, pc > 0 ? CL_WP : CL_BP, bg, 0);
                    spaces(b, lp);
                    sb_put(b, art);
                    spaces(b, cw - sw - lp);
                } else if (pc == EMPTY || irow != glyph_row) {
                    t_style(b, bg, bg, 0);
                    spaces(b, cw);
                } else if (a->piece_style == 0) {
                    t_style(b, pc > 0 ? CL_WP : CL_BP, bg, A_BOLD);
                    spaces(b, pad_l);
                    sb_put(b, G_FILLED[t]);
                    spaces(b, pad_r);
                } else if (a->piece_style == 1) {
                    t_style(b, CL_BP, bg, A_BOLD);
                    spaces(b, pad_l);
                    sb_put(b, pc > 0 ? G_OUTLINE[t] : G_FILLED[t]);
                    spaces(b, pad_r);
                } else {
                    t_style(b, pc > 0 ? CL_WP : CL_BP, bg, A_BOLD);
                    spaces(b, pad_l);
                    sb_printf(b, "%c", pc > 0 ? G_ASCII[t] : (char)(G_ASCII[t] + 32));
                    spaces(b, pad_r);
                }
            }
            t_style_fg(b, CL_FRAME, 0);
            sb_put(b, "│");
        }
    }
    t_moveto(b, BT + 8 * ch + 1, BL);
    t_style_fg(b, CL_FRAME, 0);
    sb_put(b, "╰");
    hline(b, bw);
    sb_put(b, "╯");
    /* file letters */
    t_moveto(b, BT + 8 * ch + 2, BL + 1);
    t_style_fg(b, CL_DIM, 0);
    for (int df = 0; df < 8; df++) {
        int file = a->flip ? 7 - df : df;
        spaces(b, pad_l);
        sb_printf(b, "%c", 'a' + file);
        spaces(b, pad_r);
    }
    t_reset(b);
}

/* ------------------------------ notation panel ------------------------------ */
static void draw_notation(App *a, int r0, int c0, int r1, int c1) {
    SB *b = &a->fb;
    Game *g = &a->game;
    char title[64];
    int notes = game_note_count(g);
    if (notes) snprintf(title, sizeof title, "Moves  %d/%d · %d note%s", g->view, g->n,
                        notes, notes == 1 ? "" : "s");
    else snprintf(title, sizeof title, "Moves  %d/%d", g->view, g->n);
    draw_box(a, r0, c0, r1, c1, title, CL_PANEL);
    int iw = c1 - c0 - 1;
    int vis = r1 - r0 - 1;
    if (vis < 1 || iw < 20) return;

    int offset = g->start.stm < 0 ? 1 : 0;
    int total_rows = (offset + g->n + 1) / 2;
    int cur_row = g->view > 0 ? (g->view - 1 + offset) / 2 : 0;
    int scroll = 0;
    if (total_rows > vis) {
        scroll = cur_row - vis / 2;
        if (scroll < 0) scroll = 0;
        if (scroll > total_rows - vis) scroll = total_rows - vis;
    }
    int wsan = (iw - 7) / 2;
    if (wsan > 12) wsan = 12;
    if (wsan < 6) wsan = 6;

    for (int line = 0; line < vis; line++) {
        int r = scroll + line;
        t_moveto(b, r0 + 1 + line, c0 + 1);
        if (r >= total_rows) {
            t_reset(b);
            put_padded(b, "", iw);
            continue;
        }
        char num[16];
        snprintf(num, sizeof num, "%3d.", g->start.fullmove + r);
        t_style_fg(b, CL_DIM, 0);
        sb_put(b, num);
        sb_put(b, " ");
        int used = (int)strlen(num) + 1;
        for (int col = 0; col < 2; col++) {
            int ply = r * 2 + col + 1 - offset; /* 1-based */
            bool real = ply >= 1 && ply <= g->n;
            char cell[24];
            if (!real) snprintf(cell, sizeof cell, "%s", ply < 1 ? "…" : "");
            else snprintf(cell, sizeof cell, "%s%s%s", g->san[ply - 1],
                          nag_glyph(g->nag[ply - 1]),
                          *game_note(g, ply) ? "\xe2\x80\xa2" : "");
            if (real && ply == g->view) {
                t_style(b, CL_SEL_FG, CL_SEL_BG, A_BOLD);
                put_padded(b, cell, wsan);
                t_reset(b);
            } else if (real && (g->nag[ply - 1] || *game_note(g, ply))) {
                t_style_fg(b, CL_ACC, 0);          /* annotated: stands out */
                put_padded(b, cell, wsan);
            } else {
                t_style_fg(b, real ? CL_TXT : CL_DIM, 0);
                put_padded(b, cell, wsan);
            }
            sb_put(b, " ");
            used += wsan + 1;
        }
        t_reset(b);
        for (; used < iw; used++) sb_put(b, " ");
    }
}

/* ------------------------------ annotation panel ------------------------------ */
/* Rows the annotation of the position on screen needs, 0 when there is none. */
static int note_panel_rows(const App *a, int width) {
    const char *note = game_note(&a->game, a->game.view);
    if (!*note || width < 8) return 0;
    int lines = (utf8_cols(note) + width - 1) / width;
    if (lines > 3) lines = 3;
    return lines + 2;                 /* the box borders */
}

static void draw_note_panel(App *a, int r0, int c0, int r1, int c1) {
    const char *note = game_note(&a->game, a->game.view);
    int width = c1 - c0 - 1;
    char title[48];
    const Game *g = &a->game;
    if (g->view > 0)
        snprintf(title, sizeof title, "Note · %d%s %s", g->pos[g->view - 1].fullmove,
                 g->pos[g->view - 1].stm > 0 ? "." : "...", g->san[g->view - 1]);
    else
        snprintf(title, sizeof title, "Note · start");
    draw_box(a, r0, c0, r1, c1, title, CL_PANEL);
    for (int r = r0 + 1; r < r1; r++) {
        t_moveto(&a->fb, r, c0 + 1);
        t_style_fg(&a->fb, CL_TXT, 0);
        put_padded(&a->fb, note, width);
        int cols = 0;
        while (*note && cols < width) {
            note++;
            while (((unsigned char)*note & 0xC0) == 0x80) note++;
            cols++;
        }
    }
    t_reset(&a->fb);
}

/* ------------------------------ analysis panel ------------------------------ */
static void draw_analysis(App *a, int r0, int c0, int r1, int c1) {
    SB *b = &a->fb;
    char title[80];
    int depth = a->ana.lines[0].valid ? a->ana.lines[0].depth : 0;
    int64_t elapsed = a->ana.state == ANA_RUNNING && a->ana.started_ms ? now_ms() - a->ana.started_ms : a->ana.elapsed_ms;
    if (elapsed < 0) elapsed = 0;
    snprintf(title, sizeof title, "Stockfish d%d · %02lld:%02lld.%lld", depth,
        (long long)(elapsed / 60000), (long long)(elapsed / 1000 % 60), (long long)(elapsed / 100 % 10));
    draw_box(a, r0, c0, r1, c1, title, CL_PANEL);
    int iw = c1 - c0 - 1;

    const Pos *p = &a->game.pos[a->game.view];
    Move l[MAX_MOVES];
    bool over = gen_legal(p, l) == 0;
    bool mate = over && in_check(p, p->stm);

    /* row 1: evaluation bar */
    t_moveto(b, r0 + 1, c0 + 1);
    char sc[16];
    score_str(&a->ana.lines[0], sc, sizeof sc);
    int barw = iw - 8;
    if (barw < 6) barw = 6;
    double wp = 0.5;
    if (over) {
        wp = mate ? (p->stm > 0 ? 0.0 : 1.0) : 0.5;
        snprintf(sc, sizeof sc, "%s", mate ? "#" : "=");
    } else if (a->ana.lines[0].valid) {
        const EngineLine *L = &a->ana.lines[0];
        if (L->mate) wp = L->score > 0 ? 1.0 : 0.0;
        else wp = 1.0 / (1.0 + exp(-0.00368208 * (double)L->score));
    }
    int wcells = (int)(wp * barw + 0.5);
    t_style(b, CL_BAR_B, CL_BAR_W, 0);
    for (int i = 0; i < wcells; i++) sb_put(b, " ");
    t_style(b, CL_BAR_W, CL_BAR_B, 0);
    for (int i = wcells; i < barw; i++) sb_put(b, " ");
    t_reset(b);
    t_style_fg(b, CL_ACC, A_BOLD);
    char sctxt[24];
    snprintf(sctxt, sizeof sctxt, " %s", sc);
    put_padded(b, sctxt, iw - barw);

    /* rows 2-4: the three best lines */
    for (int i = 0; i < 3; i++) {
        t_moveto(b, r0 + 2 + i, c0 + 1);
        if (over) {
            if (i == 0) {
                t_style_fg(b, CL_ACC, A_BOLD);
                put_padded(b, mate ? "Checkmate" : "No legal moves", iw);
            } else { t_reset(b); put_padded(b, "", iw); }
            continue;
        }
        const EngineLine *L = &a->ana.lines[i];
        t_style_fg(b, CL_DIM, 0);
        sb_printf(b, "%d ", i + 1);
        char s2[16];
        score_str(L, s2, sizeof s2);
        char left[24];
        snprintf(left, sizeof left, "%6s ", s2);
        t_style_fg(b, CL_TXT, A_BOLD);
        sb_put(b, left);
        t_style_fg(b, L->valid ? CL_TXT : CL_DIM, 0);
        put_padded(b, L->valid ? L->pv_san : "…", iw - 2 - 7);
    }
    t_reset(b);
}

/* ------------------------------ bottom rows ------------------------------ */
static void draw_bottom(App *a, int *cur_r, int *cur_c) {
    SB *b = &a->fb;
    Game *g = &a->game;
    int msg_row = a->th - 2, in_row = a->th - 1;

    /* message row */
    t_moveto(b, msg_row, 2);
    if (a->msg[0]) {
        uint32_t col = a->msg_kind == 1 ? CL_ERR : a->msg_kind == 2 ? CL_OK : CL_DIM;
        t_style_fg(b, col, a->msg_kind ? A_BOLD : 0);
        put_padded(b, a->msg, a->tw - 2);
    } else if (g->result != RES_ONGOING) {
        t_style_fg(b, CL_ACC, A_BOLD);
        char line[160];
        snprintf(line, sizeof line, "★ %s — %s", result_str(g->result), g->reason);
        put_padded(b, line, a->tw - 2);
    } else {
        t_reset(b);
        put_padded(b, "", a->tw - 2);
    }

    /* input row */
    t_moveto(b, in_row, 2);
    if (a->modal == MODAL_SAVE) {
        t_style_fg(b, CL_ACC, A_BOLD);
        sb_put(b, "Save PGN › ");
        t_style_fg(b, CL_TXT, 0);
        put_padded(b, a->save_input, a->tw - 15);
        *cur_r = in_row;
        *cur_c = 2 + 11 + a->save_len;
    } else if (a->modal == MODAL_NOTE) {
        t_style_fg(b, CL_DIM, 0);
        put_padded(b, "Editing annotation",
                   a->tw - 2);
    } else if (a->modal == MODAL_PROMO) {
        t_style_fg(b, CL_ACC, A_BOLD);
        put_padded(b, "Choose a promotion piece", a->tw - 2);
    } else if (a->modal == MODAL_CONFIRM_QUIT) {
        t_style_fg(b, CL_ERR, A_BOLD);
        put_padded(b, "Leave this game? The game in progress will be lost.", a->tw - 2);
    } else if (a->modal == MODAL_RESIGN) {
        t_style_fg(b, CL_ERR, A_BOLD);
        put_padded(b, "Resign the Lichess game?", a->tw - 2);
    } else if (a->modal == MODAL_CONFIRM_NEW) {
        t_style_fg(b, CL_ACC, A_BOLD);
        put_padded(b, "Start a new game?", a->tw - 2);
    } else if (a->mode == MODE_PLAY || a->mode == MODE_ANALYZE || a->mode == MODE_LOCAL || a->mode == MODE_ONLINE) {
        t_style_fg(b, CL_ACC, A_BOLD);
        sb_put(b, "› ");
        t_style_fg(b, CL_TXT, A_BOLD);
        if (a->input_len > 0) put_padded(b, a->input, a->tw - 6);
        else {
            char hint[160];
            if (a->mode == MODE_ANALYZE)
                snprintf(hint, sizeof hint,
                         "position %d/%d · %s to move",
                         g->view, g->n, g->pos[g->view].stm > 0 ? "White" : "Black");
            else
                snprintf(hint, sizeof hint, "%s", g->result == RES_ONGOING ?
                         "Move (e4, Nf3, O-O)" :
                         "Game over");
            t_style_fg(b, CL_DIM, 0);
            put_padded(b, hint, a->tw - 6);
        }
        *cur_r = in_row;
        *cur_c = 4 + a->input_len;
    } else {
        t_style_fg(b, CL_DIM, 0);
        char nav[120];
        if (a->mode == MODE_OPENINGS)
            snprintf(nav, sizeof nav, "Opening explorer · ply %d", g->view);
        else
            snprintf(nav, sizeof nav, "position %d/%d",
                     g->view, g->n);
        put_padded(b, nav, a->tw - 2);
    }

    t_reset(b);
}

/* ------------------------------ annotation editor ------------------------------ */
/* A full-width overlay rather than a one-line field: annotations are sentences,
   and they have to stay readable while they are being typed. */
static void draw_note_editor(App *a, int *cur_r, int *cur_c) {
    SB *b = &a->fb;
    const Game *g = &a->game;
    int w = a->tw - 8;
    if (w > 76) w = 76;
    if (w < 40) w = a->tw - 4;
    int text_w = w - 2;
    int lines = (a->note_len + text_w - 1) / text_w;
    if (lines < 1) lines = 1;
    if (lines > 4) lines = 4;
    int h = lines + 2;
    int r0 = a->th - 5 - h;
    if (r0 < 2) r0 = 2;
    int c0 = (a->tw - w) / 2 + 1;
    if (c0 < 2) c0 = 2;

    char title[80];
    if (g->view > 0)
        snprintf(title, sizeof title, "Annotation · %d%s %s",
                 g->pos[g->view - 1].fullmove,
                 g->pos[g->view - 1].stm > 0 ? "." : "...", g->san[g->view - 1]);
    else
        snprintf(title, sizeof title, "Annotation · starting position");
    draw_box(a, r0, c0, r0 + h - 1, c0 + w - 1, title, CL_ACC);

    for (int i = 0; i < lines; i++) {
        t_moveto(b, r0 + 1 + i, c0 + 1);
        t_style_fg(b, CL_TXT, 0);
        int off = i * text_w;
        char chunk[NOTE_MAX];
        int k = 0;
        for (; off + k < a->note_len && k < text_w; k++) chunk[k] = a->note_input[off + k];
        chunk[k] = 0;
        put_padded(b, chunk, text_w);
    }
    if (a->note_len == 0) {
        t_moveto(b, r0 + 1, c0 + 1);
        t_style_fg(b, CL_DIM, 0);
        put_padded(b, "type the comment for this position…", text_w);
    }
    t_moveto(b, r0 + h - 1, c0 + 3);
    t_style_fg(b, CL_DIM, 0);
    char foot[64];
    snprintf(foot, sizeof foot, "╴%d/%d╶", a->note_len, NOTE_MAX - 1);
    sb_put(b, foot);
    t_reset(b);
    *cur_r = r0 + 1 + (a->note_len / text_w > lines - 1 ? lines - 1 : a->note_len / text_w);
    *cur_c = c0 + 1 + a->note_len % text_w;
}

/* ------------------------------ prefix command palette ------------------------------ */
static void draw_commands(App *a, int *cur_r, int *cur_c) {
    Command commands[COMMAND_MAX];
    int count = app_commands(a, commands);
    int w = 62, h = count + 6;
    int r0 = (a->th - h) / 2 + 1, c0 = (a->tw - w) / 2 + 1;
    const char *context = a->modal != MODAL_NONE || a->prompt != PROMPT_NONE || a->db_filtering ? "Input" :
                          a->screen == SCR_ONLINE ? "Lichess" : a->screen == SCR_DB ? "Database" : a->screen == SCR_MENU ? "Main menu" :
                          a->screen == SCR_PICKER ? "Game picker" :
                          a->mode == MODE_OPENINGS ? "Opening explorer" : "Board";
    char title[80];
    snprintf(title, sizeof title, "C-x · %s", context);
    SB *b = &a->fb;
    /* Paint the whole interior so the previous screen cannot bleed through. */
    for (int i = 1; i < h - 1; i++) {
        t_moveto(b, r0 + i, c0 + 1);
        t_style_fg(b, CL_TXT, 0);
        put_padded(b, "", w - 2);
    }
    draw_box(a, r0, c0, r0 + h - 1, c0 + w - 1, title, CL_ACC);
    t_moveto(b, r0 + 1, c0 + 2);
    t_style_fg(b, a->command_unknown ? CL_ERR : CL_DIM, 0);
    put_padded(b, a->command_unknown ? "Unknown key. Choose a listed command." : "Choose a command", w - 4);
    for (int i = 0; i < count; i++) {
        t_moveto(b, r0 + 2 + i, c0 + 3);
        t_style_fg(b, CL_ACC, A_BOLD);
        sb_printf(b, "%c   ", commands[i].key);
        t_style_fg(b, CL_TXT, 0);
        put_padded(b, commands[i].label, w - 10);
    }
    t_moveto(b, r0 + h - 3, c0 + 2);
    t_style_fg(b, CL_DIM, 0);
    const char *navigation = "Arrows navigate · Enter selects / applies";
    if (a->screen == SCR_GAME && a->modal == MODAL_NONE)
        navigation = a->mode == MODE_OPENINGS ? "Arrows explore · Home root · End last" :
                     "Type moves · Enter plays · Left/Right browse";
    put_padded(b, navigation, w - 4);
    t_moveto(b, r0 + h - 2, c0 + 2);
    put_padded(b, "C-g / Esc cancel prefix", w - 4);
    t_reset(b);
    *cur_r = *cur_c = 0;
}

/* ------------------------------ game screen ------------------------------ */
static int draw_opening_name(App *a, int c0, int c1) {
    const char *name = opening_name(a->game.opening[a->game.view]);
    if (!*name) name = "No named opening yet";
    int width = c1 - c0 - 1;
    int rows = (utf8_cols(name) + width - 1) / width;
    if (rows > a->th - 14) rows = a->th - 14;
    if (rows < 1) rows = 1;
    draw_box(a, 1, c0, rows + 2, c1, "Opening", CL_PANEL);
    for (int r = 0; r < rows; r++) {
        t_moveto(&a->fb, r + 2, c0 + 1);
        t_style_fg(&a->fb, CL_ACC, 0);
        put_padded(&a->fb, name, width);
        int cols = 0;
        while (*name && cols < width) {
            name++;
            while (((unsigned char)*name & 0xC0) == 0x80) name++;
            cols++;
        }
    }
    return rows + 3;
}

static void draw_openings(App *a, int r0, int c0, int r1, int c1) {
    SB *b = &a->fb;
    Game *g = &a->game;
    uint16_t first = opening_child(a->opening_path[g->view]);
    int count = 0;
    for (uint16_t node = first; node; node = opening_next(node)) count++;
    char title[80];
    snprintf(title, sizeof title, "Continuations · %d", count);
    draw_box(a, r0, c0, r1, c1, title, CL_PANEL);
    int visible = r1 - r0 - 1, width = c1 - c0 - 1;
    if (visible < 1) return;
    int selected = a->opening_selection[g->view];
    int scroll = selected >= visible ? selected - visible + 1 : 0;
    int index = 0;
    for (uint16_t node = first; node; node = opening_next(node), index++) {
        if (index < scroll || index >= scroll + visible) continue;
        char san[16], line[256];
        move_to_san(&g->pos[g->view], opening_move(node), san);
        snprintf(line, sizeof line, "%s %-7s %s", index == selected ? "▸" : " ",
                 san, opening_name(opening_label(node)));
        t_moveto(b, r0 + 1 + index - scroll, c0 + 1);
        t_style_fg(b, index == selected ? CL_ACC : CL_TXT, index == selected ? A_BOLD : 0);
        put_padded(b, line, width);
    }
    if (!count) {
        t_moveto(b, r0 + 1, c0 + 1);
        t_style_fg(b, CL_DIM, 0);
        put_padded(b, "End of line", width);
    }
}

static void draw_game(App *a, int *cur_r, int *cur_c) {
    draw_board(a);
    int rx = BL + 8 * CELL_W[board_eff_size(a)] + 4; /* left column of the panels */
    int rgt = a->tw - 1;
    if (rgt > rx + 70) rgt = rx + 70;     /* don't grow too wide */
    int bottom = a->th - 3;
    /* Labels wrap to a variable height; clear the panel region before layout. */
    for (int row = 1; row <= bottom; row++) {
        t_moveto(&a->fb, row, rx);
        t_reset(&a->fb);
        put_padded(&a->fb, "", rgt - rx + 1);
    }
    int top = draw_opening_name(a, rx, rgt);
    if (a->mode == MODE_OPENINGS) {
        draw_openings(a, top, rx, bottom, rgt);
    } else {
        int eng_h = a->ana.want ? 6 : 0;
        int note_h = note_panel_rows(a, rgt - rx - 1);
        /* The engine panel comes first: an annotation gives up its rows before
           the notation is squeezed below a usable height. */
        while (note_h > 0 && bottom - eng_h - note_h - top < 4) note_h--;
        if (note_h < 3) note_h = 0;
        draw_notation(a, top, rx, bottom - eng_h - note_h, rgt);
        if (note_h)
            draw_note_panel(a, bottom - eng_h - note_h + 1, rx, bottom - eng_h, rgt);
        if (eng_h) draw_analysis(a, bottom - eng_h + 1, rx, bottom, rgt);
    }
    draw_bottom(a, cur_r, cur_c);
    if (a->modal == MODAL_NOTE) draw_note_editor(a, cur_r, cur_c);
}

/* ------------------------------ menu ------------------------------ */
static int menu_row(int item) {
    /* Separate the play modes and the quit action with blank rows. */
    return item + (item >= MI_LOCAL) + (item >= MI_ONLINE) + (item >= MI_QUIT);
}

static void draw_menu(App *a, int *cur_r, int *cur_c) {
    SB *b = &a->fb;
    const int MW = 68;                       /* menu block width */
    int cx = (a->tw - MW) / 2 + 1;
    if (cx < 2) cx = 2;
    int avail = a->tw - cx - 1;
    int r = a->th >= 30 ? 3 : 2;

    t_moveto(b, r, cx);
    t_style_fg(b, CL_ACC, A_BOLD);
    sb_put(b, "♞  C H E S S T T Y");
    t_moveto(b, r + 1, cx);
    t_style_fg(b, CL_DIM, 0);
    sb_put(b, "   chess in your terminal · Stockfish built in");
    r += 3;

    static const char *items[MI_COUNT] = {
        "Play against Stockfish", "Difficulty", "Color",
        "Local two-player game", "Minutes per player", "Increment (seconds)", "Play online on Lichess",
        "Analysis board", "Load PGN…", "Load FEN…", "Game database…",
        "Opening explorer", "Quit"
    };
    static const char *hints[MI_COUNT] = {
        "", "", "", "two players on this keyboard", "", "", "browser login and human opponents",
        "empty board, both sides, engine lines",
        "open a game and analyse or annotate it",
        "set up a position from a FEN string",
        "browse, search and store your games",
        "", ""
    };
    int label_w = 46;
    if (label_w > avail - 2) label_w = avail - 2;
    if (label_w < 24) label_w = 24;
    /* The right-hand column explains an entry; a clipped explanation is worse
       than none, so it disappears when there is not enough room for it. */
    int hint_w = avail - label_w - 2;
    if (hint_w < 30) hint_w = 0;

    int visible = a->th - r - 7;
    if (visible < 1) visible = 1;
    if (a->menu_item < a->menu_scroll) a->menu_scroll = a->menu_item;
    while (menu_row(a->menu_item) - menu_row(a->menu_scroll) >= visible)
        a->menu_scroll++;
    while (a->menu_scroll > 0 &&
           menu_row(MI_COUNT - 1) - menu_row(a->menu_scroll - 1) < visible)
        a->menu_scroll--;
    for (int row = r; row < r + visible; row++) {
        t_moveto(b, row, cx);
        t_reset(b);
        put_padded(b, "", avail);
    }
    for (int i = a->menu_scroll; i < MI_COUNT; i++) {
        int offset = menu_row(i) - menu_row(a->menu_scroll);
        if (offset >= visible) break;
        int row = r + offset;
        t_moveto(b, row, cx);
        bool sel = a->menu_item == i && a->prompt == PROMPT_NONE;
        t_style_fg(b, sel ? CL_ACC : CL_TXT, sel ? A_BOLD : 0);
        sb_printf(b, "%s ", sel ? "▸" : " ");
        char line[160];
        if (i == MI_DIFF) {
            char bar[64];                    /* 16 cells × 3 UTF-8 bytes + NUL */
            int fill = (a->menu_diff * 16 + 99) / 100;   /* 1..16 */
            size_t bl = 0;
            for (int j = 0; j < 16; j++) {
                const char *cell = j < fill ? "█" : "░";
                memcpy(bar + bl, cell, 3);
                bl += 3;
            }
            bar[bl] = 0;
            snprintf(line, sizeof line, "Difficulty  ‹%s› %3d", bar, a->menu_diff);
            put_padded(b, line, label_w);
            t_style_fg(b, CL_DIM, 0);
            char sub[64];
            if (a->menu_diff >= 95) snprintf(sub, sizeof sub, "%s", diff_label(a->menu_diff));
            else snprintf(sub, sizeof sub, "%s · ~%d Elo", diff_label(a->menu_diff),
                          diff_elo(a->menu_diff));
            put_padded(b, sub, hint_w ? hint_w : 0);
        } else if (i == MI_TIME || i == MI_INCREMENT) {
            snprintf(line, sizeof line, "%s  ‹ %d ›", items[i], i == MI_TIME ? a->menu_minutes : a->menu_increment);
            put_padded(b, line, label_w); put_padded(b, "", hint_w);
        } else if (i == MI_COLOR) {
            static const char *cols[3] = { "White", "Black", "Random" };
            snprintf(line, sizeof line, "Color       ‹ %s ›", cols[a->menu_color]);
            put_padded(b, line, label_w);
            t_reset(b);
            put_padded(b, "", hint_w);
        } else {
            put_padded(b, items[i], label_w);
            t_style_fg(b, sel ? CL_TXT : CL_DIM, 0);
            put_padded(b, hint_w ? hints[i] : "", hint_w);
        }
    }
    int ry = r + visible + 1;

    /* prompt */
    t_moveto(b, ry, cx);
    if (a->prompt != PROMPT_NONE) {
        const char *label = a->prompt == PROMPT_PGN ? "PGN file › " :
                            a->prompt == PROMPT_FEN ? "FEN › " : "Collection › ";
        t_style_fg(b, CL_ACC, A_BOLD);
        sb_put(b, label);
        int lw = utf8_cols(label);
        int w = avail - lw;
        if (w < 10) w = 10;
        if (a->path_len > 0) {
            t_style_fg(b, CL_TXT, 0);
            put_padded(b, a->path_input, w);
        } else {
            t_style_fg(b, CL_DIM, 0);
            put_padded(b, a->prompt == PROMPT_FEN
                       ? "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"
                       : a->prompt == PROMPT_PGN ? "path to a .pgn file"
                       : "a folder of PGN files, or a single .pgn", w);
        }
        *cur_r = ry;
        *cur_c = cx + lw + a->path_len;
    } else {
        t_reset(b);
        put_padded(b, "", avail);
    }

    /* engine status */
    t_moveto(b, ry + 2, cx);
    if (a->engine_probed) {
        t_style_fg(b, CL_OK, 0);
        char stat[160];
        snprintf(stat, sizeof stat, "✓ engine ready: %s", a->engine_name);
        put_padded(b, stat, avail);
    } else {
        t_style_fg(b, CL_ERR, A_BOLD);
        put_padded(b, "✗ engine not found — run `make engine` in the project folder", avail);
    }
    t_moveto(b, ry + 3, cx);
    if (a->msg[0]) {
        t_style_fg(b, a->msg_kind == 1 ? CL_ERR : a->msg_kind == 2 ? CL_OK : CL_DIM, 0);
        put_padded(b, a->msg, avail);
    } else {
        t_reset(b);
        put_padded(b, "", avail);
    }

    t_moveto(b, a->th, 2);
    t_style_fg(b, CL_DIM, 0);
    put_padded(b, "", a->tw - 2);
    t_reset(b);
}

/* ------------------------------ Lichess lobby ------------------------------ */
static void draw_online(App *a) {
    SB *b = &a->fb;
    draw_box(a, 1, 2, a->th - 1, a->tw - 1, "Play online · Lichess", CL_PANEL);
    t_moveto(b, 3, 5); t_style_fg(b, CL_ACC, A_BOLD);
    char status[160];
    snprintf(status, sizeof status, "%s%s", a->online.authenticated ? "Logged in as " : "Not logged in",
             a->online.authenticated ? a->online.user : "");
    put_padded(b, status, a->tw - 10);
    static const char *items[] = {"Log in with Lichess", "Find an opponent (casual)", "Resume / reconnect game",
        "Cancel matchmaking", "Minutes per player", "Increment (seconds)", "Color", "Log out", "Back"};
    for (int i = 0; i < 9; i++) {
        t_moveto(b, 5 + i, 5); t_style_fg(b, a->online_item == i ? CL_ACC : CL_TXT, a->online_item == i ? A_BOLD : 0);
        char line[120];
        if (i == 4 || i == 5) snprintf(line, sizeof line, "%s %s  ‹ %d ›", a->online_item == i ? "▸" : " ", items[i], i == 4 ? a->menu_minutes : a->menu_increment);
        else if (i == 6) snprintf(line, sizeof line, "%s Color  ‹ %s ›", a->online_item == i ? "▸" : " ", a->menu_color == 0 ? "White" : a->menu_color == 1 ? "Black" : "Random");
        else snprintf(line, sizeof line, "%s %s", a->online_item == i ? "▸" : " ", items[i]);
        put_padded(b, line, a->tw - 10);
    }
    t_moveto(b, 16, 5); t_style_fg(b, CL_DIM, 0);
    put_padded(b, "Public games: Rapid / Classical · engine assistance disabled", a->tw - 10);
    t_moveto(b, 18, 5);
    put_padded(b, a->online.connecting ? "Waiting for browser authorization…" : a->online.seeking ? "Looking for an opponent…" : "", a->tw - 10);
    t_moveto(b, a->th - 2, 5);
    put_padded(b, a->msg[0] ? a->msg : a->online.message, a->tw - 10);
    t_reset(b);
}

/* ------------------------------ games database ------------------------------ */
static void draw_db(App *a, int *cur_r, int *cur_c) {
    SB *b = &a->fb;
    const Db *db = &a->db;
    int r1 = a->th - 2, c1 = a->tw - 1;
    char title[160];
    snprintf(title, sizeof title, "Collection · %s", db->dir[0] ? db->dir : "-");
    draw_box(a, 1, 2, r1, c1, title, CL_PANEL);
    int iw = c1 - 3;                      /* usable inner width */

    /* filter row */
    t_moveto(b, 2, 3);
    if (a->db_filtering || a->db_filter_len > 0) {
        t_style_fg(b, CL_ACC, A_BOLD);
        sb_put(b, "search › ");
        t_style_fg(b, a->db_filtering ? CL_TXT : CL_DIM, 0);
        int width = iw > 10 ? iw - 10 : 1;
        int start = a->db_filter_len > width ? a->db_filter_len - width : 0;
        put_padded(b, a->db_filter + start, iw - 9);
        if (a->db_filtering) { *cur_r = 2; *cur_c = 3 + 9 + a->db_filter_len - start; }
    } else {
        t_style_fg(b, CL_DIM, 0);
        put_padded(b, "Search all fields", iw);
    }

    /* Columns shrink with the terminal; the three name columns share what is
       left once the fixed ones are placed. */
    int wnum = 5, wres = 7, wdate = 11, wply = 5;
    int rest = iw - wnum - wres - wdate - wply - 6;   /* six single-space gaps */
    if (rest < 18) rest = 18;
    int wwhite = rest * 36 / 100, wblack = rest * 36 / 100;
    if (wwhite > 26) wwhite = 26;
    if (wblack > 26) wblack = 26;
    int wevent = rest - wwhite - wblack;
    if (wevent < 0) wevent = 0;

    static const char *heads[DBS_COUNT] = { "#", "White", "Black", "Event", "Date", "Len" };
    t_moveto(b, 3, 3);
    const struct { const char *txt; int w; int key; } cols[] = {
        { heads[0], wnum,   DBS_NATURAL },
        { heads[1], wwhite, DBS_WHITE },
        { heads[2], wblack, DBS_BLACK },
        { "Res",    wres,   -1 },
        { heads[4], wdate,  DBS_DATE },
        { heads[5], wply,   DBS_PLIES },
        { heads[3], wevent, DBS_EVENT },
    };
    size_t ncols = sizeof cols / sizeof cols[0];
    for (size_t i = 0; i < ncols; i++) {
        bool on = cols[i].key >= 0 && cols[i].key == db->sort;
        t_style_fg(b, on ? CL_ACC : CL_DIM, on ? A_BOLD : 0);
        char h[40];
        if (on) snprintf(h, sizeof h, "%s%s", cols[i].txt, db->sort_desc ? "↓" : "↑");
        else snprintf(h, sizeof h, "%s", cols[i].txt);
        put_padded(b, h, cols[i].w);
        t_reset(b);
        if (i + 1 < ncols) sb_put(b, " ");
    }

    int first_row = 4, vis = r1 - first_row - 1;
    if (vis < 1) vis = 1;
    if (a->db_idx < a->db_scroll) a->db_scroll = a->db_idx;
    if (a->db_idx >= a->db_scroll + vis) a->db_scroll = a->db_idx - vis + 1;
    if (a->db_scroll > db->nview - vis) a->db_scroll = db->nview - vis;
    if (a->db_scroll < 0) a->db_scroll = 0;

    for (int i = 0; i < vis; i++) {
        int idx = a->db_scroll + i;
        t_moveto(b, first_row + i, 3);
        const DbRec *rec = db_at(db, idx);
        if (!rec) { t_reset(b); put_padded(b, "", iw); continue; }
        bool sel = idx == a->db_idx;
        char num[16], date[16], ply[8];
        snprintf(num, sizeof num, "%d", idx + 1);
        if (rec->year && rec->month && rec->day)
            snprintf(date, sizeof date, "%04d.%02d.%02d", rec->year, rec->month, rec->day);
        else if (rec->year) snprintf(date, sizeof date, "%04d", rec->year);
        else snprintf(date, sizeof date, "-");
        snprintf(ply, sizeof ply, "%d", (rec->plies + 1) / 2);
        const char *res = rec->result == DBR_WHITE ? "1-0" :
                          rec->result == DBR_BLACK ? "0-1" :
                          rec->result == DBR_DRAW ? "½-½" : "*";
        if (sel) t_style(b, CL_SEL_FG, CL_SEL_BG, A_BOLD);
        else t_style_fg(b, CL_TXT, 0);
        put_padded(b, num, wnum);    sb_put(b, " ");
        put_padded(b, rec->white, wwhite); sb_put(b, " ");
        put_padded(b, rec->black, wblack); sb_put(b, " ");
        put_padded(b, res, wres);    sb_put(b, " ");
        put_padded(b, date, wdate);  sb_put(b, " ");
        put_padded(b, ply, wply);    sb_put(b, " ");
        put_padded(b, rec->event, wevent);
        t_reset(b);
    }

    /* Metadata of the selected game, including fields hidden from the table. */
    if (r1 > first_row + 1) {
        t_moveto(b, r1 - 1, 3);
        t_style_fg(b, CL_DIM, 0);
        const DbRec *rec = db_at(db, a->db_idx);
        char detail[1024] = "";
        if (rec) {
            char white_elo[8] = "?", black_elo[8] = "?";
            if (rec->white_elo) snprintf(white_elo, sizeof white_elo, "%u", rec->white_elo);
            if (rec->black_elo) snprintf(black_elo, sizeof black_elo, "%u", rec->black_elo);
            snprintf(detail, sizeof detail, "Elo %s/%s · ECO %s · %s · round %s · %s",
                     white_elo, black_elo, *rec->eco ? rec->eco : "?",
                     *rec->site ? rec->site : "?", *rec->round ? rec->round : "?",
                     rec->file_id < (uint32_t)db->nfiles ? db->files[rec->file_id].name : "?");
        }
        put_padded(b, detail, iw);
    }

    /* Counts and feedback stay above the always-visible command row. */
    t_moveto(b, a->th - 1, 2);
    t_style_fg(b, a->msg[0] && a->msg_kind == 1 ? CL_ERR : CL_DIM, 0);
    char stat[320];
    snprintf(stat, sizeof stat, "%d/%d games · sorted by %s %s%s%s",
             db->nview, db->nrecs, db_sort_name(db->sort),
             db->sort_desc ? "↓" : "↑", a->msg[0] ? " · " : "", a->msg);
    put_padded(b, stat, a->tw - 2);

    t_reset(b);
}

/* ------------------------------ game picker ------------------------------ */
static void draw_picker(App *a) {
    SB *b = &a->fb;
    char title[96];
    snprintf(title, sizeof title, "Pick a game · %d in the file", a->plist ? a->plist->n : 0);
    int r1 = a->th - 2;
    draw_box(a, 1, 2, r1, a->tw - 1, title, CL_PANEL);
    int vis = r1 - 2;
    int n = a->plist ? a->plist->n : 0;
    if (a->pick_idx < a->pick_scroll) a->pick_scroll = a->pick_idx;
    if (a->pick_idx >= a->pick_scroll + vis) a->pick_scroll = a->pick_idx - vis + 1;
    for (int i = 0; i < vis; i++) {
        int idx = a->pick_scroll + i;
        t_moveto(b, 2 + i, 3);
        if (idx >= n) { t_reset(b); put_padded(b, "", a->tw - 4); continue; }
        const PgnRef *g = &a->plist->games[idx];
        char line[400];   /* four PGN tags plus separators */
        snprintf(line, sizeof line, "%4d. %s — %s   %s   %s", idx + 1,
                 g->white, g->black, g->result, g->event);
        if (idx == a->pick_idx) {
            t_style(b, CL_SEL_FG, CL_SEL_BG, A_BOLD);
            put_padded(b, line, a->tw - 4);
            t_reset(b);
        } else {
            t_style_fg(b, CL_TXT, 0);
            put_padded(b, line, a->tw - 4);
        }
    }
    t_moveto(b, a->th, 2);
    t_style_fg(b, CL_DIM, 0);
    put_padded(b, "", a->tw - 2);
    t_reset(b);
}

/* ------------------------------ frame ------------------------------ */
void draw_frame(App *a) {
    SB *b = &a->fb;
    sb_reset(b);
    sb_put(b, "\x1b[?25l");
    if (a->force_clear) {
        t_reset(b);
        t_clear_all(b);
        a->force_clear = false;
    }
    int cur_r = 0, cur_c = 0;

    if (a->tw < 78 || a->th < 22) {
        t_moveto(b, 1, 1);
        t_clear_all(b);
        t_style_fg(b, CL_ERR, A_BOLD);
        t_moveto(b, a->th / 2, 2);
        sb_printf(b, "Terminal too small (%dx%d): at least 78x22 is needed", a->tw, a->th);
        t_reset(b);
        term_frame_flush(b, 0, 0);
        return;
    }
    switch (a->screen) {
        case SCR_MENU: draw_menu(a, &cur_r, &cur_c); break;
        case SCR_PICKER: draw_picker(a); break;
        case SCR_DB: draw_db(a, &cur_r, &cur_c); break;
        case SCR_GAME: draw_game(a, &cur_r, &cur_c); break;
        case SCR_ONLINE: draw_online(a); break;
    }
    /* A single entry point in the top border replaces footer help lists. */
    t_moveto(b, 1, a->tw - 16);
    t_style_fg(b, CL_DIM, 0);
    sb_put(b, " C-x commands ");
    t_moveto(b, a->th, 1);
    t_reset(b);
    t_clear_eol(b);
    if (a->commands_open) draw_commands(a, &cur_r, &cur_c);
    t_reset(b);
    term_frame_flush(b, cur_r, cur_c);
}
