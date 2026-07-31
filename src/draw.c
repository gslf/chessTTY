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
static const char *SPIN[10] = { "⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏" };

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
        if (v >= 100) v = 99.9; if (v <= -100) v = -99.9;
        snprintf(out, n, "%+.2f", v);
    }
}

/* ------------------------------ board ------------------------------ */
static void draw_player_line(App *a, int row, int side) {
    SB *b = &a->fb;
    Game *g = &a->game;
    char nm[32];
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
    /* status to the right of the name */
    if (a->mode == MODE_PLAY && side == a->opp.side && a->opp.state == OPP_THINKING) {
        t_style_fg(b, CL_ACC, A_BOLD);
        sb_printf(b, "%s thinking", SPIN[a->spin]);
    } else if (turn) {
        t_style_fg(b, CL_ACC, 0);
        put_padded(b, "● to move", 10);
    } else {
        t_reset(b);
        put_padded(b, "", 10);
    }
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
    snprintf(title, sizeof title, "Moves  %d/%d", g->view, g->n);
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
        char num[8];
        snprintf(num, sizeof num, "%3d.", g->start.fullmove + r);
        t_style_fg(b, CL_DIM, 0);
        sb_put(b, num);
        sb_put(b, " ");
        int used = (int)strlen(num) + 1;
        for (int col = 0; col < 2; col++) {
            int ply = r * 2 + col + 1 - offset; /* 1-based */
            char cell[20];
            if (ply < 1 || ply > g->n) snprintf(cell, sizeof cell, "%s", ply < 1 ? "…" : "");
            else snprintf(cell, sizeof cell, "%s", g->san[ply - 1]);
            if (ply >= 1 && ply <= g->n && ply == g->view) {
                t_style(b, CL_SEL_FG, CL_SEL_BG, A_BOLD);
                put_padded(b, cell, wsan);
                t_reset(b);
            } else {
                t_style_fg(b, ply >= 1 && ply <= g->n ? CL_TXT : CL_DIM, 0);
                put_padded(b, cell, wsan);
            }
            sb_put(b, " ");
            used += wsan + 1;
        }
        t_reset(b);
        for (; used < iw; used++) sb_put(b, " ");
    }
}

/* ------------------------------ analysis panel ------------------------------ */
static void draw_analysis(App *a, int r0, int c0, int r1, int c1) {
    SB *b = &a->fb;
    char title[80];
    int depth = a->ana.lines[0].valid ? a->ana.lines[0].depth : 0;
    if (depth > 0)
        snprintf(title, sizeof title, "Stockfish · depth %d", depth);
    else
        snprintf(title, sizeof title, "Stockfish");
    draw_box(a, r0, c0, r1, c1, title, CL_PANEL);
    int iw = c1 - c0 - 1;

    const Pos *p = &a->game.pos[a->game.view];
    Move l[MAX_MOVES];
    bool over = gen_legal(p, l) == 0;

    /* row 1: evaluation bar */
    t_moveto(b, r0 + 1, c0 + 1);
    char sc[16];
    score_str(&a->ana.lines[0], sc, sizeof sc);
    int barw = iw - 8;
    if (barw < 6) barw = 6;
    double wp = 0.5;
    if (over) {
        GameState st = pos_state(p);
        wp = st == GS_CHECKMATE ? (p->stm > 0 ? 0.0 : 1.0) : 0.5;
        snprintf(sc, sizeof sc, "%s", st == GS_CHECKMATE ? "#" : "=");
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
                GameState st = pos_state(p);
                t_style_fg(b, CL_ACC, A_BOLD);
                put_padded(b, st == GS_CHECKMATE ? "Checkmate" : "No legal moves", iw);
            } else { t_reset(b); put_padded(b, "", iw); }
            continue;
        }
        const EngineLine *L = &a->ana.lines[i];
        t_style_fg(b, CL_DIM, 0);
        sb_printf(b, "%d ", i + 1);
        char s2[16];
        score_str(L, s2, sizeof s2);
        char left[16];
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
    int msg_row = a->th - 2, in_row = a->th - 1, help_row = a->th;

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
    } else if (a->modal == MODAL_PROMO) {
        t_style_fg(b, CL_ACC, A_BOLD);
        put_padded(b, "Promote to: [Q]ueen  [R]ook  [B]ishop  k[N]ight   (Esc cancels)", a->tw - 2);
    } else if (a->modal == MODAL_CONFIRM_QUIT) {
        t_style_fg(b, CL_ERR, A_BOLD);
        put_padded(b, "Return to the menu? The game in progress will be lost · [y] yes  [n] no", a->tw - 2);
    } else if (a->modal == MODAL_CONFIRM_NEW) {
        t_style_fg(b, CL_ACC, A_BOLD);
        put_padded(b, "Start a new game? · [y] yes  [n] no", a->tw - 2);
    } else if (a->mode == MODE_PLAY) {
        t_style_fg(b, CL_ACC, A_BOLD);
        sb_put(b, "› ");
        t_style_fg(b, CL_TXT, A_BOLD);
        if (a->input_len > 0) put_padded(b, a->input, a->tw - 6);
        else {
            t_style_fg(b, CL_DIM, 0);
            put_padded(b, g->result == RES_ONGOING ?
                       "type a move (e4, Nf3, O-O) and press Enter" :
                       "game over · s save · n new game · q menu", a->tw - 6);
        }
        *cur_r = in_row;
        *cur_c = 4 + a->input_len;
    } else {
        t_style_fg(b, CL_DIM, 0);
        char nav[120];
        snprintf(nav, sizeof nav, "position %d/%d · ←→ browse · Home/End first/last",
                 g->view, g->n);
        put_padded(b, nav, a->tw - 2);
    }

    /* help row */
    t_moveto(b, help_row, 2);
    t_style_fg(b, CL_DIM, 0);
    const char *help;
    if (a->mode == MODE_PLAY)
        help = "←→ browse · v lines · +- board · z undo · s save · r rotate · u pieces · n new · q menu · ? help";
    else
        help = "←→ browse · v lines · +- board · s save · r rotate · u pieces · q menu · ? help";
    put_padded(b, help, a->tw - 2);
    t_reset(b);
}

