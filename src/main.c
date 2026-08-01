/* main.c — ChessTUI: chess in the terminal with Stockfish built in */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "app.h"
#include "platform.h"
#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef CHESSTUI_VERSION            /* set by the Makefile from the git tag */
#define CHESSTUI_VERSION "dev"
#endif

static App g_app;
static volatile sig_atomic_t g_sigint = 0;

/* ------------------------------ difficulty ------------------------------ */
int diff_elo(int d) {
    if (d >= 95) return 3600;
    return 1320 + (d - 1) * (3190 - 1320) / 93;
}
const char *diff_label(int d) {
    if (d <= 15) return "Beginner";
    if (d <= 30) return "Casual player";
    if (d <= 45) return "Club player";
    if (d <= 60) return "Strong club player";
    if (d <= 75) return "Candidate Master";
    if (d <= 89) return "Master";
    if (d <= 99) return "Grandmaster";
    return "Full-strength Stockfish";
}

/* ------------------------------ messages ------------------------------ */
static void set_msg(App *a, int kind, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->msg, sizeof a->msg, fmt, ap);
    va_end(ap);
    a->msg_kind = kind;
    a->dirty = true;
}

/* shows a message right away (for operations that block for a few seconds) */
static void flash_msg(App *a, const char *text) {
    set_msg(a, 0, "%s", text);
    draw_frame(a);
}

/* ------------------------------ engine binary lookup ------------------------------ */
static void find_engine(App *a, const char *cli_path) {
    const char *envp = getenv("CHESSTUI_ENGINE");
    /* Searched relative to the executable: next to it (release archive), one
       level up (build tree), then the `make install` layout
       (<prefix>/bin/chesstui with <prefix>/lib/chesstui/stockfish). */
#ifdef _WIN32
    static const char *const rel[] = { "engine\\stockfish.exe",
                                       "..\\engine\\stockfish.exe", NULL };
    const char *bare = "stockfish.exe";
#else
    static const char *const rel[] = { "engine/stockfish", "../engine/stockfish",
                                       "../lib/chesstui/stockfish", NULL };
    const char *bare = "stockfish";
#endif
    char cand[1100];
    if (cli_path && *cli_path) { snprintf(a->engine_path, sizeof a->engine_path, "%s", cli_path); return; }
    if (envp && *envp) { snprintf(a->engine_path, sizeof a->engine_path, "%s", envp); return; }
    char dir[1024];
    if (exe_dir(dir, sizeof dir)) {
        for (int i = 0; rel[i]; i++) {
            snprintf(cand, sizeof cand, "%s%s", dir, rel[i]);
            if (file_exists(cand)) {
                snprintf(a->engine_path, sizeof a->engine_path, "%s", cand);
                return;
            }
        }
    }
    if (file_exists(rel[0])) { snprintf(a->engine_path, sizeof a->engine_path, "%s", rel[0]); return; }
    snprintf(a->engine_path, sizeof a->engine_path, "%s", bare); /* last resort: PATH */
}

static void probe_engine(App *a) {
    Engine e;
    if (engine_start(&e, a->engine_path)) {
        snprintf(a->engine_name, sizeof a->engine_name, "%s", e.name[0] ? e.name : "UCI engine");
        a->engine_probed = true;
        engine_quit(&e);
    } else {
        a->engine_probed = false;
        a->engine_name[0] = 0;
    }
}

/* ------------------------------ analyser ------------------------------ */
static bool ana_ensure_started(App *a) {
    if (a->ana.eng.ok) return true;
    flash_msg(a, "Starting the analysis engine…");
    if (!engine_start(&a->ana.eng, a->engine_path)) {
        set_msg(a, 1, "Analysis unavailable: engine failed to start (%s)", a->engine_path);
        return false;
    }
    int th = cpu_count() / 2;
    if (th < 1) th = 1;
    if (th > 4) th = 4;
    engine_send(&a->ana.eng, "setoption name Threads value %d", th);
    engine_send(&a->ana.eng, "setoption name Hash value 256");
    engine_send(&a->ana.eng, "setoption name MultiPV value 3");
    engine_send(&a->ana.eng, "ucinewgame");
    engine_send(&a->ana.eng, "isready");
    engine_wait_for(&a->ana.eng, "readyok", 8000);
    a->ana.state = ANA_IDLE;
    return true;
}

static void ana_clear_lines(App *a) {
    memset(a->ana.lines, 0, sizeof a->ana.lines);
}

