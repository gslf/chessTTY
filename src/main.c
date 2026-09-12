/* main.c — ChessTTY: chess in the terminal with Stockfish built in */
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

#ifndef CHESSTTY_VERSION            /* set by the Makefile from the git tag */
#define CHESSTTY_VERSION "dev"
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
    const char *envp = getenv("CHESSTTY_ENGINE");
    /* Searched relative to the executable: next to it (release archive), one
       level up (build tree), then the `make install` layout
       (<prefix>/bin/chesstty with <prefix>/lib/chesstty/stockfish). */
#ifdef _WIN32
    static const char *const rel[] = { "engine\\stockfish.exe",
                                       "..\\engine\\stockfish.exe", NULL };
    const char *bare = "stockfish.exe";
#else
    static const char *const rel[] = { "engine/stockfish", "../engine/stockfish",
                                       "../lib/chesstty/stockfish", NULL };
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

/* Live clocks are anchored to monotonic time, never decremented per frame. */
static void record_clock(App *a) {
    if (!a->clock.enabled) return;
    int n = a->game.n;
    a->game.clocks[n][0] = clock_remaining(&a->clock, 1, now_ms());
    a->game.clocks[n][1] = clock_remaining(&a->clock, -1, now_ms());
}
static bool live_mode(const App *a) { return a->mode == MODE_PLAY || a->mode == MODE_LOCAL || a->mode == MODE_ONLINE; }
static void init_clock(App *a) {
    a->game.initial_ms = (int64_t)a->menu_minutes * 60000;
    a->game.increment_ms = (int64_t)a->menu_increment * 1000;
    clock_start(&a->clock, a->game.initial_ms, a->game.increment_ms, a->game.pos[a->game.n].stm, now_ms());
    record_clock(a);
}
static bool flag_clock(App *a) {
    if ((a->mode != MODE_LOCAL && a->mode != MODE_PLAY) || !a->clock.running || a->game.result != RES_ONGOING) return false;
    int side = a->clock.side;
    if (clock_remaining(&a->clock, side, now_ms()) > 0) return false;
    clock_stop(&a->clock, now_ms());
    record_clock(a);
    /* A lone king, or a lone minor against a bare king, cannot win on time. */
    int winner_pieces = 0, minor = 0, loser_pieces = 0;
    for (int i = 0; i < 64; i++) {
        int pc = a->game.pos[a->game.n].sq[i];
        if (pc * -side > 0 && pc * -side != KING) { winner_pieces++; minor += pc * -side == BISHOP || pc * -side == KNIGHT; }
        if (pc * side > 0 && pc * side != KING) loser_pieces++;
    }
    bool draw = winner_pieces == 0 || (winner_pieces == 1 && minor == 1 && loser_pieces == 0);
    a->game.result = draw ? RES_DRAW : side > 0 ? RES_BLACK : RES_WHITE;
    snprintf(a->game.reason, sizeof a->game.reason, "%s", draw ? "Time expired; insufficient mating material" : "Time expired");
    snprintf(a->game.tag_result, sizeof a->game.tag_result, "%s", result_str(a->game.result));
    if (a->opp.state == OPP_THINKING) engine_send(&a->opp.eng, "stop");
    a->opp.state = OPP_IDLE;
    set_msg(a, 2, "%s", a->game.reason);
    return true;
}

/* ------------------------------ analyser ------------------------------ */
static bool ana_ensure_started(App *a) {
    if (a->mode == MODE_ONLINE || a->mode == MODE_LOCAL || (a->online.game_ready && !a->online.ended)) return false;
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
    a->ana.started_ms = a->ana.elapsed_ms = 0;
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
    a->ana.started_ms = now_ms();
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
    if (a->mode == MODE_ONLINE || a->mode == MODE_LOCAL) return;
    a->ana.want = !a->ana.want;
    a->force_clear = true;
    if (a->ana.want) {
        if (!ana_ensure_started(a)) { a->ana.want = false; return; }
        ana_position_changed(a);
    } else if (a->ana.eng.ok && a->ana.state == ANA_RUNNING) {
        a->ana.elapsed_ms = now_ms() - a->ana.started_ms;
        engine_send(&a->ana.eng, "stop");
        a->ana.state = ANA_STOPPING;
        a->ana.pending = false;
    }
    a->dirty = true;
}

