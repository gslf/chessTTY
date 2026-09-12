/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "app.h"

enum { MENU = 1, DB = 2, PICKER = 4, PLAY = 8, ANALYSIS = 16,
       OPENINGS = 32, EDIT = 64, CONFIRM = 128, PROMOTION = 256, LOCAL = 512, ONLINE = 1024, ONLINE_GAME = 2048 };
#define BOARD (PLAY | ANALYSIS | OPENINGS | LOCAL | ONLINE_GAME)

int app_commands(const App *a, Command out[COMMAND_MAX]) {
    static const struct { unsigned context; Command cmd; } bindings[] = {
        {MENU, {'h', "Local two-player game", CMD_LOCAL}},
        {MENU, {'l', "Play online on Lichess", CMD_ONLINE}},
        {ONLINE, {'l', "Log in with Lichess", CMD_LOGIN}},
        {ONLINE, {'s', "Find an opponent", CMD_SEEK}},
        {ONLINE, {'r', "Resume / reconnect game", CMD_RESUME}},
        {ONLINE, {'c', "Cancel matchmaking", CMD_CANCEL_SEEK}},
        {ONLINE, {'o', "Log out", CMD_LOGOUT}},
        {ONLINE_GAME, {'z', "Resign game…", CMD_RESIGN}},
        {ONLINE_GAME, {'d', "Offer / accept draw", CMD_DRAW}},
        {ONLINE_GAME, {'x', "Abort unstarted game", CMD_ABORT}},
        {MENU, {'p', "Play against Stockfish", CMD_PLAY}},
        {MENU, {'a', "Analysis board", CMD_ANALYSIS}},
        {MENU, {'f', "Open PGN file", CMD_PGN}},
        {MENU, {'e', "Load FEN position", CMD_FEN}},
        {MENU, {'b', "Open game database", CMD_DB}},
        {MENU, {'o', "Opening explorer", CMD_OPENINGS}},
        {DB, {'s', "Search all fields", CMD_SEARCH}},
        {DB, {'d', "Sort by date / toggle direction", CMD_DATE}},
        {DB, {'o', "Cycle sort field", CMD_SORT}},
        {DB, {'r', "Reverse sort order", CMD_REVERSE}},
        {DB, {'i', "Rebuild database index", CMD_REINDEX}},
        {PLAY | ANALYSIS, {'v', "Toggle engine analysis", CMD_LINES}},
        {BOARD, {'w', "Write PGN file", CMD_SAVE}},
        {BOARD, {'b', "Save to game database", CMD_STORE}},
        {PLAY | ANALYSIS, {'a', "Edit position annotation", CMD_NOTE}},
        {PLAY | ANALYSIS, {'m', "Cycle move annotation glyph", CMD_NAG}},
        {PLAY | ANALYSIS, {'u', "Take back move", CMD_UNDO}},
        {BOARD, {'r', "Rotate board", CMD_ROTATE}},
        {BOARD, {'p', "Cycle piece style", CMD_PIECES}},
        {BOARD, {'+', "Larger board", CMD_LARGER}},
        {BOARD, {'-', "Smaller board", CMD_SMALLER}},
        {PLAY | LOCAL, {'n', "New game", CMD_NEW}},
        {DB | PICKER | BOARD | ONLINE, {'q', "Back to previous screen", CMD_BACK}},
        {MENU, {'q', "Quit ChessTTY", CMD_BACK}},
        {EDIT, {'c', "Confirm input", CMD_ACCEPT}},
        {CONFIRM, {'y', "Confirm", CMD_YES}},
        {PROMOTION, {'q', "Promote to queen", CMD_QUEEN}},
        {PROMOTION, {'r', "Promote to rook", CMD_ROOK}},
        {PROMOTION, {'b', "Promote to bishop", CMD_BISHOP}},
        {PROMOTION, {'n', "Promote to knight", CMD_KNIGHT}},
        {EDIT | CONFIRM | PROMOTION, {'g', "Cancel input", CMD_CANCEL}},
    };
    unsigned context;
    if (a->modal == MODAL_PROMO) context = PROMOTION;
    else if (a->modal == MODAL_CONFIRM_QUIT || a->modal == MODAL_CONFIRM_NEW || a->modal == MODAL_RESIGN) context = CONFIRM;
    else if (a->modal == MODAL_NOTE || a->modal == MODAL_SAVE ||
             (a->screen == SCR_MENU && a->prompt != PROMPT_NONE) ||
             (a->screen == SCR_DB && a->db_filtering)) context = EDIT;
    else if (a->screen == SCR_ONLINE) context = ONLINE;
    else if (a->screen == SCR_MENU) context = MENU;
    else if (a->screen == SCR_DB) context = DB;
    else if (a->screen == SCR_PICKER) context = PICKER;
    else if (a->mode == MODE_LOCAL) context = LOCAL;
    else if (a->mode == MODE_ONLINE) context = ONLINE_GAME;
    else context = a->mode == MODE_PLAY ? PLAY : a->mode == MODE_ANALYZE ? ANALYSIS : OPENINGS;
    _Static_assert(sizeof bindings / sizeof *bindings <= COMMAND_MAX, "Command buffer too small");
    int count = 0;
    for (size_t i = 0; i < sizeof bindings / sizeof *bindings; i++)
        if (bindings[i].context & context) out[count++] = bindings[i].cmd;
    return count;
}
