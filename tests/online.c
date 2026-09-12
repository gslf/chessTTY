/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "portable.h"
#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif
#include "online.h"
#include "auth.h"
#include "json.h"
#include "clocks.h"
#include "pgn.h"
#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)
static Online o;
static Game game;
static App app;
static const char *full =
 "{\"type\":\"gameFull\",\"id\":\"abcd1234\",\"variant\":{\"key\":\"standard\"},"
 "\"white\":{\"id\":\"alice\",\"name\":\"Alice\"},\"black\":{\"id\":\"bob\",\"name\":\"Bob\"},"
 "\"initialFen\":\"startpos\",\"clock\":{\"initial\":600000,\"increment\":2000},"
 "\"state\":{\"type\":\"gameState\",\"moves\":\"e2e4 e7e5\",\"wtime\":599000,\"btime\":598000,\"winc\":2000,\"binc\":2000,\"status\":\"started\"}}";
static void clocks(void) {
    ChessClock c; clock_start(&c, 60000, 2000, 1, 100);
    for (int i = 0; i < 100000; i++) CHECK(clock_remaining(&c, 1, 2500) == 57600);
    CHECK(clock_remaining(&c, -1, 2500) == 60000);
    CHECK(clock_move(&c, 1, 2500));
    CHECK(clock_remaining(&c, 1, 9000) == 59600 && clock_remaining(&c, -1, 9000) == 53500);
    CHECK(!clock_move(&c, 1, 9000));
    clock_stop(&c, 9000);
    CHECK(clock_remaining(&c, -1, 100000) == 53500);
    clock_start(&c, 1000, 2000, 1, 0);
    CHECK(!clock_move(&c, 1, 1000));
    CHECK(clock_remaining(&c, 1, 999999) == 0);
    clock_sync(&c, 12345, 67890, -1, true, 100);
    CHECK(clock_remaining(&c, 1, 1000) == 12345 && clock_remaining(&c, -1, 1000) == 66990);
    char out[24]; clock_format(1, out, sizeof out); CHECK(!strcmp(out, "0:00.1"));
    clock_format(0, out, sizeof out); CHECK(!strcmp(out, "0:00.0"));
    int64_t ms;
    CHECK(clock_parse("1:02:03.456", &ms) && ms == 3723456);
    CHECK(!clock_parse("1:99:00", &ms)); CHECK(!clock_parse("1", &ms));
}
static void parser(void) {
    Json j; char out[64];
    CHECK(json_parse(&j, "{\"outer\":{\"id\":\"wrong\"},\"id\":\"right\",\"n\":9223372036854775808}"));
    CHECK(json_string(&j, json_get(&j, 0, "id"), out, sizeof out) && !strcmp(out, "right"));
    CHECK(json_number(&j, json_get(&j, 0, "n"), -1) == -1);
    CHECK(json_parse(&j, "{\"x\":\"caf\\u00e9 \\ud83d\\ude00\"}"));
    CHECK(json_string(&j, json_get(&j, 0, "x"), out, sizeof out) && !strcmp(out, "café 😀"));
    CHECK(!json_parse(&j, "{\"a\":1,}")); CHECK(!json_parse(&j, "[1,,2]"));
    CHECK(!json_parse(&j, "{\"a\":\"\\uZZZZ\"}"));
    CHECK(json_parse(&j, "{\"x\":\"text\\ud800\"}"));
    CHECK(!json_string(&j, json_get(&j, 0, "x"), out, sizeof out) && !*out);
    char challenge[44];
    CHECK(oauth_challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk", challenge));
    CHECK(!strcmp(challenge, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM")); /* RFC 7636 */
}
static void callbacks(void) {
#ifndef _WIN32
    OAuth a = {0}; char url[1024], code[128];
    CHECK(oauth_begin(&a, url, sizeof url, 100));
    CHECK(strstr(url, "code_challenge_method=S256") && strstr(url, "scope=board%3Aplay"));
    unsigned port; CHECK(sscanf(a.redirect, "http://127.0.0.1:%u/callback", &port) == 1);
    for (int valid = 0; valid < 2; valid++) {
        int sock = socket(AF_INET, SOCK_STREAM, 0); CHECK(sock >= 0);
        struct sockaddr_in addr = {0}; addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = htons((unsigned short)port);
        CHECK(connect(sock, (struct sockaddr *)&addr, sizeof addr) == 0);
        char req[256]; int len = snprintf(req, sizeof req, "GET /callback?code=test_code&state=%s HTTP/1.1\r\nHost: localhost\r\n\r\n", valid ? a.state : "wrong");
        CHECK(send(sock, req, (size_t)len, 0) == len);
        CHECK(oauth_poll(&a, code, sizeof code, 200) == valid);
        CHECK(a.active == !valid);
        close(sock);
    }
    CHECK(!strcmp(code, "test_code") && *a.verifier);
    oauth_close(&a); CHECK(!*a.verifier);
    CHECK(oauth_begin(&a, url, sizeof url, 100));
    CHECK(oauth_poll(&a, code, sizeof code, 180100) == -1 && !a.active);
#endif
}
static void pgn_clocks(void) {
    char comment[400]; memset(comment, 'a', sizeof comment - 1); comment[sizeof comment - 1] = 0;
    char err[128];
    const char *tc[] = {"600+2", "600", "600x", "600+", "999999999999999999999", "600+999999999999999"};
    for (int i = 0; i < 6; i++) {
        char *buf = malloc(1024); CHECK(buf);
        int n = snprintf(buf, 1024, "[Event \"Clock test\"]\n[TimeControl \"%s\"]\n\n1. e4 {%s [%%clk 0:09:59.123]} e5 {[%%clk 0:09:58.456]} *\n", tc[i], comment);
        PgnList *list = pgn_scan_mem(buf, (size_t)n, err, sizeof err);
        CHECK(list && pgn_load_game(list, 0, &game, err, sizeof err));
        CHECK(game.initial_ms == (i < 2 ? 600000 : 0));
        CHECK(game.clocks[1][0] == 599123 && game.clocks[2][0] == 599123 && game.clocks[2][1] == 598456);
        pgn_list_free(list);
    }
}
static void states(const char *path) {
    strcpy(o.user_id, "alice");
    CHECK(online_game_event(&o, full, 1000));
    CHECK(o.game.n == 2 && o.side == 1 && o.connected && o.clock.running);
    CHECK(clock_remaining(&o.clock, 1, 2234) == 597766);
    CHECK(clock_remaining(&o.clock, -1, 2234) == 598000);
    o.move_pending = true;
    const char *next = "{\"type\":\"gameState\",\"moves\":\"e2e4 e7e5 g1f3\",\"wtime\":599500,\"btime\":598000,\"winc\":2000,\"binc\":2000,\"status\":\"started\"}";
    CHECK(online_game_event(&o, next, 2500));
    CHECK(o.game.n == 3 && !o.move_pending && o.clock.side == -1);
    CHECK(o.game.clocks[2][0] == 599000 && o.game.clocks[3][0] == 599500);
    CHECK(online_game_event(&o, next, 3000) && o.game.n == 3); /* duplicate snapshot */
    CHECK(!online_game_event(&o, "{\"type\":\"gameState\",\"moves\":\"e2e4 e7e5 g1f3 z9z9\",\"status\":\"started\"}", 4000));
    CHECK(o.game.n == 3 && o.game.clocks[3][0] == 599500);
    CHECK(online_game_event(&o, full, 4000) && o.game.n == 2); /* takeback/resync */
    const char *mate = "{\"type\":\"gameState\",\"moves\":\"e2e4 e7e5\",\"wtime\":599000,\"btime\":0,\"status\":\"outoftime\",\"winner\":\"white\"}";
    CHECK(online_game_event(&o, mate, 5000) && o.ended && !o.clock.running && o.game.result == RES_WHITE);
    char err[128]; CHECK(pgn_save(&o.game, path, err, sizeof err));
    PgnList *list = pgn_scan_file(path, err, sizeof err);
    CHECK(list && pgn_load_game(list, 0, &game, err, sizeof err));
    CHECK(game.initial_ms == 600000 && game.increment_ms == 2000);
    CHECK(game.clocks[2][1] == 0);
    pgn_list_free(list); remove(path);
    /* Online and local games must not offer engine help through the palette. */
    app.screen = SCR_GAME;
    for (int i = 0; i < 2; i++) {
        app.mode = i ? MODE_ONLINE : MODE_LOCAL;
        Command commands[COMMAND_MAX]; int n = app_commands(&app, commands);
        for (int k = 0; k < n; k++) CHECK(commands[k].id != CMD_LINES);
    }
}
int main(int argc, char **argv) {
    CHECK(argc == 2); clocks(); parser(); callbacks(); pgn_clocks(); states(argv[1]);
    puts("Online/clock tests passed: PKCE, JSON, authoritative snapshots, reconnect, flag boundaries, PGN clocks, engine isolation.");
    return 0;
}
