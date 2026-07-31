/* engine.c — UCI communication */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "engine.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool engine_start(Engine *e, const char *path) {
    memset(e, 0, sizeof *e);
    if (!proc_start(&e->proc, path)) return false;
    engine_send(e, "uci");
    long long t0 = now_ms();
    char line[4096];
    while (now_ms() - t0 < 10000) {
        if (engine_poll_line(e, line, sizeof line)) {
            if (!strncmp(line, "id name ", 8))
                snprintf(e->name, sizeof e->name, "%s", line + 8);
            if (!strcmp(line, "uciok")) { e->ok = true; return true; }
        } else msleep(5);
    }
    proc_kill(&e->proc);
    return false;
}

void engine_send(Engine *e, const char *fmt, ...) {
    char buf[8192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    buf[n] = '\n';
    buf[n + 1] = 0;
    proc_write(&e->proc, buf, (size_t)n + 1);
}

bool engine_poll_line(Engine *e, char *out, size_t n) {
    for (;;) {
        /* is a complete line already buffered? */
        for (int i = 0; i < e->rlen; i++) {
            if (e->rbuf[i] == '\n') {
                int len = i;
                while (len > 0 && (e->rbuf[len - 1] == '\r')) len--;
                if ((size_t)len >= n) len = (int)n - 1;
                memcpy(out, e->rbuf, (size_t)len);
                out[len] = 0;
                memmove(e->rbuf, e->rbuf + i + 1, (size_t)(e->rlen - i - 1));
                e->rlen -= i + 1;
                return true;
            }
        }
        if (e->rlen >= (int)sizeof e->rbuf - 1) e->rlen = 0; /* absurd line: discard */
        int got = proc_read(&e->proc, e->rbuf + e->rlen, sizeof e->rbuf - 1 - (size_t)e->rlen);
        if (got <= 0) return false;
        e->rlen += got;
    }
}

bool engine_wait_for(Engine *e, const char *prefix, int timeout_ms) {
    long long t0 = now_ms();
    char line[4096];
    while (now_ms() - t0 < timeout_ms) {
        if (engine_poll_line(e, line, sizeof line)) {
            if (!strncmp(line, prefix, strlen(prefix))) return true;
        } else msleep(5);
    }
    return false;
}

void engine_quit(Engine *e) {
    if (e->proc.alive) {
        engine_send(e, "quit");
        msleep(50);
    }
    proc_kill(&e->proc);
    e->ok = false;
}

/* ---- parsing of "info ... multipv N score cp X ... pv m1 m2 ..." ---- */

static void pv_to_san(const Pos *base, const char *pv, char *out, size_t outn) {
    Pos p = *base;
    char *o = out;
    size_t left = outn;
    const char *s = pv;
    int count = 0;
    while (*s && count < 14 && left > 24) {
        while (*s == ' ') s++;
        char tok[8];
        int tl = 0;
        while (*s && *s != ' ' && tl < 7) tok[tl++] = *s++;
        tok[tl] = 0;
        if (tl < 4) break;
        Move m;
        if (!uci_to_move(&p, tok, &m)) break;
        char san[16];
        move_to_san(&p, m, san);
        int w;
        if (p.stm > 0)
            w = snprintf(o, left, "%s%d.%s", count ? " " : "", p.fullmove, san);
        else if (count == 0)
            w = snprintf(o, left, "%d\xE2\x80\xA6%s", p.fullmove, san); /* "12…e5" */
        else
            w = snprintf(o, left, " %s", san);
        if (w < 0 || (size_t)w >= left) break;
        o += w;
        left -= (size_t)w;
        make_move(&p, m);
        count++;
    }
    *o = 0;
}

bool engine_parse_info(const char *line, const Pos *base, EngineLine lines[3]) {
    if (strncmp(line, "info ", 5)) return false;
    if (!strstr(line, " pv ")) return false;
    if (strstr(line, " string ")) return false;

    int multipv = 1, depth = 0, score = 0;
    bool mate = false, has_score = false;
    const char *pv = NULL;

    const char *s = line + 5;
    while (*s) {
        while (*s == ' ') s++;
        if (!strncmp(s, "multipv ", 8)) { multipv = atoi(s + 8); s += 8; }
        else if (!strncmp(s, "depth ", 6)) { depth = atoi(s + 6); s += 6; }
        else if (!strncmp(s, "score cp ", 9)) { score = atoi(s + 9); has_score = true; s += 9; }
        else if (!strncmp(s, "score mate ", 11)) { score = atoi(s + 11); mate = true; has_score = true; s += 11; }
        else if (!strncmp(s, "pv ", 3)) { pv = s + 3; break; }
        while (*s && *s != ' ') s++;
    }
    if (!pv || !has_score || multipv < 1 || multipv > 3) return false;

    EngineLine *L = &lines[multipv - 1];
    L->depth = depth;
    L->mate = mate;
    /* UCI reports the score from the side to move: normalise to White */
    L->score = base->stm > 0 ? score : -score;
    pv_to_san(base, pv, L->pv_san, sizeof L->pv_san);
    L->first_uci[0] = 0;
    sscanf(pv, "%7s", L->first_uci);
    L->valid = true;
    return true;
}