static void ana_start_search(App *a) {
    if (!a->ana.want || !a->ana.eng.ok) return;
    ana_clear_lines(a);
    a->ana.base = a->game.pos[a->game.view];
    Move l[MAX_MOVES];
    if (gen_legal(&a->ana.base, l) == 0) return; /* mate or stalemate: nothing to search */
    char cmd[10000];
    game_uci_position(&a->game, a->game.view, cmd, sizeof cmd);
    engine_send(&a->ana.eng, "%s", cmd);
    engine_send(&a->ana.eng, "go infinite");
    a->ana.state = ANA_RUNNING;
}

static void ana_position_changed(App *a) {
    ana_clear_lines(a);
    if (!a->ana.want || !a->ana.eng.ok) return;
    if (a->ana.state == ANA_RUNNING) {
        engine_send(&a->ana.eng, "stop");
        a->ana.state = ANA_STOPPING;
        a->ana.pending = true;
    } else if (a->ana.state == ANA_STOPPING) {
        a->ana.pending = true;
    } else {
        ana_start_search(a);
    }
}

static void ana_toggle(App *a) {
    a->ana.want = !a->ana.want;
    a->force_clear = true;
    if (a->ana.want) {
        if (!ana_ensure_started(a)) { a->ana.want = false; return; }
        ana_position_changed(a);
    } else if (a->ana.eng.ok && a->ana.state == ANA_RUNNING) {
        engine_send(&a->ana.eng, "stop");
        a->ana.state = ANA_STOPPING;
        a->ana.pending = false;
    }
    a->dirty = true;
}

static bool pump_analyzer(App *a) {
    if (!a->ana.eng.ok) return false;
    bool act = false;
    char line[8192];
    while (engine_poll_line(&a->ana.eng, line, sizeof line)) {
        if (!strncmp(line, "bestmove", 8)) {
            if (a->ana.state == ANA_STOPPING) {
                a->ana.state = ANA_IDLE;
                if (a->ana.pending) { a->ana.pending = false; ana_start_search(a); }
            } else a->ana.state = ANA_IDLE;
            act = true;
        } else if (a->ana.state == ANA_RUNNING &&
                   engine_parse_info(line, &a->ana.base, a->ana.lines)) {
            act = true;
        }
    }
    return act;
}

/* ------------------------------ opponent ------------------------------ */
static bool opp_start(App *a) {
    if (a->opp.eng.ok) return true;
    flash_msg(a, "Starting Stockfish…");
    if (!engine_start(&a->opp.eng, a->engine_path)) {
        set_msg(a, 1, "Engine failed to start (%s). Run `make engine` and try again.", a->engine_path);
        return false;
    }
    engine_send(&a->opp.eng, "setoption name Threads value 1");
    engine_send(&a->opp.eng, "setoption name Hash value 64");
    if (a->opp.difficulty < 95) {
        engine_send(&a->opp.eng, "setoption name UCI_LimitStrength value true");
        engine_send(&a->opp.eng, "setoption name UCI_Elo value %d", diff_elo(a->opp.difficulty));
    }
    engine_send(&a->opp.eng, "ucinewgame");
    engine_send(&a->opp.eng, "isready");
    engine_wait_for(&a->opp.eng, "readyok", 8000);
    a->opp.state = OPP_IDLE;
    return true;
}

static void opp_shutdown(App *a) {
    if (a->opp.eng.ok) engine_quit(&a->opp.eng);
    a->opp.state = OPP_IDLE;
}

static bool pump_opponent(App *a) {
    if (a->screen != SCR_GAME || a->mode != MODE_PLAY || !a->opp.eng.ok) return false;
    bool act = false;
    char line[4096];
    while (engine_poll_line(&a->opp.eng, line, sizeof line)) {
        if (!strncmp(line, "bestmove ", 9) && a->opp.state == OPP_THINKING) {
            a->opp.state = OPP_IDLE;
            char mv[8] = {0};
            sscanf(line + 9, "%7s", mv);
            Move m;
            int before = a->game.n;
            if (strcmp(mv, "(none)") != 0 && uci_to_move(&a->game.pos[a->game.n], mv, &m)) {
                bool follow = a->game.view == before;
                game_push(&a->game, m);
                if (follow) {
                    a->game.view = a->game.n;
                    ana_position_changed(a);
                } else {
                    set_msg(a, 0, "Stockfish played %s — press End to jump to the live position",
                            a->game.san[a->game.n - 1]);
                }
                if (a->game.result != RES_ONGOING)
                    set_msg(a, 2, "%s", a->game.reason);
            }
            act = true;
        }
    }
    /* engine's turn? */
    if (a->opp.state == OPP_IDLE && a->game.result == RES_ONGOING &&
        a->game.pos[a->game.n].stm == a->opp.side && a->modal != MODAL_PROMO) {
        char cmd[10000];
        game_uci_position(&a->game, a->game.n, cmd, sizeof cmd);
        engine_send(&a->opp.eng, "%s", cmd);
        int d = a->opp.difficulty;
        int mt = 60 + 9 * d;
        if (d <= 15) engine_send(&a->opp.eng, "go depth %d movetime %d", 1 + (d - 1) / 5, mt);
        else engine_send(&a->opp.eng, "go movetime %d", mt);
        a->opp.state = OPP_THINKING;
        act = true;
    }
    return act;
}

