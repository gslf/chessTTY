/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "portable.h"
#include "online.h"
#include "auth.h"
#include "json.h"
#include "platform.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef CHESSTTY_TEST_MOCKS
/* Test fixtures supply game states; dispatching real HTTP is an error. */
CURLcode test_http_forbidden(CURL *easy);
CURLMcode test_stream_forbidden(CURLM *multi, CURL *easy);
#define curl_easy_perform test_http_forbidden
#define curl_multi_add_handle test_stream_forbidden
#endif

#define HOST "https://lichess.org"
enum { REQUEST, EVENTS, GAME, SEEK, CHANNELS };
enum { REQ_NONE, REQ_TOKEN, REQ_ACCOUNT, REQ_MOVE, REQ_ACTION };
typedef struct {
    CURL *easy;
    struct curl_slist *headers;
    char data[65536];
    size_t used;
    int kind;
    int64_t last_activity;
} Request;
struct OnlineNet {
    CURLM *multi;
    Request req[CHANNELS];
    OAuth auth;
    char token[2048];
    int64_t retry[CHANNELS], blocked_until, move_deadline;
    int backoff[CHANNELS];
    bool listening, seek_requested;
    int minutes, increment, color;
};
static void message(Online *o, const char *s) {
    snprintf(o->message, sizeof o->message, "%s", s); o->changed = true;
}
static int member(const Json *j, int obj, const char *key) { return json_get(j, obj, key); }
static bool string(const Json *j, int obj, const char *key, char *out, size_t n) {
    return json_string(j, member(j, obj, key), out, n);
}
static int64_t number(const Json *j, int obj, const char *key, int64_t fallback) {
    return json_number(j, member(j, obj, key), fallback);
}
static bool safe_id(const char *id) {
    size_t n = strlen(id);
    if (n < 8 || n > 16) return false;
    for (size_t i = 0; i < n; i++)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'z') || (id[i] >= 'A' && id[i] <= 'Z'))) return false;
    return true;
}
static bool server_move(const Pos *p, const char *text, Move *m) {
    if (uci_to_move(p, text, m)) return true;
    /* Board API can encode standard castling as king-to-rook. */
    if (!strcmp(text, "e1h1") || !strcmp(text, "e1a1") || !strcmp(text, "e8h8") || !strcmp(text, "e8a8")) {
        char uci[8]; snprintf(uci, sizeof uci, "%s", text);
        uci[2] = text[2] == 'h' ? 'g' : 'c';
        int from = SQ(4, text[1] - '1');
        if (p->sq[from] == KING * p->stm) return uci_to_move(p, uci, m);
    }
    return false;
}
static bool game_event(Online *o, const char *text, int64_t now) {
    Json j;
    if (!json_parse(&j, text)) return false;
    int state = 0;
    bool full = json_is(&j, member(&j, 0, "type"), "gameFull");
    if (!full && !json_is(&j, member(&j, 0, "type"), "gameState")) return true;
    if (full) {
        int variant = member(&j, 0, "variant");
        if (!json_is(&j, member(&j, variant, "key"), "standard") &&
            !json_is(&j, member(&j, variant, "key"), "fromPosition")) {
            message(o, "This game uses an unsupported chess variant."); return false;
        }
        char id[24], fen[128], white_id[80], black_id[80];
        if (!string(&j, 0, "id", id, sizeof id) || !safe_id(id) ||
            !string(&j, 0, "initialFen", fen, sizeof fen)) return false;
        int white = member(&j, 0, "white"), black = member(&j, 0, "black");
        string(&j, white, "id", white_id, sizeof white_id);
        string(&j, black, "id", black_id, sizeof black_id);
        int side = !strcmp(white_id, o->user_id) ? 1 : !strcmp(black_id, o->user_id) ? -1 : 0;
        if (!side) return false;
        if (!o->game_ready || strcmp(o->game_id, id)) {
            if (strcmp(fen, "startpos")) { Pos p; if (!pos_from_fen(&p, fen)) return false; }
            game_reset(&o->game, !strcmp(fen, "startpos") ? NULL : fen);
            int64_t created = number(&j, 0, "createdAt", 0);
            time_t when = created > 0 && created < 4102444800000LL ? (time_t)(created / 1000) : time(NULL);
            struct tm *date = gmtime(&when);
            if (date) strftime(o->game.tag_date, sizeof o->game.tag_date, "%Y.%m.%d", date);
        }
        snprintf(o->game_id, sizeof o->game_id, "%s", id); o->side = side;
        string(&j, white, "name", o->game.tag_white, sizeof o->game.tag_white);
        string(&j, black, "name", o->game.tag_black, sizeof o->game.tag_black);
        snprintf(o->game.tag_site, sizeof o->game.tag_site, HOST "/%s", id);
        snprintf(o->game.tag_event, sizeof o->game.tag_event, "Lichess online game");
        int clock = member(&j, 0, "clock");
        o->game.initial_ms = number(&j, clock, "initial", 0);
        o->game.increment_ms = number(&j, clock, "increment", 0);
        if (o->game.initial_ms > 0) o->game.clocks[0][0] = o->game.clocks[0][1] = o->game.initial_ms;
        state = member(&j, 0, "state");
    } else if (!o->game_ready) return false;
    char moves[8192], status[40], winner[16];
    if (!string(&j, state, "moves", moves, sizeof moves) ||
        !string(&j, state, "status", status, sizeof status)) return false;
    string(&j, state, "winner", winner, sizeof winner);
    const char *p = moves; int ply = 0;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        char uci[8]; size_t len = strcspn(p, " ");
        if (len < 4 || len > 5 || ply >= MAXPLY) return false;
        memcpy(uci, p, len); uci[len] = 0; p += len;
        Move m;
        if (!server_move(&o->game.pos[ply], uci, &m)) return false;
        if (ply < o->game.n && (m.from != o->game.moves[ply].from || m.to != o->game.moves[ply].to || m.promo != o->game.moves[ply].promo))
            game_truncate(&o->game, ply);
        if (ply == o->game.n && !game_push(&o->game, m)) return false;
        ply++;
    }
    if (ply < o->game.n) game_truncate(&o->game, ply);
    bool ended = strcmp(status, "created") && strcmp(status, "started");
    o->ended = ended;
    o->game.result = !ended || !strcmp(status, "aborted") ? RES_ONGOING : !strcmp(winner, "white") ? RES_WHITE : !strcmp(winner, "black") ? RES_BLACK : RES_DRAW;
    snprintf(o->game.tag_result, sizeof o->game.tag_result, "%s", result_str(o->game.result));
    snprintf(o->game.reason, sizeof o->game.reason, "%s", ended ? status : "");
    int64_t wtime = number(&j, state, "wtime", -1), btime = number(&j, state, "btime", -1);
    if (wtime > INT32_MAX || btime > INT32_MAX || wtime < -1 || btime < -1) return false;
    clock_sync(&o->clock, wtime, btime, o->game.pos[ply].stm, !ended && ply >= 2, now);
    o->clock.increment[0] = number(&j, state, "winc", 0);
    o->clock.increment[1] = number(&j, state, "binc", 0);
    o->game.clocks[ply][0] = wtime; o->game.clocks[ply][1] = btime;
    o->draw_offer = json_is(&j, member(&j, state, o->side > 0 ? "bdraw" : "wdraw"), "true");
    o->connected = true;
    o->game_ready = true; o->updated = o->changed = true;
    if (o->game.pos[ply].stm != o->side || ended) o->move_pending = false;
    if (ended) message(o, o->game.reason);
    else if (o->draw_offer) message(o, "Opponent offers a draw.");
    return true;
}
bool online_game_event(Online *o, const char *text, int64_t now) {
    /* A malformed snapshot must not partially advance the live board. */
    Online before = *o;
    if (game_event(o, text, now)) return true;
    *o = before;
    return false;
}
static void stop(Online *o, int slot) {
    Request *r = &o->net->req[slot];
    if (r->easy) { curl_multi_remove_handle(o->net->multi, r->easy); curl_easy_cleanup(r->easy); }
    curl_slist_free_all(r->headers); memset(r, 0, sizeof *r);
}
static size_t receive(char *data, size_t size, size_t count, void *userdata) {
    Request *r = userdata;
    size_t n = size * count;
    r->last_activity = now_ms();
    if (n > sizeof r->data - 1 - r->used) return 0;
    memcpy(r->data + r->used, data, n); r->used += n; r->data[r->used] = 0;
    return n;
}
static bool request(Online *o, int slot, int kind, const char *path, const char *form, bool authenticated) {
    OnlineNet *net = o->net;
    if (!net || now_ms() < net->blocked_until || net->req[slot].easy) return false;
    Request *r = &net->req[slot];
    r->easy = curl_easy_init(); if (!r->easy) return false;
    r->kind = kind; r->last_activity = now_ms();
    char url[512]; snprintf(url, sizeof url, HOST "%s", path);
    curl_easy_setopt(r->easy, CURLOPT_URL, url);
    curl_easy_setopt(r->easy, CURLOPT_USERAGENT, "ChessTTY/1.0 (Board API)");
    curl_easy_setopt(r->easy, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(r->easy, CURLOPT_WRITEDATA, r);
    curl_easy_setopt(r->easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(r->easy, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(r->easy, CURLOPT_TCP_KEEPALIVE, 1L);
    /* TLS verification stays enabled; redirects cannot receive bearer tokens. */
    curl_easy_setopt(r->easy, CURLOPT_FOLLOWLOCATION, 0L);
#if defined(_WIN32) && defined(CURLSSLOPT_NATIVE_CA)
    curl_easy_setopt(r->easy, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif
    if (slot == REQUEST) curl_easy_setopt(r->easy, CURLOPT_TIMEOUT, 20L);

    if (authenticated) {
        char auth[2100]; snprintf(auth, sizeof auth, "Authorization: Bearer %s", net->token);
        r->headers = curl_slist_append(r->headers, auth);
        if (!r->headers) { stop(o, slot); return false; }
        curl_easy_setopt(r->easy, CURLOPT_HTTPHEADER, r->headers);
    }
    if (form) { curl_easy_setopt(r->easy, CURLOPT_POST, 1L); curl_easy_setopt(r->easy, CURLOPT_COPYPOSTFIELDS, form); }
    if (curl_multi_add_handle(net->multi, r->easy) != CURLM_OK) { stop(o, slot); return false; }
    return true;
}
bool online_init(Online *o) {
    if (o->net) return true;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return false;
    o->net = calloc(1, sizeof *o->net);
    if (!o->net) { curl_global_cleanup(); return false; }
    o->net->multi = curl_multi_init();
    if (!o->net->multi) { free(o->net); o->net = NULL; curl_global_cleanup(); return false; }
    return true;
}
void online_close(Online *o) {
    if (o->net) {
        for (int i = 0; i < CHANNELS; i++) stop(o, i);
        oauth_close(&o->net->auth);
        curl_multi_cleanup(o->net->multi);
        volatile char *p = o->net->token;
        for (size_t i = 0; i < sizeof o->net->token; i++) p[i] = 0;
        free(o->net); curl_global_cleanup();
    }
    memset(o, 0, sizeof *o);
}
void online_login(Online *o) {
    if (!online_init(o)) { message(o, "Cannot initialize HTTPS support."); return; }
    if (o->authenticated || o->connecting) return;
    if (!oauth_begin(&o->net->auth, o->login_url, sizeof o->login_url, now_ms())) { message(o, "Cannot start secure browser login."); return; }
    o->connecting = true;
    if (open_browser(o->login_url)) message(o, "Complete Lichess login in your browser. Session token stays in memory.");
    else { o->connecting = false; oauth_close(&o->net->auth); message(o, "Cannot open browser. Install a default browser / desktop URL handler."); }
}
void online_logout(Online *o) { online_close(o); message(o, "Logged out of ChessTTY."); }
void online_resume(Online *o) {
    if (!o->authenticated) { message(o, "Log in to Lichess first."); return; }
    o->net->listening = true;
    if (*o->game_id && !o->ended && !o->net->req[GAME].easy) o->net->retry[GAME] = 0;
    message(o, "Connecting to Lichess games…");
}
void online_seek(Online *o, int minutes, int increment, int color) {
    if (!o->authenticated) { message(o, "Log in to Lichess first."); return; }
    if (o->game_ready && !o->ended) { message(o, "Finish or resume the current game first."); return; }
    if (minutes < 1 || minutes > 180 || increment < 0 || increment > 180 || minutes * 60 + increment * 40 < 480) {
        message(o, "Lichess public Board seeks require Rapid or Classical (e.g. 10+0, 15+10)."); return;
    }
    o->net->minutes = minutes; o->net->increment = increment; o->net->color = color;
    o->net->listening = o->net->seek_requested = true;
    o->seeking = true; message(o, "Finding an opponent (casual standard game)…");
}
void online_cancel_seek(Online *o) {
    if (!o->net) return;
    stop(o, SEEK); o->net->seek_requested = false; o->seeking = false;
    message(o, "Matchmaking canceled.");
}
void online_move(Online *o, Move move) {
    if (!o->net || !o->game_ready || o->ended || !o->connected || o->move_pending || o->game.pos[o->game.n].stm != o->side) {
        message(o, "Wait for your turn and a synchronized connection."); return;
    }
    char uci[8], path[128]; move_to_uci(move, uci);
    snprintf(path, sizeof path, "/api/board/game/%s/move/%s", o->game_id, uci);
    if (request(o, REQUEST, REQ_MOVE, path, "", true)) {
        o->move_pending = true; o->net->move_deadline = now_ms() + 22000;
        message(o, "Sending move…");
    } else message(o, "Network request busy or rate limited; move was not sent.");
}
void online_action(Online *o, const char *action) {
    if (!o->authenticated || !o->game_ready || o->ended) return;
    if (strcmp(action, "resign") && strcmp(action, "abort") && strcmp(action, "draw/yes")) return;
    char path[128]; snprintf(path, sizeof path, "/api/board/game/%s/%s", o->game_id, action);
    if (!request(o, REQUEST, REQ_ACTION, path, "", true)) message(o, "Network request busy or rate limited.");
}
static void event(Online *o, const char *text) {
    Json j; if (!json_parse(&j, text)) return;
    if (!json_is(&j, member(&j, 0, "type"), "gameStart")) return;
    int game = member(&j, 0, "game");
    char id[24];
    if (!string(&j, game, "gameId", id, sizeof id)) string(&j, game, "id", id, sizeof id);
    int compat = member(&j, game, "compat");
    if (!safe_id(id) || json_is(&j, member(&j, compat, "board"), "false")) return;
    if (o->game_ready && !o->ended && strcmp(o->game_id, id)) return;
    if (strcmp(o->game_id, id)) { stop(o, GAME); o->game_ready = false; }
    snprintf(o->game_id, sizeof o->game_id, "%s", id);
    o->ended = false; o->net->retry[GAME] = 0;
    if (o->seeking) { stop(o, SEEK); o->seeking = o->net->seek_requested = false; }
    message(o, "Synchronizing game…");
}
static bool lines(Online *o, int slot) {
    Request *r = &o->net->req[slot];
    char *nl; size_t consumed = 0;
    while ((nl = memchr(r->data + consumed, '\n', r->used - consumed))) {
        *nl = 0;
        if (nl > r->data + consumed) {
            if (slot == EVENTS) event(o, r->data + consumed);
            else if (slot == GAME && !online_game_event(o, r->data + consumed, now_ms())) return false;
        }
        consumed = (size_t)(nl - r->data) + 1;
    }
    if (consumed) { r->used -= consumed; memmove(r->data, r->data + consumed, r->used); r->data[r->used] = 0; }
    return true;
}
static void completed(Online *o, int slot, CURLcode code) {
    OnlineNet *net = o->net; Request *r = &net->req[slot];
    long status = 0; curl_easy_getinfo(r->easy, CURLINFO_RESPONSE_CODE, &status);
    bool ok = code == CURLE_OK && status >= 200 && status < 300;
    if (status == 429) { net->blocked_until = now_ms() + 60000; message(o, "Lichess rate limit: retrying after one minute."); }
    else if (!ok) {
        char error[160]; snprintf(error, sizeof error, "Lichess connection error (HTTP %ld, %s).", status, curl_easy_strerror(code)); message(o, error);
    }
    if (slot == REQUEST) {
        int kind = r->kind;
        Json j; bool parsed = json_parse(&j, r->data);
        if (kind == REQ_TOKEN) {
            if (ok && parsed && string(&j, 0, "access_token", net->token, sizeof net->token) && *net->token) {
                stop(o, slot);
                if (!request(o, REQUEST, REQ_ACCOUNT, "/api/account", NULL, true)) { o->connecting = false; message(o, "Account request failed; retry login."); }
                return;
            }
            o->connecting = false;
        } else if (kind == REQ_ACCOUNT) {
            o->connecting = false;
            if (ok && parsed && string(&j, 0, "id", o->user_id, sizeof o->user_id) &&
                string(&j, 0, "username", o->user, sizeof o->user)) {
                o->authenticated = true; message(o, "Logged in. Choose Find opponent or Resume game.");
            }
        } else if (kind == REQ_MOVE) {
            if (!ok) { o->move_pending = false; stop(o, GAME); o->connected = false; net->retry[GAME] = now_ms() + 1000; }
            else message(o, "Move accepted; waiting for authoritative game state.");
        } else if (kind == REQ_ACTION && ok) message(o, "Game action accepted.");
    } else if (slot == SEEK) {
        o->seeking = net->seek_requested = false;
        if (ok && !o->game_ready) message(o, "Seek ended. Waiting for game-start event; seek again if no match appears.");
    } else {
        if (slot == GAME) o->connected = false;
        int delay = net->backoff[slot] ? net->backoff[slot] * 2 : 1000;
        if (delay > 60000) delay = 60000;
        net->backoff[slot] = delay; net->retry[slot] = now_ms() + delay;
        if (slot == GAME && !o->ended && status != 429) message(o, "Connection lost. Reconnecting; displayed clocks are estimates.");
    }
    if (status == 401 || status == 403) {
        o->authenticated = false; o->connecting = false;
        net->listening = net->seek_requested = o->seeking = false;
        message(o, "Lichess authorization expired or permission denied. Log in again.");
    }
    stop(o, slot);
}
bool online_poll(Online *o) {
    browser_reap();
    if (!o->net) return false;
    OnlineNet *net = o->net; int64_t now = now_ms();
    if (net->auth.active) {
        char code[2048]; int result = oauth_poll(&net->auth, code, sizeof code, now);
        if (result > 0) {
            CURL *escape = curl_easy_init();
            char *redirect = escape ? curl_easy_escape(escape, net->auth.redirect, 0) : NULL;
            char form[2560];
            if (redirect) {
                snprintf(form, sizeof form, "grant_type=authorization_code&code=%s&code_verifier=%s&redirect_uri=%s&client_id=chesstty", code, net->auth.verifier, redirect);
                if (!request(o, REQUEST, REQ_TOKEN, "/api/token", form, false)) { o->connecting = false; message(o, "Token exchange could not start; retry login."); }
                curl_free(redirect);
            } else o->connecting = false;
            if (escape) curl_easy_cleanup(escape);
            oauth_close(&net->auth); o->login_url[0] = 0;
        } else if (result < 0) { o->connecting = false; o->login_url[0] = 0; message(o, "Login canceled or timed out."); }
    }
    int running;
    curl_multi_perform(net->multi, &running);
    for (int slot = EVENTS; slot <= SEEK; slot++) {
        Request *r = &net->req[slot];
        if (!r->easy) continue;
        long status = 0; curl_easy_getinfo(r->easy, CURLINFO_RESPONSE_CODE, &status);
        if (status == 200) {
            if (slot == SEEK) r->used = 0;
            else if (!lines(o, slot)) {
                stop(o, slot); o->connected = false; net->retry[slot] = now + 5000;
                message(o, "Invalid or unsupported game snapshot; reconnecting.");
            }
            if (o->connected && slot == GAME) net->backoff[GAME] = 0;
        }
    }
    CURLMsg *msg; int queued;
    while ((msg = curl_multi_info_read(net->multi, &queued))) {
        if (msg->msg != CURLMSG_DONE) continue;
        for (int slot = 0; slot < CHANNELS; slot++)
            if (net->req[slot].easy == msg->easy_handle) { completed(o, slot, msg->data.result); break; }
    }
    for (int slot = EVENTS; slot < CHANNELS; slot++) {
        Request *r = &net->req[slot];
        if (r->easy && (!o->authenticated || now - r->last_activity > 45000)) {
            stop(o, slot); net->retry[slot] = now + 2000;
            if (slot == GAME) { o->connected = false; message(o, "Game stream timed out; reconnecting."); }
            if (slot == SEEK) { o->seeking = net->seek_requested = false; message(o, "Matchmaking connection lost. Start a new seek."); }
        }
    }
    if (o->authenticated && now >= net->blocked_until) {
        if (net->listening && !net->req[EVENTS].easy && now >= net->retry[EVENTS])
            request(o, EVENTS, REQ_NONE, "/api/stream/event", NULL, true);
        if (*o->game_id && !o->ended && !net->req[GAME].easy && now >= net->retry[GAME]) {
            char path[128]; snprintf(path, sizeof path, "/api/board/game/stream/%s", o->game_id);
            request(o, GAME, REQ_NONE, path, NULL, true);
        }
        long status = 0;
        if (net->req[EVENTS].easy) curl_easy_getinfo(net->req[EVENTS].easy, CURLINFO_RESPONSE_CODE, &status);
        if (net->seek_requested && status == 200 && !net->req[SEEK].easy) {
            char form[160]; snprintf(form, sizeof form, "rated=false&variant=standard&time=%d&increment=%d&color=%s", net->minutes, net->increment,
                net->color == 0 ? "white" : net->color == 1 ? "black" : "random");
            request(o, SEEK, REQ_NONE, "/api/board/seek", form, true);
        }
    }
    if (o->move_pending && now > net->move_deadline) {
        o->move_pending = false; stop(o, GAME); o->connected = false; net->retry[GAME] = now + 1000;
        message(o, "Move status uncertain. Resynchronizing before another move.");
    }
    bool changed = o->changed; o->changed = false;
    return changed;
}

/* Exercise the packaged libcurl, TLS backend, DNS and certificate trust without
   opening a browser or touching an account. Only used by the CLI self-test. */
int online_https_test(void) {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 1;
    CURL *easy = curl_easy_init();
    if (!easy) { curl_global_cleanup(); return 1; }
    curl_easy_setopt(easy, CURLOPT_URL, HOST "/robots.txt");
    curl_easy_setopt(easy, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "ChessTTY/release-test");
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
#if defined(_WIN32) && defined(CURLSSLOPT_NATIVE_CA)
    curl_easy_setopt(easy, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif
    CURLcode code = curl_easy_perform(easy);
    long status = 0; curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    const curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
    bool ok = code == CURLE_OK && status == 200;
    printf("HTTPS %s: HTTP %ld, %s (%s)\n", ok ? "passed" : "failed", status,
           curl_easy_strerror(code), info->ssl_version ? info->ssl_version : "no TLS backend");
    curl_easy_cleanup(easy); curl_global_cleanup();
    return ok ? 0 : 1;
}
