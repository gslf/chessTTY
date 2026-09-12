/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ONLINE_H
#define ONLINE_H
#include "game.h"
#include "clocks.h"
#include <stdbool.h>
typedef struct OnlineNet OnlineNet;
typedef struct {
    OnlineNet *net;
    Game game;
    ChessClock clock;
    char user[80], user_id[80], game_id[24], message[200], login_url[1024];
    bool authenticated, connecting, seeking, game_ready, updated, changed;
    bool connected, move_pending, ended, draw_offer;
    int side;
} Online;
int online_https_test(void); /* read-only release smoke test, no account */
bool online_init(Online *o);
void online_close(Online *o);
void online_login(Online *o);
void online_logout(Online *o);
void online_seek(Online *o, int minutes, int increment, int color);
void online_cancel_seek(Online *o);
void online_resume(Online *o);
void online_move(Online *o, Move move);
void online_action(Online *o, const char *action); /* resign, abort, draw/yes */
bool online_poll(Online *o);
/* Deterministic state reducer, also used by protocol fixture tests. */
bool online_game_event(Online *o, const char *json, int64_t now);
#endif