/* ------------------------------ navigation ------------------------------ */
static void set_view(App *a, int v) {
    if (v < 0) v = 0;
    if (v > a->game.n) v = a->game.n;
    if (v == a->game.view) return;
    a->game.view = v;
    ana_position_changed(a);
    a->msg[0] = 0;
    a->dirty = true;
}

/* ------------------------------ game ------------------------------ */
static void today_str(char *out, size_t n) {
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    strftime(out, n, "%Y.%m.%d", lt);
}

static void start_game(App *a) {
    opp_shutdown(a);
    a->player_side = a->menu_color == 0 ? 1 : a->menu_color == 1 ? -1 : (rand() & 1) ? 1 : -1;
    a->opp.side = -a->player_side;
    a->opp.difficulty = a->menu_diff;
    game_reset(&a->game, NULL);
    today_str(a->game.tag_date, sizeof a->game.tag_date);
    char sfname[64];
    snprintf(sfname, sizeof sfname, "Stockfish (level %d)", a->opp.difficulty);
    snprintf(a->player_side > 0 ? a->game.tag_white : a->game.tag_black,
             sizeof a->game.tag_white, "%s", "Player");
    snprintf(a->player_side > 0 ? a->game.tag_black : a->game.tag_white,
             sizeof a->game.tag_black, "%s", sfname);
    if (!opp_start(a)) return;
    a->mode = MODE_PLAY;
    a->screen = SCR_GAME;
    a->modal = MODAL_NONE;
    a->flip = a->player_side < 0;
    a->input_len = 0;
    a->input[0] = 0;
    a->force_clear = true;
    ana_clear_lines(a);
    if (a->ana.want) ana_position_changed(a);
    set_msg(a, 0, "Game on: type your moves (e4, Nf3, O-O or e2e4) and press Enter");
    a->dirty = true;
}

static void leave_game(App *a) {
    opp_shutdown(a);
    if (a->ana.eng.ok && a->ana.state == ANA_RUNNING) {
        engine_send(&a->ana.eng, "stop");
        a->ana.state = ANA_STOPPING;
        a->ana.pending = false;
    }
    a->ana.want = false;
    if (a->plist) { pgn_list_free(a->plist); a->plist = NULL; }
    a->screen = SCR_MENU;
    a->modal = MODAL_NONE;
    a->msg[0] = 0;
    a->force_clear = true;
    a->dirty = true;
}

/* ------------------------------ opening a PGN ------------------------------ */
static void enter_analyze(App *a, int idx) {
    char err[128];
    if (!pgn_load_game(a->plist, idx, &a->game, err, sizeof err)) {
        set_msg(a, 1, "%s", err);
        return;
    }
    a->mode = MODE_ANALYZE;
    a->screen = SCR_GAME;
    a->modal = MODAL_NONE;
    a->flip = false;
    a->game.view = 0;
    a->input_len = 0;
    a->force_clear = true;
    a->ana.want = false;
    if (ana_ensure_started(a)) {
        a->ana.want = true;
        ana_position_changed(a);
    }
    set_msg(a, 0, "%s — %s  ·  use the arrow keys to step through the moves",
            a->game.tag_white, a->game.tag_black);
    a->dirty = true;
}

