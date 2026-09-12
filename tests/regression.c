/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "chess.h"
#include "game.h"
#include "pgn.h"
#include "db.h"
#include "engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "line %d: %s\n", __LINE__, #expr); exit(1); } } while (0)
static Game g, loaded;
static char err[256];

static PgnList *scan(const char *text) {
    size_t n = strlen(text);
    char *buf = malloc(n + 1);
    CHECK(buf);
    memcpy(buf, text, n + 1);
    PgnList *list = pgn_scan_mem(buf, n, err, sizeof err);
    CHECK(list);
    return list;
}

static void chess_tests(void) {
    const char *bad[] = {
        "4k3/8/8/8/8/8/4K3 w - - 0 1",
        "4k3/7/8/8/8/8/8/4K3 w - - 0 1",
        "4k3/8/8/8/8/8/8/4K2 w - - 0 1",
        "4k3/8/8/8/8/8/8/3KK3 w - - 0 1",
        "4k3/8/8/8/8/8/8/4K2P w - - 0 1",
        "4k3/8/8/8/8/8/8/4K3 w X - 0 1",
        "4k3/8/8/8/8/8/8/4K3 w - e6 0 1",
        "4k3/8/8/8/8/8/8/4K3 w - e9 0 1",
        "4k3/8/8/8/8/8/8/4K3 w - - -1 1",
        "4k3/8/8/8/8/8/8/4K3 w - - 0 999999999999999999999",
        "4k3/8/8/8/8/8/8/4K3 w - - 0 0",
        "4k3/8/8/8/8/8/8/4K3 w - - 0 1 junk"
    };
    Pos p, original;
    pos_start(&original);
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        p = original;
        CHECK(!pos_from_fen(&p, bad[i]));
        CHECK(!memcmp(&p, &original, sizeof p));
    }
    CHECK(pos_from_fen(&p, "4k3/8/8/8/8/8/8/4K3 w - -"));
    CHECK(p.fullmove == 1 && p.halfmove == 0);
    PosKey ep, no_ep;
    CHECK(pos_from_fen(&p, "k3r3/8/8/3pP3/8/8/8/4K3 w - d6 0 1"));
    pos_key(&p, &ep);
    p.ep = -1;
    pos_key(&p, &no_ep);
    CHECK(poskey_eq(&ep, &no_ep)); /* pinned pawn cannot capture */
    CHECK(pos_from_fen(&p, "k7/8/8/3pP3/8/8/8/4K3 w - d6 0 1"));
    pos_key(&p, &ep);
    p.ep = -1;
    pos_key(&p, &no_ep);
    CHECK(!poskey_eq(&ep, &no_ep));
    game_reset(&g, "7k/6Q1/5K2/8/8/8/8/8 b - - 0 1");
    CHECK(g.result == RES_WHITE);
    game_reset(&g, "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");
    CHECK(g.result == RES_DRAW);
    game_reset(&g, NULL);
    for (int i = 0; i < 8; i++) {
        const char *cycle[] = {"Nf3", "Nf6", "Ng1", "Ng8"};
        Move m;
        CHECK(san_to_move(&g.pos[g.n], cycle[i % 4], &m) && game_push(&g, m));
    }
    CHECK(game_repetitions(&g) == 3 && g.result == RES_DRAW);
}

static void note_tests(void) {
    game_reset(&g, NULL);
    g.n = 64;
    char full[NOTE_MAX];
    memset(full, 'x', sizeof full - 1);
    full[sizeof full - 1] = 0;
    for (int i = 0; i < 63; i++) CHECK(game_set_note(&g, i, full));
    CHECK(game_set_note(&g, 63, "keep"));
    CHECK(!game_set_note(&g, 63, full));
    CHECK(!strcmp(game_note(&g, 63), "keep"));
    CHECK(game_set_note(&g, 0, game_note(&g, 63)));
    CHECK(!strcmp(game_note(&g, 0), "keep"));
    CHECK(game_set_note(&g, 0, game_note(&g, 0)));
    CHECK(!strcmp(game_note(&g, 0), "keep"));
}

