/* app.h — application state */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef APP_H
#define APP_H

#include "engine.h"
#include "game.h"
#include "pgn.h"
#include "term.h"

typedef enum { SCR_MENU, SCR_PICKER, SCR_GAME } Screen;
typedef enum { MODE_PLAY, MODE_ANALYZE } GameMode;
typedef enum {
    MODAL_NONE, MODAL_PROMO, MODAL_SAVE,
    MODAL_CONFIRM_QUIT, MODAL_CONFIRM_NEW, MODAL_HELP
} Modal;
typedef enum { ANA_IDLE = 0, ANA_RUNNING, ANA_STOPPING } AnaState;
typedef enum { OPP_IDLE = 0, OPP_THINKING } OppState;

typedef struct {
    Engine eng;
    AnaState state;
    bool want;            /* panel visible */
    bool pending;         /* restart the search after the bestmove */
    EngineLine lines[3];
    Pos base;             /* position being analysed */
} Ana;

typedef struct {
    Engine eng;
    OppState state;
    int difficulty;       /* 1..100 */
    int side;             /* +1/-1 */
} Opp;

typedef struct App {
    Screen screen;
    GameMode mode;
    Modal modal;
    Game game;
    Ana ana;
    Opp opp;
    int player_side;

    int tw, th;
    bool dirty, quit, force_clear;

    char input[24];  int input_len;      /* typed move */
    char msg[200];   int msg_kind;       /* 0 info, 1 error, 2 ok */

    /* menu */
    int menu_item;                       /* 0 play 1 difficulty 2 color 3 pgn 4 quit */
    int menu_diff, menu_color;           /* color: 0 white 1 black 2 random */
    char path_input[512]; int path_len;  bool path_editing;

    /* game picker for multi-game PGN files */
    PgnList *plist;
    int pick_idx, pick_scroll;
    char loaded_path[512];

    /* graphics */
    int piece_style;                     /* 0 coloured glyphs, 1 print style, 2 letters */
    bool flip;
    int board_size;                      /* wanted size 0..2; drawing clamps to what fits */

    /* pending promotion */
    char promo_base[24];

    /* saving */
    char save_input[160]; int save_len;

    char engine_path[1100];
    bool engine_probed;
    char engine_name[64];

    long long spin_last; int spin;
    SB fb;
} App;

/* draw.c */
void draw_frame(App *a);
int  board_max_size(const App *a);   /* largest size that fits the terminal */
int  board_eff_size(const App *a);   /* min(board_size, board_max_size) */

/* main.c */
int diff_elo(int d);
const char *diff_label(int d);

#endif