/* ------------------------------ help overlay ------------------------------ */
static void draw_help(App *a) {
    static const char *L[] = {
        "",
        "  Moves       type them in notation (e4, Nf3, O-O, exd5,",
        "              e8=Q or e2e4) and press Enter",
        "  ← →         browse the game · space: next move",
        "  Home / End  jump to the start / the end",
        "  PgUp / PgDn skip 6 plies",
        "",
        "  v           show/hide Stockfish's 3 best lines",
        "  + / -       board size (starts at the largest that fits)",
        "  z           take back your last move",
        "  s           save the game as PGN",
        "  r           rotate the board",
        "  u           piece style (coloured / print / letters)",
        "  n           new game",
        "  q / Esc     back to the menu",
        "",
    };
    int nl = (int)(sizeof L / sizeof L[0]);
    int w = 64, h = nl + 2;
    int r0 = (a->th - h) / 2 + 1, c0 = (a->tw - w) / 2 + 1;
    if (r0 < 1) r0 = 1;
    if (c0 < 1) c0 = 1;
    draw_box(a, r0, c0, r0 + h - 1, c0 + w - 1, "Help", CL_ACC);
    SB *b = &a->fb;
    for (int i = 0; i < nl; i++) {
        t_moveto(b, r0 + 1 + i, c0 + 1);
        t_style_fg(b, CL_TXT, 0);
        put_padded(b, L[i], w - 2);
    }
    t_moveto(b, r0 + h - 1, c0 + 4);
    t_style_fg(b, CL_DIM, 0);
    sb_put(b, "╴press any key╶");
    t_reset(b);
}

/* ------------------------------ game screen ------------------------------ */
static void draw_game(App *a, int *cur_r, int *cur_c) {
    draw_board(a);
    int rx = BL + 8 * CELL_W[board_eff_size(a)] + 4; /* left column of the panels */
    int rgt = a->tw - 1;
    if (rgt > rx + 70) rgt = rx + 70;     /* don't grow too wide */
    int bottom = a->th - 3;
    if (a->ana.want) {
        draw_notation(a, 1, rx, bottom - 6, rgt);
        draw_analysis(a, bottom - 5, rx, bottom, rgt);
    } else {
        draw_notation(a, 1, rx, bottom, rgt);
    }
    draw_bottom(a, cur_r, cur_c);
    if (a->modal == MODAL_HELP) draw_help(a);
}