static void pgn_tests(const char *path) {
    PgnList *list = scan("[Site \"Rome\"]\n[Round \"7\"]\n\n1.e4 (1.d4 ; ) is only a comment\n d5) e5 $999999999999999999999999999 1-0");
    CHECK(pgn_load_game(list, 0, &g, err, sizeof err));
    CHECK(g.n == 2 && g.result == RES_WHITE);
    CHECK(!strcmp(g.tag_site, "Rome") && !strcmp(g.tag_round, "7"));
    pgn_list_free(list);
    strcpy(g.tag_event, "Quote \" and backslash \\");
    CHECK(pgn_save(&g, path, err, sizeof err));
    list = pgn_scan_file(path, err, sizeof err);
    CHECK(list && pgn_load_game(list, 0, &loaded, err, sizeof err));
    CHECK(!strcmp(g.tag_event, loaded.tag_event));
    CHECK(!strcmp(g.tag_site, loaded.tag_site));
    CHECK(!strcmp(g.tag_round, loaded.tag_round));
    pgn_list_free(list);
    long off, len;
    CHECK(pgn_append(&g, path, &off, &len, err, sizeof err));
    CHECK(off > 0 && pgn_load_at(path, off, len, &loaded, err, sizeof err));
    CHECK(!pgn_load_at(path, off, len + 10, &loaded, err, sizeof err));
    list = scan("[FEN \"r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1\"]\n\n1.0-0 0-0-0 *");
    CHECK(pgn_load_game(list, 0, &g, err, sizeof err) && g.n == 2);
    pgn_list_free(list);
    list = scan("1.e4 * ; no trailing newline");
    CHECK(pgn_load_game(list, 0, &g, err, sizeof err));
    pgn_list_free(list);
}

static void db_tests(const char *path) {
    Db db = {0};
    CHECK(db_open(&db, path, err, sizeof err));
    CHECK(db.nrecs == 2);
    char index[PATH_MAX_CT];
    snprintf(index, sizeof index, "%s", db.index_path);
    db_close(&db);
    CHECK(db_open(&db, path, err, sizeof err) && db.scanned == 0);
    db_set_filter(&db, "White");
    int matches = db.nview;
    db_set_sort(&db, DBS_WHITE);
    CHECK(db.nview == matches);
    db_close(&db);
    /* Corrupt a string in the on-disk file table, preserving index length. */
    FILE *f = fopen(index, "r+b");
    CHECK(f && fseek(f, 32, SEEK_SET) == 0);
    for (int i = 0; i < PATH_MAX_CT; i++) CHECK(fputc('x', f) != EOF);
    CHECK(fclose(f) == 0);
    CHECK(db_open(&db, path, err, sizeof err) && db.scanned == 1 && db.nrecs == 2);
    db_close(&db);
    /* Corrupt the first record's player field. */
    f = fopen(index, "r+b");
    CHECK(f && fseek(f, 32 + (long)sizeof(DbFile) + (long)offsetof(DbRec, white), SEEK_SET) == 0);
    for (int i = 0; i < DB_NAME_MAX; i++) CHECK(fputc('x', f) != EOF);
    CHECK(fclose(f) == 0);
    CHECK(db_open(&db, path, err, sizeof err) && db.scanned == 1 && db.nrecs == 2);
    db_close(&db);
    remove(index);
}