static void open_pgn(App *a, const char *path_in) {
    /* clean up the path: quotes, spaces, leading ~ */
    char path[600];
    size_t n = 0;
    const char *s = path_in;
    while (*s == ' ' || *s == '"' || *s == '\'') s++;
    while (*s && n < sizeof path - 1) path[n++] = *s++;
    while (n > 0 && (path[n-1] == ' ' || path[n-1] == '"' || path[n-1] == '\'')) n--;
    path[n] = 0;
    /* drop shell escapes (drag & drop: "\ " -> " ") */
    char clean[600];
    size_t cn = 0;
    for (size_t i = 0; i < n; i++) {
        if (path[i] == '\\' && i + 1 < n && path[i+1] == ' ') continue;
        clean[cn++] = path[i];
    }
    clean[cn] = 0;
#ifndef _WIN32
    if (clean[0] == '~') {
        const char *home = getenv("HOME");
        if (home) {
            char tmp[600];
            snprintf(tmp, sizeof tmp, "%s%s", home, clean + 1);
            strcpy(clean, tmp);
        }
    }
#endif
    if (!clean[0]) { set_msg(a, 1, "Empty path"); return; }
    char err[128];
    if (a->plist) { pgn_list_free(a->plist); a->plist = NULL; }
    a->plist = pgn_scan_file(clean, err, sizeof err);
    if (!a->plist) { set_msg(a, 1, "%s: %s", err, clean); return; }
    snprintf(a->loaded_path, sizeof a->loaded_path, "%s", clean);
    if (a->plist->n == 1) enter_analyze(a, 0);
    else {
        a->screen = SCR_PICKER;
        a->pick_idx = 0;
        a->pick_scroll = 0;
        a->force_clear = true;
        a->msg[0] = 0;
    }
    a->dirty = true;
}

/* ------------------------------ user moves ------------------------------ */
static void apply_player_move(App *a, Move m) {
    game_push(&a->game, m);
    a->game.view = a->game.n;
    a->input_len = 0;
    a->input[0] = 0;
    a->msg[0] = 0;
    ana_position_changed(a);
    if (a->game.result != RES_ONGOING) set_msg(a, 2, "%s", a->game.reason);
    a->dirty = true;
}

static void try_move_input(App *a) {
    a->input[a->input_len] = 0;
    if (a->input_len == 0) return;
    if (a->game.result != RES_ONGOING) {
        set_msg(a, 1, "The game is over: n for a new game, s to save");
        a->input_len = 0;
        return;
    }
    if (a->game.pos[a->game.n].stm != a->player_side) {
        set_msg(a, 1, "It's Stockfish's turn, hang on…");
        return;
    }
    const Pos *live = &a->game.pos[a->game.n];
    Move m;
    if (san_to_move(live, a->input, &m)) { apply_player_move(a, m); return; }
    /* maybe it's a promotion with no piece given: try =Q..=N */
    static const char pieces[4] = { 'Q', 'R', 'B', 'N' };
    for (int i = 0; i < 4; i++) {
        char tryb[32];
        snprintf(tryb, sizeof tryb, "%s=%c", a->input, pieces[i]);
        if (san_to_move(live, tryb, &m)) {
            snprintf(a->promo_base, sizeof a->promo_base, "%s", a->input);
            a->modal = MODAL_PROMO;
            a->dirty = true;
            return;
        }
        snprintf(tryb, sizeof tryb, "%s%c", a->input, tolower((unsigned char)pieces[i]));
        if (uci_to_move(live, tryb, &m)) {
            snprintf(a->promo_base, sizeof a->promo_base, "%s", a->input);
            a->modal = MODAL_PROMO;
            a->dirty = true;
            return;
        }
    }
    set_msg(a, 1, "Illegal or ambiguous move: %s", a->input);
}

static void finish_promo(App *a, char piece) {
    Move m;
    char tryb[32];
    const Pos *live = &a->game.pos[a->game.n];
    snprintf(tryb, sizeof tryb, "%s=%c", a->promo_base, piece);
    if (!san_to_move(live, tryb, &m)) {
        snprintf(tryb, sizeof tryb, "%s%c", a->promo_base, tolower((unsigned char)piece));
        if (!uci_to_move(live, tryb, &m)) {
            a->modal = MODAL_NONE;
            set_msg(a, 1, "Invalid promotion");
            return;
        }
    }
    a->modal = MODAL_NONE;
    apply_player_move(a, m);
}

/* ------------------------------ saving ------------------------------ */
static void default_save_name(App *a) {
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    char base[80];
    strftime(base, sizeof base, "game_%Y%m%d_%H%M", lt);
    char name[160];
    snprintf(name, sizeof name, "%s.pgn", base);
    for (int i = 2; file_exists(name) && i < 100; i++)
        snprintf(name, sizeof name, "%s_%d.pgn", base, i);
    snprintf(a->save_input, sizeof a->save_input, "%s", name);
    a->save_len = (int)strlen(a->save_input);
}