static bool pump_analyzer(App *a) {
    if (!a->ana.eng.ok) return false;
    if (a->ana.state == ANA_RUNNING && a->ana.started_ms) a->ana.elapsed_ms = now_ms() - a->ana.started_ms;
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
        if (!strncmp(line, "bestmove ", 9) && a->opp.state == OPP_THINKING && a->game.result == RES_ONGOING) {
            a->opp.state = OPP_IDLE;
            char mv[8] = {0};
            sscanf(line + 9, "%7s", mv);
            Move m;
            int before = a->game.n;
            if (strcmp(mv, "(none)") != 0 && uci_to_move(&a->game.pos[a->game.n], mv, &m)) {
                bool follow = a->game.view == before;
                if (flag_clock(a) || !clock_move(&a->clock, a->opp.side, now_ms())) continue;
                game_push(&a->game, m);
                if (a->game.result != RES_ONGOING) clock_stop(&a->clock, now_ms());
                record_clock(a);
                if (follow) {
                    a->game.view = a->game.n;
                    ana_position_changed(a);
                } else {
                    set_msg(a, 0, "Stockfish played %s",
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
        int64_t remaining = clock_remaining(&a->clock, a->opp.side, now_ms());
        if (remaining >= 0 && remaining / 4 < mt) mt = remaining / 4 > 1 ? (int)(remaining / 4) : 1;
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
    init_clock(a);
    set_msg(a, 0, "Game started");
    a->dirty = true;
}

static void start_local(App *a) {
    opp_shutdown(a);
    if (a->ana.eng.ok) engine_quit(&a->ana.eng);
    a->ana.want = false; a->ana.state = ANA_IDLE;
    game_reset(&a->game, NULL);
    today_str(a->game.tag_date, sizeof a->game.tag_date);
    snprintf(a->game.tag_event, sizeof a->game.tag_event, "ChessTTY local game");
    a->mode = MODE_LOCAL; a->screen = SCR_GAME; a->modal = MODAL_NONE;
    a->from_db = false; a->flip = false; a->input_len = 0; a->input[0] = 0;
    init_clock(a); a->force_clear = a->dirty = true;
    set_msg(a, 0, "Two players · %d minutes + %d seconds per move", a->menu_minutes, a->menu_increment);
}

static void open_online(App *a) {
    opp_shutdown(a);
    if (a->ana.eng.ok) engine_quit(&a->ana.eng);
    a->ana.want = false; a->ana.state = ANA_IDLE;
    clock_stop(&a->clock, now_ms());
    a->screen = SCR_ONLINE; a->online_item = 0; a->modal = MODAL_NONE;
    a->msg[0] = 0; a->force_clear = a->dirty = true;
    online_init(&a->online);
}

static void leave_game(App *a) {
    clock_stop(&a->clock, now_ms());
    if (a->mode == MODE_ONLINE) { open_online(a); return; }
    opp_shutdown(a);
    if (a->ana.eng.ok && a->ana.state == ANA_RUNNING) {
        engine_send(&a->ana.eng, "stop");
        a->ana.state = ANA_STOPPING;
        a->ana.pending = false;
    }
    a->ana.want = false;
    /* Coming from a collection, going "back" means the listing, not the menu:
       the index is already in memory, so it reopens instantly. */
    if (a->from_db && a->db.open) a->screen = SCR_DB;
    else {
        if (a->plist) { pgn_list_free(a->plist); a->plist = NULL; }
        a->screen = SCR_MENU;
    }
    a->from_db = false;
    a->modal = MODAL_NONE;
    a->msg[0] = 0;
    a->force_clear = true;
    a->dirty = true;
}

/* ------------------------------ analysis board ------------------------------ */
/* Shared tail of every way into the analysis board: a PGN, a FEN, a database
   record or an empty board all land on the same editable position. */
static void enter_analysis(App *a) {
    clock_stop(&a->clock, now_ms());
    a->mode = MODE_ANALYZE;
    a->screen = SCR_GAME;
    a->modal = MODAL_NONE;
    a->flip = false;
    a->input_len = 0;
    a->input[0] = 0;
    a->force_clear = true;
    a->ana.want = false;
    if (ana_ensure_started(a)) {
        a->ana.want = true;
        ana_position_changed(a);
    }
    a->dirty = true;
}

static void enter_analyze(App *a, int idx) {
    char err[128];
    if (!pgn_load_game(a->plist, idx, &a->game, err, sizeof err)) {
        set_msg(a, 1, "%s", err);
        return;
    }
    a->from_db = false;
    a->game.view = 0;
    enter_analysis(a);
    set_msg(a, 0, "%s — %s ",
            a->game.tag_white, a->game.tag_black);
}

static void start_analysis_board(App *a, const char *fen) {
    opp_shutdown(a);
    game_reset(&a->game, fen);
    today_str(a->game.tag_date, sizeof a->game.tag_date);
    snprintf(a->game.tag_event, sizeof a->game.tag_event, "%s", "ChessTTY analysis");
    a->from_db = false;
    a->player_side = 0;              /* both sides belong to the user */
    enter_analysis(a);
    set_msg(a, 0, "Analysis board: play both sides");
}

/* ------------------------------ paths ------------------------------ */
/* Strips quotes, padding and shell escapes, and expands a leading ~, so a path
   pasted or drag-and-dropped into the prompt works as typed. */
static void clean_path(const char *in, char *out, size_t outn) {
    char tmp[PATH_MAX_CT];
    size_t n = 0;
    const char *s = in;
    while (*s == ' ' || *s == '"' || *s == '\'') s++;
    while (*s && n < sizeof tmp - 1) tmp[n++] = *s++;
    while (n > 0 && (tmp[n-1] == ' ' || tmp[n-1] == '"' || tmp[n-1] == '\'')) n--;
    tmp[n] = 0;
    char clean[sizeof tmp];
    size_t cn = 0;
    for (size_t i = 0; i < n; i++) {
        if (tmp[i] == '\\' && i + 1 < n && tmp[i+1] == ' ') continue;
        clean[cn++] = tmp[i];
    }
    clean[cn] = 0;
#ifndef _WIN32
    if (clean[0] == '~' && (clean[1] == '/' || clean[1] == 0)) {
        const char *home = getenv("HOME");
        if (home) {
            char exp[sizeof tmp];
            int w = snprintf(exp, sizeof exp, "%s%s", home, clean + 1);
            if (w > 0 && (size_t)w < sizeof exp) memcpy(clean, exp, (size_t)w + 1);
        }
    }
#endif
    snprintf(out, outn, "%s", clean);
}

static void open_pgn(App *a, const char *path_in) {
    char clean[sizeof a->loaded_path];
    clean_path(path_in, clean, sizeof clean);
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

/* ------------------------------ opening a FEN ------------------------------ */
static void open_fen(App *a, const char *fen_in) {
    char fen[160];
    size_t n = 0;
    const char *s = fen_in;
    while (*s == ' ' || *s == '"' || *s == '\'') s++;
    while (*s && n < sizeof fen - 1) fen[n++] = *s++;
    while (n > 0 && (fen[n-1] == ' ' || fen[n-1] == '"' || fen[n-1] == '\'')) n--;
    fen[n] = 0;
    if (!fen[0]) { set_msg(a, 1, "Empty FEN"); return; }
    Pos p;
    if (!pos_from_fen(&p, fen)) {
        set_msg(a, 1, "Not a valid FEN: %s", fen);
        return;
    }
    /* A position where the side that just moved is still in check can never be
       reached, and would make the engine and the move generator disagree. */
    if (in_check(&p, -p.stm)) {
        set_msg(a, 1, "Illegal position: the side not to move is in check");
        return;
    }
    start_analysis_board(a, fen);
    set_msg(a, 0, "Position loaded · %s to move · play both sides",
            p.stm > 0 ? "White" : "Black");
}

/* ------------------------------ games database ------------------------------ */
static bool db_ensure(App *a) {
    if (a->db.open) return true;
    char dir[PATH_MAX_CT], err[128];
    db_default_dir(dir, sizeof dir);
    if (!db_open(&a->db, dir, err, sizeof err)) {
        set_msg(a, 1, "%s (%s)", err, dir);
        return false;
    }
    return true;
}

static void open_db(App *a, const char *path_in) {
    char clean[PATH_MAX_CT], err[128];
    if (path_in && *path_in) clean_path(path_in, clean, sizeof clean);
    else db_default_dir(clean, sizeof clean);
    flash_msg(a, "Opening the collection…");
    if (!db_open(&a->db, clean, err, sizeof err)) {
        set_msg(a, 1, "%s (%s)", err, clean);
        return;
    }
    a->db_idx = a->db_scroll = 0;
    a->db_filtering = false;
    a->db_filter[0] = 0;
    a->db_filter_len = 0;
    db_set_filter(&a->db, "");
    a->screen = SCR_DB;
    a->force_clear = true;
    if (a->db.nrecs == 0)
        set_msg(a, 1, "No games yet in %s — no saved games", a->db.dir);
    else
        set_msg(a, 0, "%d games from %d file(s) · indexed in %lld ms%s", a->db.nrecs,
                a->db.nfiles, a->db.open_ms,
                a->db.scanned ? " (re-indexed)" : " (from the index)");
    a->dirty = true;
}

static void enter_db_game(App *a) {
    char err[128];
    if (!db_load(&a->db, a->db_idx, &a->game, err, sizeof err)) {
        set_msg(a, 1, "%s", err);
        a->dirty = true;
        return;
    }
    a->game.view = 0;
    enter_analysis(a);
    a->from_db = true;
    set_msg(a, 0, "%s — %s ",
            a->game.tag_white, a->game.tag_black);
}

static void save_to_db(App *a) {
    if (!db_ensure(a)) return;
    char err[128];
    if (!db_add(&a->db, &a->game, err, sizeof err)) {
        set_msg(a, 1, "%s", err);
        return;
    }
    int notes = game_note_count(&a->game);
    set_msg(a, 2, "Added to the collection (%d games)%s%s", a->db.nrecs,
            notes ? " with " : "", notes ? "annotations" : "");
    a->dirty = true;
}

/* ------------------------------ user moves ------------------------------ */
/* The position a typed move is played from: the live one while playing, the
   one on screen on the analysis board (where browsing back and playing a
   different move starts a new line). */
static const Pos *move_base(const App *a) {
    return a->mode == MODE_ANALYZE ? &a->game.pos[a->game.view]
                                   : &a->game.pos[a->game.n];
}

static void apply_player_move(App *a, Move m) {
    Game *g = &a->game;
    if (a->mode == MODE_ONLINE) { online_move(&a->online, m); a->input_len = 0; a->input[0] = 0; return; }
    if (flag_clock(a)) return;
    if (a->mode == MODE_ANALYZE && g->view < g->n) {
        Move nx = g->moves[g->view];
        if (nx.from == m.from && nx.to == m.to && nx.promo == m.promo) {
            /* Replaying the move already in the line: just walk forward. */
            g->view++;
            a->input_len = 0;
            a->input[0] = 0;
            a->msg[0] = 0;
            ana_position_changed(a);
            a->dirty = true;
            return;
        }
        int dropped = g->n - g->view;
        game_truncate(g, g->view);
        set_msg(a, 0, "New line from here · %d move%s replaced", dropped,
                dropped == 1 ? "" : "s");
    } else a->msg[0] = 0;
    if (g->n >= MAXPLY) { set_msg(a, 1, "Game is too long"); return; }
    if (live_mode(a) && !clock_move(&a->clock, g->pos[g->n].stm, now_ms())) { flag_clock(a); return; }
    if (!game_push(g, m)) { set_msg(a, 1, "The line is full (%d plies)", MAXPLY); return; }
    if (live_mode(a)) { if (g->result != RES_ONGOING) clock_stop(&a->clock, now_ms()); record_clock(a); }
    g->view = g->n;
    a->input_len = 0;
    a->input[0] = 0;
    ana_position_changed(a);
    if (g->result != RES_ONGOING && live_mode(a)) set_msg(a, 2, "%s", g->reason);
    a->dirty = true;
}

static void try_move_input(App *a) {
    a->input[a->input_len] = 0;
    if (a->input_len == 0) return;
    if (live_mode(a)) {
        if (a->game.result != RES_ONGOING) {
            set_msg(a, 1, "The game is over.");
            a->input_len = 0;
            return;
        }
        if (a->mode != MODE_LOCAL && a->game.pos[a->game.n].stm != a->player_side) {
            set_msg(a, 1, "Waiting for the opponent's move…");
            return;
        }
    }
    const Pos *live = move_base(a);
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
    const Pos *live = move_base(a);
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

/* ------------------------------ annotations ------------------------------ */
/* Human name of the position after `ply` plies, used in prompts and messages. */
static void ply_label(const Game *g, int ply, char *out, size_t n) {
    if (ply <= 0) { snprintf(out, n, "the starting position"); return; }
    const Pos *p = &g->pos[ply - 1];
    snprintf(out, n, "%d%s %s", p->fullmove, p->stm > 0 ? "." : "...", g->san[ply - 1]);
}

static void begin_note(App *a) {
    a->note_ply = a->game.view;
    snprintf(a->note_input, sizeof a->note_input, "%s", game_note(&a->game, a->note_ply));
    a->note_len = (int)strlen(a->note_input);
    a->modal = MODAL_NOTE;
    a->dirty = true;
}

static void do_note(App *a) {
    a->note_input[a->note_len] = 0;
    char lab[64];
    ply_label(&a->game, a->note_ply, lab, sizeof lab);
    if (!game_set_note(&a->game, a->note_ply, a->note_input))
        set_msg(a, 1, "No room left for annotations (%d bytes per game)", NOTE_POOL_BYTES);
    else if (a->note_len) set_msg(a, 2, "Annotation saved on %s", lab);
    else set_msg(a, 0, "Annotation removed from %s", lab);
    a->modal = MODAL_NONE;
    a->force_clear = true;
    a->dirty = true;
}

static void cycle_nag(App *a) {
    Game *g = &a->game;
    if (g->view == 0) { set_msg(a, 1, "No move here to mark — step forward first"); return; }
    int i = g->view - 1;
    g->nag[i] = (uint8_t)nag_cycle(g->nag[i]);
    char lab[64];
    ply_label(g, g->view, lab, sizeof lab);
    const char *glyph = nag_glyph(g->nag[i]);
    if (*glyph) set_msg(a, 0, "%s marked %s", lab, glyph);
    else set_msg(a, 0, "Mark removed from %s", lab);
    a->dirty = true;
}

/* ------------------------------ takeback ------------------------------ */
static void takeback(App *a) {
    Game *g = &a->game;
    if (a->mode == MODE_ANALYZE) {
        if (g->n == 0) { set_msg(a, 1, "No moves to take back"); return; }
        if (g->view < g->n) {
            set_msg(a, 1, "Press End first: taking back drops the rest of the line");
            return;
        }
        game_truncate(g, g->n - 1);
        g->view = g->n;
        ana_position_changed(a);
        set_msg(a, 0, "Move taken back");
        a->dirty = true;
        return;
    }
    if (a->mode != MODE_PLAY) return;
    if (a->opp.state == OPP_THINKING) { set_msg(a, 1, "Wait for Stockfish's move"); return; }
    if (g->n == 0) { set_msg(a, 1, "No moves to take back"); return; }
    int back = (g->pos[g->n].stm == a->player_side) ? 2 : 1;
    if (back > g->n) back = g->n;
    game_truncate(g, g->n - back);
    clock_sync(&a->clock, g->clocks[g->n][0], g->clocks[g->n][1], g->pos[g->n].stm, g->result == RES_ONGOING, now_ms());
    g->view = g->n;
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

static void start_openings(App *a) {
    leave_game(a);
    game_reset(&a->game, NULL);
    memset(a->opening_path, 0, sizeof a->opening_path);
    memset(a->opening_selection, 0, sizeof a->opening_selection);
    a->screen = SCR_GAME;
    a->mode = MODE_OPENINGS;
    a->flip = false;
    a->input_len = 0;
    a->input[0] = 0;
    set_msg(a, 0, "Lichess opening names · offline collection · no statistics or evaluations");
}

static bool key_openings(App *a, Key k) {
    Game *g = &a->game;
    int ply = g->view;
    uint16_t first = opening_child(a->opening_path[ply]);
    int count = 0;
    for (uint16_t node = first; node; node = opening_next(node)) count++;
    int *selection = &a->opening_selection[ply];
    if (k.type == K_UP) { if (*selection > 0) (*selection)--; }
    else if (k.type == K_DOWN) { if (*selection + 1 < count) (*selection)++; }
    else if (k.type == K_LEFT || k.type == K_BACKSPACE) {
        if (g->view > 0) g->view--;
    } else if (k.type == K_HOME) g->view = 0;
    else if (k.type == K_END) g->view = g->n;
    else if (k.type == K_ENTER || k.type == K_RIGHT ||
             (k.type == K_CHAR && k.ch == ' ')) {
        uint16_t node = first;
        for (int i = 0; node && i < *selection; i++) node = opening_next(node);
        if (node && ply < MAXPLY) {
            /* Reuse the existing continuation, or replace it when branching. */
            if (ply < g->n && a->opening_path[ply + 1] == node) g->view++;
            else {
                g->n = ply;
                game_push(g, opening_move(node));
                g->view = g->n;
                a->opening_path[g->view] = node;
                a->opening_selection[g->view] = 0;
            }
        }
    } else return false;
    a->dirty = true;
    return true;
}

static void key_game(App *a, Key k) {
    Game *g = &a->game;

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
        else if (k.type == K_CHAR && a->save_len < (int)sizeof a->save_input - 2) {
            a->save_input[a->save_len++] = k.ch;
            a->save_input[a->save_len] = 0;
        }
        a->dirty = true;
        return;
    }
    if (a->modal == MODAL_NOTE) {
        if (k.type == K_ESC) { a->modal = MODAL_NONE; a->force_clear = true; }
        else if (k.type == K_ENTER) do_note(a);
        else if (k.type == K_BACKSPACE) { if (a->note_len > 0) a->note_input[--a->note_len] = 0; }
        else if (k.type == K_CHAR && a->note_len < (int)sizeof a->note_input - 1) {
            a->note_input[a->note_len++] = k.ch;
            a->note_input[a->note_len] = 0;
        }
        a->dirty = true;
        return;
    }
    if (a->modal == MODAL_CONFIRM_QUIT) {
        if (k.type == K_CHAR && (k.ch == 'y' || k.ch == 'Y')) leave_game(a);
        else if (k.type == K_CHAR || k.type == K_ESC || k.type == K_ENTER) a->modal = MODAL_NONE;
        a->dirty = true;
        return;
    }
    if (a->modal == MODAL_RESIGN) {
        if (k.type == K_CHAR && (k.ch == 'y' || k.ch == 'Y')) { online_action(&a->online, "resign"); a->modal = MODAL_NONE; }
        else if (k.type == K_CHAR || k.type == K_ESC || k.type == K_ENTER) a->modal = MODAL_NONE;
        a->dirty = true; return;
    }
    if (a->modal == MODAL_CONFIRM_NEW) {
        if (k.type == K_CHAR && (k.ch == 'y' || k.ch == 'Y')) { a->modal = MODAL_NONE; if (a->mode == MODE_LOCAL) start_local(a); else start_game(a); }
        else if (k.type == K_CHAR || k.type == K_ESC || k.type == K_ENTER) a->modal = MODAL_NONE;
        a->dirty = true;
        return;
    }

    if (a->mode == MODE_OPENINGS && key_openings(a, k)) return;

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
            if (live_mode(a) || a->mode == MODE_ANALYZE) try_move_input(a);
            a->dirty = true;
            return;
        case K_ESC:
            if (a->input_len > 0) { a->input_len = 0; a->input[0] = 0; a->dirty = true; return; }
            if ((a->mode == MODE_PLAY || a->mode == MODE_LOCAL) && g->result == RES_ONGOING && g->n > 0)
                a->modal = MODAL_CONFIRM_QUIT;
            else leave_game(a);
            a->dirty = true;
            return;
        default: break;
    }
    if (k.type != K_CHAR) return;
    char c = k.ch;

    /* The analysis board takes moves for both sides, so it accepts the same
       typing as a live game. */
    bool movable = a->mode == MODE_ANALYZE ||
                   (live_mode(a) && g->result == RES_ONGOING);
    bool typing = a->input_len > 0;
    if (typing) {
        /* a move is being composed: every plausible character goes to the buffer */
        if (movable && is_move_char(c) && a->input_len < (int)sizeof a->input - 2) {
            a->input[a->input_len++] = c;
            a->input[a->input_len] = 0;
            a->dirty = true;
        }
        return;
    }
    /* Printable input is exclusively move notation; actions use C-x. */
    if (movable && is_move_start(c)) {
        a->input[a->input_len++] = c;
        a->input[a->input_len] = 0;
        a->dirty = true;
        return;
    }
    if (c == ' ') set_view(a, g->view + 1);
}

static void game_command(App *a, CommandId id) {
    Game *g = &a->game;
    switch (id) {
        case CMD_RESIGN: a->modal = MODAL_RESIGN; a->dirty = true; return;
        case CMD_DRAW: online_action(&a->online, "draw/yes"); return;
        case CMD_ABORT: online_action(&a->online, "abort"); return;
        case CMD_BACK:
            if ((a->mode == MODE_PLAY || a->mode == MODE_LOCAL) && g->result == RES_ONGOING && g->n > 0)
                a->modal = MODAL_CONFIRM_QUIT;
            else leave_game(a);
            a->dirty = true;
            return;
        case CMD_LINES: ana_toggle(a); return;
        case CMD_ROTATE: a->flip = !a->flip; a->dirty = true; return;
        case CMD_PIECES:
            a->piece_style = (a->piece_style + 1) % 3;
            a->dirty = true;
            return;
        case CMD_SAVE:
            default_save_name(a);
            a->modal = MODAL_SAVE;
            a->dirty = true;
            return;
        case CMD_STORE: save_to_db(a); return;
        case CMD_NOTE: begin_note(a); return;
        case CMD_NAG: cycle_nag(a); return;
        case CMD_UNDO: takeback(a); return;
        case CMD_LARGER: {
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
        case CMD_SMALLER: {
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
        case CMD_NEW:
            if (a->mode == MODE_PLAY || a->mode == MODE_LOCAL) { a->modal = MODAL_CONFIRM_NEW; a->dirty = true; }
            return;
        default: break;
    }
}

/* ------------------------------ input: menu ------------------------------ */
static void menu_prompt(App *a, MenuPrompt kind) {
    a->prompt = kind;
    a->msg[0] = 0;
    switch (kind) {
        case PROMPT_PGN:
            snprintf(a->path_input, sizeof a->path_input, "%s", a->loaded_path);
            break;
        case PROMPT_FEN:
            a->path_input[0] = 0;
            break;
        case PROMPT_DB: {
            char dir[PATH_MAX_CT];
            if (a->db.open) snprintf(a->path_input, sizeof a->path_input, "%s", a->db.dir);
            else snprintf(a->path_input, sizeof a->path_input, "%s",
                          db_default_dir(dir, sizeof dir));
            break;
        }
        default: a->path_input[0] = 0; break;
    }
    a->path_len = (int)strlen(a->path_input);
}

static void menu_activate(App *a) {
    switch (a->menu_item) {
        case MI_PLAY: case MI_DIFF: case MI_COLOR: start_game(a); break;
        case MI_LOCAL: start_local(a); break;
        case MI_ONLINE: open_online(a); break;
        case MI_TIME: case MI_INCREMENT: break;
        case MI_ANALYSIS: start_analysis_board(a, NULL); break;
        case MI_PGN: menu_prompt(a, PROMPT_PGN); break;
        case MI_FEN: menu_prompt(a, PROMPT_FEN); break;
        case MI_DB: menu_prompt(a, PROMPT_DB); break;
        case MI_OPENINGS: start_openings(a); break;
        default: a->quit = true; break;
    }
}

static void key_menu(App *a, Key k) {
    if (a->prompt != PROMPT_NONE) {
        if (k.type == K_ESC) a->prompt = PROMPT_NONE;
        else if (k.type == K_ENTER) {
            a->path_input[a->path_len] = 0;
            MenuPrompt kind = a->prompt;
            a->prompt = PROMPT_NONE;
            if (kind == PROMPT_PGN) open_pgn(a, a->path_input);
            else if (kind == PROMPT_FEN) open_fen(a, a->path_input);
            else open_db(a, a->path_input);
        } else if (k.type == K_BACKSPACE) {
            if (a->path_len > 0) a->path_input[--a->path_len] = 0;
        } else if (k.type == K_CHAR && a->path_len < (int)sizeof a->path_input - 2) {
            a->path_input[a->path_len++] = k.ch;
            a->path_input[a->path_len] = 0;
        }
        a->dirty = true;
        return;
    }
    switch (k.type) {
        case K_UP: a->menu_item = (a->menu_item + MI_COUNT - 1) % MI_COUNT; break;
        case K_DOWN: a->menu_item = (a->menu_item + 1) % MI_COUNT; break;
        case K_LEFT:
            if (a->menu_item == MI_TIME && a->menu_minutes > 1) a->menu_minutes--;
            if (a->menu_item == MI_INCREMENT && a->menu_increment > 0) a->menu_increment--;
            if (a->menu_item == MI_DIFF && a->menu_diff > 1) a->menu_diff--;
            if (a->menu_item == MI_COLOR) a->menu_color = (a->menu_color + 2) % 3;
            break;
        case K_RIGHT:
            if (a->menu_item == MI_TIME && a->menu_minutes < 180) a->menu_minutes++;
            if (a->menu_item == MI_INCREMENT && a->menu_increment < 180) a->menu_increment++;
            if (a->menu_item == MI_DIFF && a->menu_diff < 100) a->menu_diff++;
            if (a->menu_item == MI_COLOR) a->menu_color = (a->menu_color + 1) % 3;
            break;
        case K_PGUP:
            if (a->menu_item == MI_DIFF) { a->menu_diff += 10; if (a->menu_diff > 100) a->menu_diff = 100; }
            break;
        case K_PGDN:
            if (a->menu_item == MI_DIFF) { a->menu_diff -= 10; if (a->menu_diff < 1) a->menu_diff = 1; }
            break;
        case K_ENTER: menu_activate(a); break;
        default: break;
    }
    a->dirty = true;
}

/* ------------------------------ input: database ------------------------------ */
static void db_clamp(App *a) {
    if (a->db_idx >= a->db.nview) a->db_idx = a->db.nview - 1;
    if (a->db_idx < 0) a->db_idx = 0;
}

/* Keeps the highlighted game highlighted across a re-sort. */
static void db_keep_selection(App *a, const DbRec *want) {
    if (!want) { db_clamp(a); return; }
    for (int i = 0; i < a->db.nview; i++)
        if (db_at(&a->db, i) == want) { a->db_idx = i; db_clamp(a); return; }
    db_clamp(a);
}

static void key_db(App *a, Key k) {
    if (a->db_filtering) {
        if (k.type == K_ESC) {
            a->db_filtering = false;
            a->db_filter[0] = 0;
            a->db_filter_len = 0;
            db_set_filter(&a->db, "");
            a->db_idx = 0;
        } else if (k.type == K_ENTER) {
            a->db_filtering = false;
        } else if (k.type == K_BACKSPACE) {
            if (a->db_filter_len > 0) {
                a->db_filter[--a->db_filter_len] = 0;
                db_set_filter(&a->db, a->db_filter);
                a->db_idx = 0;
            }
        } else if (k.type == K_CHAR && a->db_filter_len < (int)sizeof a->db_filter - 1) {
            a->db_filter[a->db_filter_len++] = k.ch;
            a->db_filter[a->db_filter_len] = 0;
            db_set_filter(&a->db, a->db_filter);
            a->db_idx = 0;
        }
        db_clamp(a);
        a->dirty = true;
        return;
    }
    int n = a->db.nview;
    switch (k.type) {
        case K_UP: a->db_idx--; break;
        case K_DOWN: a->db_idx++; break;
        case K_PGUP: a->db_idx -= 10; break;
        case K_PGDN: a->db_idx += 10; break;
        case K_HOME: a->db_idx = 0; break;
        case K_END: a->db_idx = n - 1; break;
        case K_ENTER: if (n > 0) enter_db_game(a); break;
        case K_ESC: a->screen = SCR_MENU; a->force_clear = true; break;
        default: break;
    }
    db_clamp(a);
    a->dirty = true;
}

static void db_command(App *a, CommandId id) {
    switch (id) {
        case CMD_BACK: a->screen = SCR_MENU; a->force_clear = true; break;
        case CMD_SEARCH:
            a->db_filtering = true;
            a->msg[0] = 0;
            break;
        case CMD_DATE:
        case CMD_SORT: {
            const DbRec *sel = db_at(&a->db, a->db_idx);
            db_set_sort(&a->db, id == CMD_DATE ? DBS_DATE : (a->db.sort + 1) % DBS_COUNT);
            db_keep_selection(a, sel);
            set_msg(a, 0, "Sorted by %s, %s", db_sort_name(a->db.sort),
                    a->db.sort_desc ? "descending" : "ascending");
            break;
        }
        case CMD_REVERSE: {
            const DbRec *sel = db_at(&a->db, a->db_idx);
            db_set_sort(&a->db, a->db.sort);
            db_keep_selection(a, sel);
            set_msg(a, 0, "Sorted by %s, %s", db_sort_name(a->db.sort),
                    a->db.sort_desc ? "descending" : "ascending");
            break;
        }
        case CMD_REINDEX: {
            char dir[PATH_MAX_CT];
            if (a->db.single_file) path_join(dir, sizeof dir, a->db.dir, a->db.write_name);
            else snprintf(dir, sizeof dir, "%s", a->db.dir);
            remove(a->db.index_path);          /* force a full re-index */
            open_db(a, dir);
            break;
        }
        default: break;
    }
    db_clamp(a);
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
        default: break;
    }
    a->dirty = true;
}

/* Lichess lobby uses the same prefix registry as the rest of the app. */
static void online_command(App *a, CommandId id) {
    switch (id) {
        case CMD_LOGIN: online_login(&a->online); break;
        case CMD_SEEK: online_seek(&a->online, a->menu_minutes, a->menu_increment, a->menu_color); break;
        case CMD_RESUME:
            online_resume(&a->online);
            if (a->online.game_ready) { a->online.updated = true; }
            break;
        case CMD_CANCEL_SEEK: online_cancel_seek(&a->online); break;
        case CMD_LOGOUT:
            if (a->online.game_ready && !a->online.ended) set_msg(a, 1, "Finish your active game before logging out.");
            else online_logout(&a->online);
            break;
        case CMD_BACK:
            if (a->online.game_ready && !a->online.ended) { a->online.updated = true; }
            else { online_cancel_seek(&a->online); a->screen = SCR_MENU; }
            break;
        default: break;
    }
    a->force_clear = a->dirty = true;
}
static void key_online(App *a, Key k) {
    static const CommandId actions[] = {CMD_LOGIN, CMD_SEEK, CMD_RESUME, CMD_CANCEL_SEEK,
        CMD_ACCEPT, CMD_ACCEPT, CMD_ACCEPT, CMD_LOGOUT, CMD_BACK};
    if (k.type == K_UP) a->online_item = (a->online_item + 8) % 9;
    else if (k.type == K_DOWN) a->online_item = (a->online_item + 1) % 9;
    else if (k.type == K_ESC) online_command(a, CMD_BACK);
    else if (k.type == K_ENTER) online_command(a, actions[a->online_item]);
    else if (k.type == K_LEFT || k.type == K_RIGHT) {
        int d = k.type == K_RIGHT ? 1 : -1;
        if (a->online_item == 4 && a->menu_minutes + d >= 1 && a->menu_minutes + d <= 180) a->menu_minutes += d;
        if (a->online_item == 5 && a->menu_increment + d >= 0 && a->menu_increment + d <= 180) a->menu_increment += d;
        if (a->online_item == 6) a->menu_color = (a->menu_color + d + 3) % 3;
    }
    a->dirty = true;
}
static bool pump_online(App *a) {
    bool changed = online_poll(&a->online);
    if (a->online.updated) {
        if (a->mode != MODE_ONLINE) {
            opp_shutdown(a);
            if (a->ana.eng.ok) engine_quit(&a->ana.eng);
            a->ana.state = ANA_IDLE; ana_clear_lines(a);
        }
        bool new_game = a->mode != MODE_ONLINE || strcmp(a->game.tag_site, a->online.game.tag_site);
        if (new_game) { a->input_len = 0; a->input[0] = 0; a->modal = MODAL_NONE; }
        bool follow = new_game || a->game.view == a->game.n;
        int view = a->game.view;
        a->game = a->online.game;
        a->game.view = follow ? a->game.n : view > a->game.n ? a->game.n : view;
        a->player_side = a->online.side; a->flip = a->player_side < 0;
        a->mode = MODE_ONLINE; a->screen = SCR_GAME; a->ana.want = false;
        a->online.updated = false;
        a->force_clear = true; changed = true;
    }
    if (changed && (a->mode == MODE_ONLINE || a->screen == SCR_ONLINE))
        set_msg(a, 0, "%s", a->online.message);
    return changed;
}

/* ------------------------------ prefix command dispatch ------------------------------ */
static void direct_key(App *a, Key k) {
    switch (a->screen) {
        case SCR_MENU: key_menu(a, k); break;
        case SCR_PICKER: key_picker(a, k); break;
        case SCR_DB: key_db(a, k); break;
        case SCR_GAME: key_game(a, k); break;
        case SCR_ONLINE: key_online(a, k); break;
    }
}

static void execute_command(App *a, CommandId id) {
    switch (id) {
        case CMD_ACCEPT: direct_key(a, (Key){K_ENTER, 0}); return;
        case CMD_CANCEL: direct_key(a, (Key){K_ESC, 0}); return;
        case CMD_YES: direct_key(a, (Key){K_CHAR, 'y'}); return;
        case CMD_QUEEN: finish_promo(a, 'Q'); return;
        case CMD_ROOK: finish_promo(a, 'R'); return;
        case CMD_BISHOP: finish_promo(a, 'B'); return;
        case CMD_KNIGHT: finish_promo(a, 'N'); return;
        default: break;
    }
    if (a->screen == SCR_ONLINE) { online_command(a, id); return; }
    if (a->screen == SCR_GAME) { game_command(a, id); return; }
    if (a->screen == SCR_DB) { db_command(a, id); return; }
    if (a->screen == SCR_PICKER) { direct_key(a, (Key){K_ESC, 0}); return; }
    switch (id) {
        case CMD_BACK: a->quit = true; break;
        case CMD_LOCAL: start_local(a); break;
        case CMD_ONLINE: open_online(a); break;
        case CMD_PLAY: start_game(a); break;
        case CMD_ANALYSIS: start_analysis_board(a, NULL); break;
        case CMD_PGN: menu_prompt(a, PROMPT_PGN); break;
        case CMD_FEN: menu_prompt(a, PROMPT_FEN); break;
        case CMD_DB: menu_prompt(a, PROMPT_DB); break;
        case CMD_OPENINGS: start_openings(a); break;
        default: break;
    }
}

static void dispatch_key(App *a, Key k) {
    if (k.type == K_CTRL && k.ch == 'x') {
        a->commands_open = !a->commands_open;
        a->command_unknown = false;
        a->force_clear = a->dirty = true;
        return;
    }
    if (a->commands_open) {
        if (k.type == K_ESC || (k.type == K_CTRL && k.ch == 'g')) {
            a->commands_open = false;
        } else {
            Command commands[COMMAND_MAX];
            int count = app_commands(a, commands);
            for (int i = 0; i < count; i++) {
                if (k.type == K_CHAR && k.ch == commands[i].key) {
                    a->commands_open = false;
                    execute_command(a, commands[i].id);
                    break;
                }
            }
            a->command_unknown = a->commands_open;
        }
        a->force_clear = a->dirty = true;
        return;
    }
    if (k.type == K_CTRL) {
        if (k.ch != 'g') return;
        /* C-g cancels input, but never leaves a board or exits a screen. */
        bool editing = a->modal != MODAL_NONE || a->prompt != PROMPT_NONE ||
                       a->db_filtering || (a->screen == SCR_GAME && a->input_len > 0);
        if (!editing) return;
        k = (Key){K_ESC, 0};
    }
    direct_key(a, k);
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
        printf("ERROR: %s\n", file_exists(a->engine_path) ? "the engine is there but did not start."
                                                          : "no engine at that path.");
        if (e.proc.last_error) {
#ifdef _WIN32
            printf("  launch failed with error %d\n", e.proc.last_error);
#else
            printf("  launch failed: %s\n", strerror(e.proc.last_error));
#endif
            printf("  Run `make engine` to build it.\n");
        } else {
            /* it was launched but never answered: almost always a binary
               built for a different CPU, dying on an illegal instruction */
            printf("  It launched but never answered `uci` within 10s.\n"
                   "  Check it by hand:  echo uci | %s\n"
                   "  If that fails, the binary is not right for this CPU:"
                   " rebuild it with `make engine`.\n", a->engine_path);
        }
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

        /* Annotations are added on the way out so the round-trip proves that
           comments and move glyphs survive writing and reading back. */
        int notes_in = game_note_count(&g);
        game_set_note(&g, 0, "Annotation round-trip: start {braced} text");
        if (g.n > 2) {
            game_set_note(&g, 2, "a second annotation, deeper in the line");
            g.nag[1] = NAG_BRILLIANT;
            g.nag[0] = NAG_DUBIOUS;
        }
        int notes_out = game_note_count(&g);

        char tmp[256];
        snprintf(tmp, sizeof tmp, "%s.roundtrip.pgn", path);
        if (!pgn_save(&g, tmp, err, sizeof err)) { printf("     save: %s\n", err); fail++; continue; }
        PgnList *L2 = pgn_scan_file(tmp, err, sizeof err);
        Game g2;
        if (!L2 || !pgn_load_game(L2, 0, &g2, err, sizeof err) || g2.n != g.n) {
            printf("     round-trip FAILED (%s)\n", err);
            fail++;
        } else {
            printf("     round-trip ok (%d plies, %d note(s) read from the file)\n",
                   g2.n, notes_in);
            if (game_note_count(&g2) != notes_out) {
                printf("     ANNOTATIONS LOST: %d written, %d read back\n",
                       notes_out, game_note_count(&g2));
                fail++;
            }
            for (int k = 0; k <= g.n; k++)
                if (strcmp(game_note(&g, k), game_note(&g2, k))) {
                    printf("     ANNOTATION DIFFERS at ply %d:\n       out: %s\n       in : %s\n",
                           k, game_note(&g, k), game_note(&g2, k));
                    fail++;
                    break;
                }
            for (int k = 0; k < g.n; k++)
                if (g.nag[k] != g2.nag[k]) {
                    printf("     MOVE GLYPH DIFFERS at ply %d (%s vs %s)\n", k + 1,
                           nag_glyph(g.nag[k]), nag_glyph(g2.nag[k]));
                    fail++;
                    break;
                }
        }
        if (L2) pgn_list_free(L2);
        remove(tmp);
    }
    pgn_list_free(L);
    if (fail) printf("%d ERROR(S)\n", fail);
    else printf("PGN test passed.\n");
    return fail ? 1 : 0;
}

static int run_db_test(const char *path) {
    char dir[PATH_MAX_CT], err[128];
    if (path && *path) snprintf(dir, sizeof dir, "%s", path);
    else db_default_dir(dir, sizeof dir);
    printf("Collection: %s\n", dir);

    Db db;
    memset(&db, 0, sizeof db);
    if (!db_open(&db, dir, err, sizeof err)) { printf("ERROR: %s\n", err); return 1; }
    printf("cold open : %d games in %d file(s), %d file(s) parsed, %lld ms\n",
           db.nrecs, db.nfiles, db.scanned, db.open_ms);
    int cold_n = db.nrecs;
    long long cold_ms = db.open_ms;

    /* Second open must hit the index and parse nothing. */
    if (!db_open(&db, dir, err, sizeof err)) { printf("ERROR: %s\n", err); db_close(&db); return 1; }
    printf("warm open : %d games, %d file(s) parsed, %lld ms, %zu KB resident\n",
           db.nrecs, db.scanned, db.open_ms, db_memory_bytes(&db) / 1024);
    int fail = 0;
    if (db.nrecs != cold_n) { printf("FAIL: the warm open found a different number of games\n"); fail++; }
    if (db.scanned != 0) { printf("FAIL: the warm open re-parsed %d file(s)\n", db.scanned); fail++; }

    /* Reading one game must touch only its own byte range. */
    if (db.nview > 0) {
        Game *g = malloc(sizeof *g);
        if (!g) { printf("FAIL: out of memory\n"); db_close(&db); return 1; }
        long long t0 = now_ms();
        int loaded = 0;
        int probe = db.nview < 64 ? db.nview : 64;
        for (int i = 0; i < probe; i++)
            if (db_load(&db, i, g, err, sizeof err)) loaded++;
            else printf("  load %d failed: %s\n", i + 1, err);
        printf("loaded    : %d/%d games in %lld ms\n", loaded, probe, now_ms() - t0);
        if (loaded != probe) fail++;
        const DbRec *r = db_at(&db, 0);
        if (r) printf("first     : %s — %s, %d plies, %s\n", r->white, r->black,
                      r->plies, r->result == DBR_WHITE ? "1-0" :
                      r->result == DBR_BLACK ? "0-1" : r->result == DBR_DRAW ? "1/2-1/2" : "*");
        free(g);
    }

    /* Filtering never touches the files. */
    if (db.nview > 0) {
        const DbRec *r = db_at(&db, 0);
        char word[DB_NAME_MAX];
        snprintf(word, sizeof word, "%s", r->white);
        /* Typed one character at a time, the way the search field feeds it. */
        long long worst = 0, total = 0;
        int keys = (int)strlen(word);
        char typed[DB_NAME_MAX];
        db_set_filter(&db, "");
        for (int i = 0; i < keys; i++) {
            snprintf(typed, sizeof typed, "%.*s", i + 1, word);
            long long t0 = now_ms();
            db_set_filter(&db, typed);
            long long dt = now_ms() - t0;
            total += dt;
            if (dt > worst) worst = dt;
        }
        printf("filter    : \"%s\" matches %d/%d · %d keystrokes in %lld ms "
               "(worst %lld ms)\n", word, db.nview, db.nrecs, keys, total, worst);
        if (db.nview < 1) fail++;
        db_set_filter(&db, "");

        long long ts = now_ms();
        for (int i = 0; i < 20; i++) db_set_sort(&db, DBS_DATE);
        printf("sort      : %.2f ms per re-sort of %d games\n",
               (double)(now_ms() - ts) / 20, db.nview);
        db_set_sort(&db, DBS_NATURAL);
    }
    (void)cold_ms;
    db_close(&db);
    printf(fail ? "\n%d DATABASE TEST(S) FAILED\n" : "\nDatabase test passed.\n", fail);
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
    a->menu_minutes = 10; a->menu_increment = 0;
    a->menu_diff = 30;
    a->menu_color = 0;
    a->piece_style = 0;
    a->board_size = 3; /* start at the largest board that fits the terminal */
    srand((unsigned)time(NULL));

    const char *pgn_arg = NULL, *engine_arg = NULL, *fen_arg = NULL, *db_arg = NULL;
    bool want_db = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--https-test")) return online_https_test();
        if (!strcmp(argv[i], "--perft")) return run_perft();
        if (!strcmp(argv[i], "--db-test"))
            return run_db_test(i + 1 < argc ? argv[i + 1] : NULL);
        if (!strcmp(argv[i], "--openings-test")) return openings_test();
        if (!strcmp(argv[i], "--engine-test"))
            return run_engine_test(i + 1 < argc ? argv[i + 1] : NULL);
        if (!strcmp(argv[i], "--pgn-test") && i + 1 < argc)
            return run_pgn_test(argv[i + 1]);
        if (!strcmp(argv[i], "--engine") && i + 1 < argc) { engine_arg = argv[++i]; continue; }
        if (!strcmp(argv[i], "--fen") && i + 1 < argc) { fen_arg = argv[++i]; continue; }
        if (!strcmp(argv[i], "--db")) {
            want_db = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') db_arg = argv[++i];
            continue;
        }
        if (!strcmp(argv[i], "--version")) {
            printf("ChessTTY %s\n", CHESSTTY_VERSION);
            printf(
                   "License GPLv3+: GNU GPL version 3 or later <https://gnu.org/licenses/gpl.html>\n"
                   "Uses Stockfish <https://stockfishchess.org> as a separate UCI engine process.\n");
            return 0;
        }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("Usage: chesstty [game.pgn] [options]\n"
                   "  With no arguments opens the menu. With a PGN file opens analysis.\n"
                   "  --fen \"FEN\"         open the analysis board on that position\n"
                   "  --db [FOLDER]       open the games collection\n"
                   "                      (default: $XDG_DATA_HOME or ~/.chesstty, /games)\n"
                   "  --engine PATH       use a specific Stockfish binary\n"
                   "  --https-test        check HTTPS and certificate trust (no login)\n"
                   "  --perft             move generator self-test\n"
                   "  --openings-test     opening index and history self-test\n"
                   "  --engine-test       engine communication self-test\n"
                   "  --pgn-test FILE     PGN parser self-test\n"
                   "  --db-test [FOLDER]  collection index self-test\n");
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
    else if (fen_arg) open_fen(a, fen_arg);
    else if (want_db) open_db(a, db_arg);

    while (!a->quit && !g_sigint) {
        bool act = flag_clock(a);
        Key k;
        while ((k = term_read_key()).type != K_NONE) {
            dispatch_key(a, k);
            act = true;
            if (a->quit) break;
        }
        if (a->quit || g_sigint) break;

        act |= pump_online(a);
        act |= pump_opponent(a);
        act |= pump_analyzer(a);

        if (now_ms() - a->clock_redraw >= 100 &&
            ((a->screen == SCR_GAME && live_mode(a)) || a->ana.state == ANA_RUNNING)) {
            a->clock_redraw = now_ms(); act = true;
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
    online_close(&a->online);
    db_close(&a->db);
    sb_free(&a->fb);
    term_restore();
    return 0;
}