static void explorer_tests(const char *path) {
    const char *pgn =
        "[White \"A player with a name longer than thirty two characters\"]\n"
        "[Black \"Opponent\"]\n[Event \"Candidates\"]\n[Site \"Zurich\"]\n"
        "[Round \"Final-A\"]\n[Date \"2024.03.09\"]\n[Result \"1-0\"]\n"
        "[WhiteElo \"2712\"]\n[BlackElo \"2689\"]\n[ECO \"B90\"]\n\n1.e4 c5 1-0";
    PgnList *list = scan(pgn);
    CHECK(pgn_load_game(list, 0, &g, err, sizeof err));
    pgn_list_free(list);
    CHECK(pgn_save(&g, path, err, sizeof err));
    strcpy(g.tag_white, "Earlier");
    strcpy(g.tag_date, "1999.12.31");
    strcpy(g.tag_site, "Berlin");
    strcpy(g.tag_round, "Semi");
    g.tag_white_elo = g.tag_black_elo = 0;
    g.tag_eco[0] = 0;
    CHECK(pgn_append(&g, path, NULL, NULL, err, sizeof err));
    game_reset(&g, "4k3/8/8/8/8/8/8/R3K3 w - - 0 1");
    strcpy(g.tag_white, "Unknown date");
    CHECK(pgn_append(&g, path, NULL, NULL, err, sizeof err));

    Db db = {0};
    for (int pass = 0; pass < 2; pass++) {
        CHECK(db_open(&db, path, err, sizeof err) && db.nrecs == 3);
        CHECK(db.scanned == (pass == 0 ? 1 : 0));
        const char *queries[] = {"thirty two characters", "Zurich", "Final-A", "2024",
            "2024.03.09", "2024-03-09", "2024/03/09", "2712", "2689", "B90",
            "zUrIcH 2024 1-0", "Zurich 1 moves", "Zurich 2 plies",
            "A player with a name longer than thirty two characters Zurich"};
        for (size_t i = 0; i < sizeof queries / sizeof *queries; i++) {
            db_set_filter(&db, queries[i]);
            CHECK(db.nview == 1 && !strcmp(db_at(&db, 0)->site, "Zurich"));
        }
        db_set_filter(&db, "setup R3K3");
        CHECK(db.nview == 1 && !strcmp(db_at(&db, 0)->white, "Unknown date"));
        db_set_filter(&db, "standard");
        CHECK(db.nview == 2);
        db_set_filter(&db, db.files[0].name);
        CHECK(db.nview == 3);
        db_set_filter(&db, "Zur");
        db_set_filter(&db, "Zurich 2712");
        CHECK(db.nview == 1);
        db_set_filter(&db, "Zurich 2713");
        CHECK(db.nview == 0);
        db_set_filter(&db, "");
        db_set_sort(&db, DBS_DATE);
        CHECK(db.sort_desc && db_at(&db, 0)->year == 2024 && db_at(&db, 1)->year == 1999);
        CHECK(db_at(&db, 2)->year == 0);
        db_set_sort(&db, DBS_DATE);
        CHECK(!db.sort_desc && db_at(&db, 0)->year == 1999 && db_at(&db, 1)->year == 2024);
        CHECK(db_at(&db, 2)->year == 0);
        db_set_filter(&db, "Zurich");
        CHECK(db_load(&db, 0, &loaded, err, sizeof err));
        CHECK(loaded.tag_white_elo == 2712 && loaded.tag_black_elo == 2689);
        CHECK(!strcmp(loaded.tag_eco, "B90"));
        if (pass == 1) {
            CHECK(db_add(&db, &loaded, err, sizeof err));
            CHECK(db.nview == 2 && db.nrecs == 4);
            CHECK(db_open(&db, path, err, sizeof err) && db.scanned == 0 && db.nrecs == 4);
            db_set_filter(&db, "Zurich Final-A 2712 B90");
            CHECK(db.nview == 2);
            remove(db.index_path);
        }
        db_close(&db);
    }
}

int main(int argc, char **argv) {
    CHECK(argc == 2); /* caller provides an isolated scratch PGN */
    chess_tests();
    note_tests();
    pgn_tests(argv[1]);
    db_tests(argv[1]);
    explorer_tests(argv[1]);
    remove(argv[1]);
    puts("Regression tests passed (FEN, repetition, notes, PGN, database cache, metadata search and date sorting).");
    return 0;
}