static void do_save(App *a) {
    a->save_input[a->save_len] = 0;
    if (a->save_len == 0) { set_msg(a, 1, "Empty file name"); return; }
    char err[128];
    if (pgn_save(&a->game, a->save_input, err, sizeof err))
        set_msg(a, 2, "Game saved to %s", a->save_input);
    else
        set_msg(a, 1, "%s (%s)", err, a->save_input);
    a->modal = MODAL_NONE;
    a->dirty = true;
}

/* ------------------------------ takeback ------------------------------ */
static void takeback(App *a) {
    if (a->mode != MODE_PLAY) return;
    if (a->opp.state == OPP_THINKING) { set_msg(a, 1, "Wait for Stockfish's move"); return; }
    if (a->game.n == 0) { set_msg(a, 1, "No moves to take back"); return; }
    int back = (a->game.pos[a->game.n].stm == a->player_side) ? 2 : 1;
    if (back > a->game.n) back = a->game.n;
    a->game.n -= back;
    a->game.view = a->game.n;
    game_update_result(&a->game);
    ana_position_changed(a);
    set_msg(a, 0, "Move taken back");
    a->dirty = true;
}

/* ------------------------------ input: game screen ------------------------------ */
/* A character that can START a move (pawn a-h, uppercase piece, castling) */
static bool is_move_start(char c) {
    return (c >= 'a' && c <= 'h') || strchr("KQRBNO0o", c) != NULL;
}
/* A character that can CONTINUE a move already begun (incl. UCI promotion e8q) */
static bool is_move_char(char c) {
    return (c >= 'a' && c <= 'h') || (c >= '0' && c <= '9') ||
           strchr("KQRBNOoqrbnx=+#-", c) != NULL;
}

static void key_game(App *a, Key k) {
    Game *g = &a->game;

    if (a->modal == MODAL_HELP) {
        if (k.type != K_NONE) { a->modal = MODAL_NONE; a->force_clear = true; a->dirty = true; }
        return;
    }
    if (a->modal == MODAL_PROMO) {
        if (k.type == K_ESC) { a->modal = MODAL_NONE; a->input_len = 0; a->input[0] = 0; }
        else if (k.type == K_CHAR) {
            char c = (char)toupper((unsigned char)k.ch);
            if (c == 'Q') finish_promo(a, 'Q');
            else if (c == 'R') finish_promo(a, 'R');
            else if (c == 'B') finish_promo(a, 'B');
            else if (c == 'N') finish_promo(a, 'N');
        }
        a->dirty = true;
        return;
    }
    if (a->modal == MODAL_SAVE) {
        if (k.type == K_ESC) a->modal = MODAL_NONE;
        else if (k.type == K_ENTER) do_save(a);
        else if (k.type == K_BACKSPACE) { if (a->save_len > 0) a->save_input[--a->save_len] = 0; }
        else if (k.type == K_CHAR && a->save_len < (int)sizeof a->save_input - 2)
            a->save_input[a->save_len++] = k.ch;
        a->dirty = true;
        return;
    }
    if (a->modal == MODAL_CONFIRM_QUIT) {
        if (k.type == K_CHAR && (k.ch == 'y' || k.ch == 'Y')) leave_game(a);
        else if (k.type == K_CHAR || k.type == K_ESC || k.type == K_ENTER) a->modal = MODAL_NONE;
        a->dirty = true;
        return;
    }
    if (a->modal == MODAL_CONFIRM_NEW) {
        if (k.type == K_CHAR && (k.ch == 'y' || k.ch == 'Y')) { a->modal = MODAL_NONE; start_game(a); }
        else if (k.type == K_CHAR || k.type == K_ESC || k.type == K_ENTER) a->modal = MODAL_NONE;
        a->dirty = true;
        return;
    }

    switch (k.type) {
        case K_LEFT: set_view(a, g->view - 1); return;
        case K_RIGHT: set_view(a, g->view + 1); return;
        case K_HOME: set_view(a, 0); return;
        case K_END: set_view(a, g->n); return;
        case K_PGUP: set_view(a, g->view - 6); return;
        case K_PGDN: set_view(a, g->view + 6); return;
        case K_BACKSPACE:
            if (a->input_len > 0) { a->input[--a->input_len] = 0; a->dirty = true; }
            return;
        case K_ENTER:
            if (a->mode == MODE_PLAY) try_move_input(a);
            a->dirty = true;
            return;
        case K_ESC:
            if (a->input_len > 0) { a->input_len = 0; a->input[0] = 0; a->dirty = true; return; }
            if (a->mode == MODE_PLAY && g->result == RES_ONGOING && g->n > 0)
                a->modal = MODAL_CONFIRM_QUIT;
            else leave_game(a);
            a->dirty = true;
            return;
        default: break;
    }
    if (k.type != K_CHAR) return;
    char c = k.ch;

    bool typing = a->input_len > 0;
    if (typing) {
        /* a move is being composed: every plausible character goes to the buffer */
        if (a->mode == MODE_PLAY && is_move_char(c) && a->input_len < (int)sizeof a->input - 2) {
            a->input[a->input_len++] = c;
            a->input[a->input_len] = 0;
            a->dirty = true;
        }
        return;
    }
    /* empty buffer: move-starting keys first, then commands */
    if (a->mode == MODE_PLAY && g->result == RES_ONGOING && is_move_start(c)) {
        a->input[a->input_len++] = c;
        a->input[a->input_len] = 0;
        a->dirty = true;
        return;
    }
    switch (c) {
        case 'q':
            if (a->mode == MODE_PLAY && g->result == RES_ONGOING && g->n > 0)
                a->modal = MODAL_CONFIRM_QUIT;
            else leave_game(a);
            a->dirty = true;
            return;
        case 'v': ana_toggle(a); return;
        case 'r': a->flip = !a->flip; a->dirty = true; return;
        case 'u':
            a->piece_style = (a->piece_style + 1) % 3;
            a->dirty = true;
            return;
        case 's':
            default_save_name(a);
            a->modal = MODAL_SAVE;
            a->dirty = true;
            return;
        case 'z': takeback(a); return;
        case '+': case '=': {
            static const char *SZ[4] = { "compact", "medium", "large", "huge" };
            int eff = board_eff_size(a);
            if (eff < board_max_size(a)) {
                a->board_size = eff + 1;
                a->force_clear = true;
                set_msg(a, 0, "Board size: %s", SZ[a->board_size]);
            } else if (eff < 3)
                set_msg(a, 1, "The terminal is too small for a bigger board");
            else
                set_msg(a, 0, "Board already at the largest size");
            a->dirty = true;
            return;
        }
        case '-': {
            static const char *SZ[4] = { "compact", "medium", "large", "huge" };
            int eff = board_eff_size(a);
            if (eff > 0) {
                a->board_size = eff - 1;
                a->force_clear = true;
                set_msg(a, 0, "Board size: %s", SZ[a->board_size]);
            } else set_msg(a, 0, "Board already at the smallest size");
            a->dirty = true;
            return;
        }
        case 'n':
            if (a->mode == MODE_PLAY) { a->modal = MODAL_CONFIRM_NEW; a->dirty = true; }
            return;
        case '?': a->modal = MODAL_HELP; a->dirty = true; return;
        case ' ': set_view(a, g->view + 1); return;
        default: break;
    }
}

