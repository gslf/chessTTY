/* SPDX-License-Identifier: GPL-3.0-or-later */
/* UI boundary double: protocol behavior is tested separately with fixtures. */
#include "online.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void message(Online *o, const char *text) {
    snprintf(o->message, sizeof o->message, "%s", text); o->changed = true;
}
static void unexpected(void) {
    fputs("Unexpected online action in UI test; supply an explicit fixture.\n", stderr);
    abort();
}
bool online_init(Online *o) { (void)o; return true; }
void online_close(Online *o) { memset(o, 0, sizeof *o); }
void online_login(Online *o) { (void)o; unexpected(); }
void online_logout(Online *o) { online_close(o); message(o, "Logged out of ChessTTY."); }
void online_seek(Online *o, int minutes, int increment, int color) {
    (void)minutes; (void)increment; (void)color;
    if (o->authenticated) unexpected();
    message(o, "Log in to Lichess first.");
}
void online_resume(Online *o) {
    if (o->authenticated) unexpected();
    message(o, "Log in to Lichess first.");
}
void online_cancel_seek(Online *o) { o->seeking = false; message(o, "Matchmaking canceled."); }
void online_move(Online *o, Move m) { (void)o; (void)m; unexpected(); }
void online_action(Online *o, const char *action) { (void)o; (void)action; unexpected(); }
bool online_poll(Online *o) { bool changed = o->changed; o->changed = false; return changed; }
int online_https_test(void) { unexpected(); return 1; }
