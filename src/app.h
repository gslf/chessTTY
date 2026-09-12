/* app.h — application state */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef APP_H
#define APP_H

#include "db.h"
#include "engine.h"
#include "game.h"
#include "pgn.h"
#include "term.h"
#include "openings.h"
#include "online.h"
#include "clocks.h"

typedef enum { SCR_MENU, SCR_PICKER, SCR_DB, SCR_GAME, SCR_ONLINE } Screen;
typedef enum { MODE_PLAY, MODE_ANALYZE, MODE_OPENINGS, MODE_LOCAL, MODE_ONLINE } GameMode;
typedef enum {
    MODAL_NONE, MODAL_PROMO, MODAL_SAVE, MODAL_NOTE,
    MODAL_CONFIRM_QUIT, MODAL_CONFIRM_NEW, MODAL_RESIGN
} Modal;

/* Main-menu rows.  Difficulty and Color are settings of the row above them. */
enum {
    MI_PLAY = 0, MI_DIFF, MI_COLOR, MI_LOCAL, MI_TIME, MI_INCREMENT, MI_ONLINE,
    MI_ANALYSIS, MI_PGN, MI_FEN, MI_DB,
    MI_OPENINGS, MI_QUIT, MI_COUNT
};

/* The single text field of the menu serves three different destinations. */
typedef enum { PROMPT_NONE = 0, PROMPT_PGN, PROMPT_FEN, PROMPT_DB } MenuPrompt;
typedef enum { ANA_IDLE = 0, ANA_RUNNING, ANA_STOPPING } AnaState;
typedef enum { OPP_IDLE = 0, OPP_THINKING } OppState;

typedef struct {
    Engine eng;
    AnaState state;
    bool want;            /* panel visible */
    bool pending;         /* restart the search after the bestmove */
    EngineLine lines[3];
    int64_t started_ms, elapsed_ms;
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
    ChessClock clock;
    Online online;
    int64_t clock_redraw;
    int player_side;

    int tw, th;
    bool dirty, quit, force_clear;
    bool commands_open, command_unknown;

    char input[24];  int input_len;      /* typed move */
    char msg[200];   int msg_kind;       /* 0 info, 1 error, 2 ok */

    /* menu */
    int menu_item;                       /* MI_* */
    int menu_minutes, menu_increment, menu_scroll, online_item;
    int menu_diff, menu_color;           /* color: 0 white 1 black 2 random */
    char path_input[512]; int path_len;
    MenuPrompt prompt;                   /* PROMPT_NONE = not editing */

    /* game picker for multi-game PGN files */
    PgnList *plist;
    int pick_idx, pick_scroll;
    char loaded_path[512];

    /* games database */
    Db db;
    int db_idx, db_scroll;
    bool db_filtering;                   /* the filter field has the keyboard */
    char db_filter[DB_FILTER_MAX]; int db_filter_len;

    bool from_db;                        /* the open game came from the collection */

    /* annotation editor */
    char note_input[NOTE_MAX]; int note_len; int note_ply;

    /* Opening explorer: current tree path and selected child at each ply. */
    uint16_t opening_path[MAXPLY + 1];
    int opening_selection[MAXPLY + 1];

    /* graphics */
    int piece_style;                     /* 0 coloured glyphs, 1 print style, 2 letters */
    bool flip;
    int board_size;                      /* wanted size 0..3; drawing clamps to what fits */

    /* pending promotion */
    char promo_base[24];

    /* saving */
    char save_input[160]; int save_len;

    char engine_path[1100];
    bool engine_probed;
    char engine_name[64];

    SB fb;
} App;

/* draw.c */
void draw_frame(App *a);
int  board_max_size(const App *a);   /* largest size that fits the terminal */
int  board_eff_size(const App *a);   /* min(board_size, board_max_size) */

/* main.c */
int diff_elo(int d);
const char *diff_label(int d);

/* One registry drives both prefix dispatch and the visible command palette. */
typedef enum {
    CMD_BACK, CMD_LOCAL, CMD_ONLINE, CMD_LOGIN, CMD_SEEK, CMD_RESUME, CMD_CANCEL_SEEK, CMD_LOGOUT, CMD_RESIGN, CMD_DRAW, CMD_ABORT, CMD_PLAY, CMD_ANALYSIS, CMD_PGN, CMD_FEN, CMD_DB, CMD_OPENINGS,
    CMD_SEARCH, CMD_DATE, CMD_SORT, CMD_REVERSE, CMD_REINDEX,
    CMD_LINES, CMD_ROTATE, CMD_PIECES, CMD_SAVE, CMD_STORE, CMD_NOTE, CMD_NAG,
    CMD_UNDO, CMD_LARGER, CMD_SMALLER, CMD_NEW,
    CMD_ACCEPT, CMD_CANCEL, CMD_YES, CMD_QUEEN, CMD_ROOK, CMD_BISHOP, CMD_KNIGHT
} CommandId;
typedef struct { char key; const char *label; CommandId id; } Command;
#define COMMAND_MAX 48
int app_commands(const App *a, Command out[COMMAND_MAX]);

#endif