/* ------------------------------ input: menu ------------------------------ */
static void key_menu(App *a, Key k) {
    if (a->path_editing) {
        if (k.type == K_ESC) a->path_editing = false;
        else if (k.type == K_ENTER) {
            a->path_input[a->path_len] = 0;
            a->path_editing = false;
            open_pgn(a, a->path_input);
        } else if (k.type == K_BACKSPACE) {
            if (a->path_len > 0) a->path_input[--a->path_len] = 0;
        } else if (k.type == K_CHAR && a->path_len < (int)sizeof a->path_input - 2)
            a->path_input[a->path_len++] = k.ch;
        a->dirty = true;
        return;
    }
    switch (k.type) {
        case K_UP: a->menu_item = (a->menu_item + 4) % 5; break;
        case K_DOWN: a->menu_item = (a->menu_item + 1) % 5; break;
        case K_LEFT:
            if (a->menu_item == 1 && a->menu_diff > 1) a->menu_diff--;
            if (a->menu_item == 2) a->menu_color = (a->menu_color + 2) % 3;
            break;
        case K_RIGHT:
            if (a->menu_item == 1 && a->menu_diff < 100) a->menu_diff++;
            if (a->menu_item == 2) a->menu_color = (a->menu_color + 1) % 3;
            break;
        case K_PGUP:
            if (a->menu_item == 1) { a->menu_diff += 10; if (a->menu_diff > 100) a->menu_diff = 100; }
            break;
        case K_PGDN:
            if (a->menu_item == 1) { a->menu_diff -= 10; if (a->menu_diff < 1) a->menu_diff = 1; }
            break;
        case K_ENTER:
            if (a->menu_item == 0 || a->menu_item == 1 || a->menu_item == 2) start_game(a);
            else if (a->menu_item == 3) { a->path_editing = true; a->msg[0] = 0; }
            else a->quit = true;
            break;
        case K_CHAR:
            if (k.ch == 'q') a->quit = true;
            break;
        default: break;
    }
    a->dirty = true;
}