/* ------------------------------ menu ------------------------------ */
static void draw_menu(App *a, int *cur_r, int *cur_c) {
    SB *b = &a->fb;
    int cx = (a->tw - 58) / 2 + 1;
    if (cx < 2) cx = 2;
    int r = a->th >= 30 ? 3 : 2;

    t_moveto(b, r, cx);
    t_style_fg(b, CL_ACC, A_BOLD);
    sb_put(b, "♞  C H E S S T U I");
    t_moveto(b, r + 1, cx);
    t_style_fg(b, CL_DIM, 0);
    sb_put(b, "   chess in your terminal · Stockfish built in");
    r += 3;

    const char *items[5] = { "Play against Stockfish", "Difficulty", "Color",
                             "Analyze a game (PGN)", "Quit" };
    for (int i = 0; i < 5; i++) {
        t_moveto(b, r + i * 2, cx);
        bool sel = a->menu_item == i && !a->path_editing;
        t_style_fg(b, sel ? CL_ACC : CL_TXT, sel ? A_BOLD : 0);
        sb_printf(b, "%s ", sel ? "▸" : " ");
        char line[128];
        if (i == 1) {
            char bar[64] = "";
            int fill = (a->menu_diff + 4) / 5; /* 1..20 */
            char *o = bar;
            for (int j = 0; j < 20; j++)
                o += sprintf(o, "%s", j < fill ? "█" : "░");
            snprintf(line, sizeof line, "Difficulty  ‹%s› %3d", bar, a->menu_diff);
            put_padded(b, line, 44);
            t_style_fg(b, CL_DIM, 0);
            char sub[64];
            if (a->menu_diff >= 95)
                snprintf(sub, sizeof sub, "%s", diff_label(a->menu_diff));
            else
                snprintf(sub, sizeof sub, "%s · ~%d Elo", diff_label(a->menu_diff), diff_elo(a->menu_diff));
            t_moveto(b, r + i * 2 + 1, cx + 14);
            put_padded(b, sub, 42);
        } else if (i == 2) {
            const char *cols[3] = { "White", "Black", "Random" };
            snprintf(line, sizeof line, "Color       ‹ %s ›", cols[a->menu_color]);
            put_padded(b, line, 44);
        } else {
            put_padded(b, items[i], 44);
        }
    }
    int ry = r + 5 * 2;
    t_moveto(b, ry, cx);
    if (a->path_editing) {
        t_style_fg(b, CL_ACC, A_BOLD);
        sb_put(b, "PGN file › ");
        t_style_fg(b, CL_TXT, 0);
        int w = a->tw - cx - 12;
        put_padded(b, a->path_input, w > 10 ? w : 10);
        *cur_r = ry;
        *cur_c = cx + 11 + a->path_len;
    } else {
        t_reset(b);
        put_padded(b, "", a->tw - cx - 1);
    }
    ry += 2;

    /* engine status */
    t_moveto(b, ry + 1, cx);
    if (a->engine_probed) {
        t_style_fg(b, CL_OK, 0);
        char stat[160];
        snprintf(stat, sizeof stat, "✓ engine ready: %s", a->engine_name);
        put_padded(b, stat, a->tw - cx - 1);
    } else {
        t_style_fg(b, CL_ERR, A_BOLD);
        put_padded(b, "✗ engine not found — run `make engine` in the project folder",
                   a->tw - cx - 1);
    }
    if (a->msg[0]) {
        t_moveto(b, ry + 2, cx);
        t_style_fg(b, a->msg_kind == 1 ? CL_ERR : CL_DIM, 0);
        put_padded(b, a->msg, a->tw - cx - 1);
    }

    t_moveto(b, a->th, 2);
    t_style_fg(b, CL_DIM, 0);
    put_padded(b, "↑↓ select · ←→ adjust · Enter confirm · q quit", a->tw - 2);
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
        char line[256];
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
    put_padded(b, "↑↓ select · Enter open · Esc back", a->tw - 2);
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
        case SCR_GAME: draw_game(a, &cur_r, &cur_c); break;
    }
    t_reset(b);
    term_frame_flush(b, cur_r, cur_c);
}
