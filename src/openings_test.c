/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "openings.h"
#include "game.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static int visited, named;
static bool walk(uint16_t parent, const Pos *pos, int depth) {
    if (depth >= MAXPLY) return false;
    Move legal[MAX_MOVES];
    int n = gen_legal(pos, legal);
    for (uint16_t node = opening_child(parent); node; node = opening_next(node)) {
        Move m = opening_move(node);
        bool found = false;
        for (int i = 0; i < n; i++)
            if (legal[i].from == m.from && legal[i].to == m.to && legal[i].promo == m.promo)
                found = true;
        if (!found || ++visited > 65535) return false;
        Pos next = *pos;
        make_move(&next, m);
        if (opening_label(node)) {
            PosKey key;
            pos_key(&next, &key);
            if (opening_lookup(&key) != opening_label(node) ||
                !*opening_name(opening_label(node))) return false;
            named++;
        }
        if (!walk(node, &next, depth + 1)) return false;
    }
    return true;
}

static bool push(Game *g, const char *san) {
    Move m;
    return san_to_move(&g->pos[g->n], san, &m) && game_push(g, m);
}

static bool push_unnamed(Game *g) {
    Move moves[MAX_MOVES];
    int count = gen_legal(&g->pos[g->n], moves);
    for (int i = 0; i < count; i++) {
        Pos pos = g->pos[g->n];
        PosKey key;
        make_move(&pos, moves[i]);
        pos_key(&pos, &key);
        if (!opening_lookup(&key)) return game_push(g, moves[i]);
    }
    return false;
}

int openings_test(void) {
    Game g;
    Pos pos;
    pos_start(&pos);
    visited = named = 0;
    if (!walk(0, &pos, 0) || named < 3000) {
        fprintf(stderr, "Opening tree contains invalid moves or missing labels\n");
        return 1;
    }
#define CHECK(test) do { if (!(test)) { fprintf(stderr, "Opening test failed: %s (line %d)\n", #test, __LINE__); return 1; } } while (0)
    game_reset(&g, NULL);
    CHECK(!g.opening[0]);
    CHECK(push(&g, "e4") && push(&g, "c5"));
    CHECK(strstr(opening_name(g.opening[2]), "Sicilian Defense"));
    uint16_t sicilian = g.opening[2];
    CHECK(push_unnamed(&g) && push_unnamed(&g));
    CHECK(g.opening[4] == sicilian); /* retain most recent name outside book */
    CHECK(g.opening[2] == sicilian); /* browsing is per-ply, never the future name */
    g.n = 1;
    CHECK(push(&g, "e5"));
    CHECK(g.opening[2] != sicilian); /* undo and branch replace cached labels */

    /* Same named position through a different move order. */
    game_reset(&g, NULL);
    CHECK(push(&g, "d4") && push(&g, "d5") && push(&g, "c4") && push(&g, "e6") &&
          push(&g, "Nc3") && push(&g, "Nf6"));
    uint16_t qgd = g.opening[g.n];
    PosKey qgd_key = g.keys[g.n];
    CHECK(qgd && opening_lookup(&qgd_key) == qgd);
    game_reset(&g, NULL);
    CHECK(push(&g, "d4") && push(&g, "Nf6") && push(&g, "c4") && push(&g, "e6") &&
          push(&g, "Nc3") && push(&g, "d5"));
    CHECK(poskey_eq(&qgd_key, &g.keys[g.n]) && g.opening[g.n] == qgd);
    char fen[128];
    pos_to_fen(&g.pos[g.n], fen, sizeof fen);
    game_reset(&g, fen);
    CHECK(g.opening[0] == qgd); /* custom-FEN PGNs use the same index */
    game_reset(&g, "8/8/8/8/8/4k3/8/4K3 w - - 0 1");
    CHECK(!g.opening[0]);
    clock_t start = clock();
    volatile unsigned sum = 0;
    for (int i = 0; i < 1000000; i++)
        sum += opening_lookup(i & 1 ? &qgd_key : &g.keys[0]);
    double elapsed = (double)(clock() - start) / CLOCKS_PER_SEC;
    CHECK(sum > 0);
    printf("Openings OK: %d nodes, %d named lines, %zu bytes read-only data\n",
           visited + 1, named, opening_storage_bytes());
    printf("1,000,000 repeated lookups (hits/misses): %.3f s; %.3f microseconds/lookup\n", elapsed, elapsed);
    return 0;
#undef CHECK
}