/* ------------------------------ input: game picker ------------------------------ */
static void key_picker(App *a, Key k) {
    int n = a->plist ? a->plist->n : 0;
    switch (k.type) {
        case K_UP: if (a->pick_idx > 0) a->pick_idx--; break;
        case K_DOWN: if (a->pick_idx < n - 1) a->pick_idx++; break;
        case K_PGUP: a->pick_idx -= 10; if (a->pick_idx < 0) a->pick_idx = 0; break;
        case K_PGDN: a->pick_idx += 10; if (a->pick_idx > n - 1) a->pick_idx = n - 1; break;
        case K_HOME: a->pick_idx = 0; break;
        case K_END: a->pick_idx = n - 1; break;
        case K_ENTER: enter_analyze(a, a->pick_idx); break;
        case K_ESC:
            a->screen = SCR_MENU;
            a->force_clear = true;
            break;
        case K_CHAR:
            if (k.ch == 'q') { a->screen = SCR_MENU; a->force_clear = true; }
            break;
        default: break;
    }
    a->dirty = true;
}

/* ------------------------------ text-mode utilities (tests) ------------------------------ */
static int run_perft(void) {
    static const struct { const char *fen; int depth; unsigned long long want; } T[] = {
        { "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 5, 4865609ULL },
        { "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 4, 4085603ULL },
        { "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 5, 674624ULL },
        { "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 4, 422333ULL },
        { "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 4, 2103487ULL },
    };
    int fail = 0;
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        Pos p;
        if (!pos_from_fen(&p, T[i].fen)) { printf("INVALID FEN: %s\n", T[i].fen); fail++; continue; }
        unsigned long long got = perft(&p, T[i].depth);
        printf("perft(%d) = %-10llu %s  [%s]\n", T[i].depth, got,
               got == T[i].want ? "OK" : "FAIL", T[i].fen);
        if (got != T[i].want) fail++;
    }
    if (fail) printf("\n%d TEST(S) FAILED\n", fail);
    else printf("\nAll perft tests passed.\n");
    return fail ? 1 : 0;
}

static int run_engine_test(const char *path) {
    App *a = &g_app;
    find_engine(a, path);
    printf("Engine: %s\n", a->engine_path);
    Engine e;
    if (!engine_start(&e, a->engine_path)) {
        printf("ERROR: could not start the engine.\nRun `make engine` to build it.\n");
        return 1;
    }
    printf("UCI handshake ok: %s\n", e.name);
    engine_send(&e, "setoption name MultiPV value 3");
    engine_send(&e, "position startpos");
    engine_send(&e, "go depth 12");
    long long t0 = now_ms();
    char line[8192];
    Pos start;
    pos_start(&start);
    EngineLine lines[3];
    memset(lines, 0, sizeof lines);
    bool done = false;
    while (!done && now_ms() - t0 < 20000) {
        if (engine_poll_line(&e, line, sizeof line)) {
            engine_parse_info(line, &start, lines);
            if (!strncmp(line, "bestmove ", 9)) {
                printf("bestmove: %s\n", line + 9);
                done = true;
            }
        } else msleep(5);
    }
    for (int i = 0; i < 3; i++)
        if (lines[i].valid)
            printf("line %d: depth %d, score %+0.2f, %s\n", i + 1, lines[i].depth,
                   lines[i].mate ? 999.0 * (lines[i].score > 0 ? 1 : -1) : lines[i].score / 100.0,
                   lines[i].pv_san);
    engine_quit(&e);
    printf("%s", done ? "Engine test passed.\n" : "ERROR: no bestmove received.\n");
    return done ? 0 : 1;
}

static int run_pgn_test(const char *path) {
    char err[128];
    PgnList *L = pgn_scan_file(path, err, sizeof err);
    if (!L) { printf("ERROR: %s\n", err); return 1; }
    printf("Games in the file: %d\n", L->n);
    int fail = 0;
    for (int i = 0; i < L->n; i++) {
        Game g;
        if (!pgn_load_game(L, i, &g, err, sizeof err)) {
            printf("  %d) ERROR: %s\n", i + 1, err);
            fail++;
            continue;
        }
        printf("  %d) %s — %s: %d plies, result %s (%s)\n", i + 1,
               g.tag_white, g.tag_black, g.n, result_str(g.result), g.reason);
        /* round-trip: save and reload */
        char tmp[256];
        snprintf(tmp, sizeof tmp, "%s.roundtrip.pgn", path);
        if (!pgn_save(&g, tmp, err, sizeof err)) { printf("     save: %s\n", err); fail++; continue; }
        PgnList *L2 = pgn_scan_file(tmp, err, sizeof err);
        Game g2;
        if (!L2 || !pgn_load_game(L2, 0, &g2, err, sizeof err) || g2.n != g.n) {
            printf("     round-trip FAILED (%s)\n", err);
            fail++;
        } else printf("     round-trip ok (%d plies)\n", g2.n);
        if (L2) pgn_list_free(L2);
        remove(tmp);
    }
    pgn_list_free(L);
    if (fail) printf("%d ERROR(S)\n", fail);
    else printf("PGN test passed.\n");
    return fail ? 1 : 0;
}

/* ------------------------------ main ------------------------------ */
static void on_sigint(int sig) {
    (void)sig;
    g_sigint = 1;
}

static void cleanup(void) {
    term_restore();
}

int main(int argc, char **argv) {
    App *a = &g_app;
    memset(a, 0, sizeof *a);
    a->menu_diff = 30;
    a->menu_color = 0;
    a->piece_style = 0;
    a->board_size = 3; /* start at the largest board that fits the terminal */
    srand((unsigned)time(NULL));

    const char *pgn_arg = NULL, *engine_arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--perft")) return run_perft();
        if (!strcmp(argv[i], "--engine-test"))
            return run_engine_test(i + 1 < argc ? argv[i + 1] : NULL);
        if (!strcmp(argv[i], "--pgn-test") && i + 1 < argc)
            return run_pgn_test(argv[i + 1]);
        if (!strcmp(argv[i], "--engine") && i + 1 < argc) { engine_arg = argv[++i]; continue; }
        if (!strcmp(argv[i], "--version")) {
            printf("ChessTUI %s\n", CHESSTUI_VERSION);
            printf(
                   "License GPLv3+: GNU GPL version 3 or later <https://gnu.org/licenses/gpl.html>\n"
                   "Uses Stockfish <https://stockfishchess.org> as a separate UCI engine process.\n");
            return 0;
        }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("Usage: chesstui [game.pgn] [--engine PATH]\n"
                   "  With no arguments opens the menu. With a PGN file opens analysis.\n"
                   "  --engine PATH       use a specific Stockfish binary\n"
                   "  --perft             move generator self-test\n"
                   "  --engine-test       engine communication self-test\n"
                   "  --pgn-test FILE     PGN parser self-test\n");
            return 0;
        }
        if (argv[i][0] != '-') pgn_arg = argv[i];
    }

    find_engine(a, engine_arg);
    probe_engine(a);

    if (!term_init()) {
        fprintf(stderr, "Error: an interactive terminal (TTY) is required.\n");
        return 1;
    }
    atexit(cleanup);
    signal(SIGINT, on_sigint);
#ifndef _WIN32
    signal(SIGTERM, on_sigint);
#endif

    term_size(&a->tw, &a->th);
    a->force_clear = true;
    a->dirty = true;
    a->screen = SCR_MENU;

    if (pgn_arg) open_pgn(a, pgn_arg);

    while (!a->quit && !g_sigint) {
        bool act = false;
        Key k;
        while ((k = term_read_key()).type != K_NONE) {
            switch (a->screen) {
                case SCR_MENU: key_menu(a, k); break;
                case SCR_PICKER: key_picker(a, k); break;
                case SCR_GAME: key_game(a, k); break;
            }
            act = true;
            if (a->quit) break;
        }
        if (a->quit || g_sigint) break;

        act |= pump_opponent(a);
        act |= pump_analyzer(a);

        if (a->opp.state == OPP_THINKING && now_ms() - a->spin_last > 120) {
            a->spin_last = now_ms();
            a->spin = (a->spin + 1) % 10;
            act = true;
        }
        int w, h;
        term_size(&w, &h);
        if (w != a->tw || h != a->th) {
            a->tw = w; a->th = h;
            a->force_clear = true;
            act = true;
        }
        if (act || a->dirty) {
            draw_frame(a);
            a->dirty = false;
        }
        msleep(12);
    }

    if (a->opp.eng.ok) engine_quit(&a->opp.eng);
    if (a->ana.eng.ok) engine_quit(&a->ana.eng);
    if (a->plist) pgn_list_free(a->plist);
    sb_free(&a->fb);
    term_restore();
    return 0;
}
