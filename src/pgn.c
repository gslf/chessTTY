/* pgn.c — PGN reading and writing */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pgn.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(char *err, size_t errn, const char *msg) {
    if (err && errn) snprintf(err, errn, "%s", msg);
}

/* Copies a tag value into a fixed-size field, truncating what does not fit:
   a PGN value may legitimately be longer than the field that receives it. */
static void field_set(char *dst, size_t n, const char *src) {
    size_t k = strlen(src);
    if (k >= n) k = n - 1;
    memcpy(dst, src, k);
    dst[k] = 0;
}

void pgn_list_free(PgnList *L) {
    if (!L) return;
    free(L->buf);
    free(L->games);
    free(L);
}

/* parses `[Key "Value"]` starting at s; returns the pointer past ']' or NULL */
static const char *parse_tag(const char *s, const char *end, char *key, size_t kn,
                             char *val, size_t vn) {
    if (s >= end || *s != '[') return NULL;
    s++;
    size_t k = 0;
    while (s < end && !isspace((unsigned char)*s) && *s != ']') {
        if (k < kn - 1) key[k++] = *s;
        s++;
    }
    key[k] = 0;
    while (s < end && isspace((unsigned char)*s)) s++;
    size_t v = 0;
    if (s < end && *s == '"') {
        s++;
        while (s < end && *s != '"') {
            if (*s == '\\' && s + 1 < end && (s[1] == '"' || s[1] == '\\')) s++;
            if (v < vn - 1) val[v++] = *s;
            s++;
        }
        if (s < end) s++; /* closing quote */
    }
    val[v] = 0;
    while (s < end && *s != ']') s++;
    if (s < end) s++;
    return s;
}

PgnList *pgn_scan_file(const char *path, char *err, size_t errn) {
    FILE *f = fopen(path, "rb");
    if (!f) { set_err(err, errn, "Cannot open file"); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 64 * 1024 * 1024) {
        fclose(f);
        set_err(err, errn, sz <= 0 ? "Empty file" : "File too large (max 64 MB)");
        return NULL;
    }
    char *buf = malloc((size_t)sz + 1);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); free(buf);
        set_err(err, errn, "Read error");
        return NULL;
    }
    fclose(f);
    buf[sz] = 0;

    PgnList *L = calloc(1, sizeof *L);
    L->buf = buf;
    L->len = (size_t)sz;
    int cap = 16;
    L->games = malloc((size_t)cap * sizeof *L->games);
    L->n = 0;

    const char *s = buf, *end = buf + sz;
    /* skip a possible BOM */
    if (sz >= 3 && !memcmp(s, "\xEF\xBB\xBF", 3)) s += 3;

    while (s < end) {
        while (s < end && isspace((unsigned char)*s)) s++;
        if (s >= end) break;
        PgnRef ref;
        memset(&ref, 0, sizeof ref);
        strcpy(ref.result, "*");
        bool saw_tag = false;
        /* tag section */
        while (s < end && *s == '[') {
            char key[32], val[128];
            const char *nx = parse_tag(s, end, key, sizeof key, val, sizeof val);
            if (!nx) break;
            saw_tag = true;
            if (!strcmp(key, "White")) field_set(ref.white, sizeof ref.white, val);
            else if (!strcmp(key, "Black")) field_set(ref.black, sizeof ref.black, val);
            else if (!strcmp(key, "Event")) field_set(ref.event, sizeof ref.event, val);
            else if (!strcmp(key, "Date")) field_set(ref.date, sizeof ref.date, val);
            else if (!strcmp(key, "Result")) field_set(ref.result, sizeof ref.result, val);
            else if (!strcmp(key, "FEN")) field_set(ref.fen, sizeof ref.fen, val);
            s = nx;
            while (s < end && isspace((unsigned char)*s)) s++;
        }
        /* movetext: up to the next line that starts with '[' (outside comments) */
        ref.mt_start = s - buf;
        bool in_brace = false, at_bol = true;
        const char *mt_end = end;
        while (s < end) {
            char c = *s;
            if (in_brace) {
                if (c == '}') in_brace = false;
            } else {
                if (c == '{') in_brace = true;
                else if (c == ';') { while (s < end && *s != '\n') s++; c = '\n'; }
                else if (c == '[' && at_bol) { mt_end = s; break; }
            }
            at_bol = (c == '\n');
            s++;
        }
        ref.mt_end = mt_end - buf;
        if (saw_tag || ref.mt_end > ref.mt_start) {
            if (L->n >= cap) {
                cap *= 2;
                L->games = realloc(L->games, (size_t)cap * sizeof *L->games);
            }
            if (!ref.white[0]) strcpy(ref.white, "?");
            if (!ref.black[0]) strcpy(ref.black, "?");
            L->games[L->n++] = ref;
        }
        s = buf + ref.mt_end;
    }
    if (L->n == 0) {
        pgn_list_free(L);
        set_err(err, errn, "No games found in the file");
        return NULL;
    }
    return L;
}

bool pgn_load_game(const PgnList *L, int idx, Game *g, char *err, size_t errn) {
    if (idx < 0 || idx >= L->n) { set_err(err, errn, "Invalid game index"); return false; }
    const PgnRef *ref = &L->games[idx];
    if (ref->fen[0]) {
        Pos t;
        if (!pos_from_fen(&t, ref->fen)) {
            set_err(err, errn, "Invalid FEN tag in the PGN");
            return false;
        }
    }
    game_reset(g, ref->fen[0] ? ref->fen : NULL);
    snprintf(g->tag_white, sizeof g->tag_white, "%s", ref->white);
    snprintf(g->tag_black, sizeof g->tag_black, "%s", ref->black);
    if (ref->event[0]) snprintf(g->tag_event, sizeof g->tag_event, "%s", ref->event);
    if (ref->date[0]) snprintf(g->tag_date, sizeof g->tag_date, "%s", ref->date);
    snprintf(g->tag_result, sizeof g->tag_result, "%s", ref->result);

    const char *s = L->buf + ref->mt_start, *end = L->buf + ref->mt_end;
    int ply = 0;
    while (s < end) {
        char c = *s;
        if (isspace((unsigned char)c)) { s++; continue; }
        if (c == '{') {
            s++;
            while (s < end && *s != '}') s++;
            if (s < end) s++;
            continue;
        }
        if (c == '(') {
            int depth = 1;
            s++;
            bool brace = false;
            while (s < end && depth > 0) {
                if (brace) { if (*s == '}') brace = false; }
                else if (*s == '{') brace = true;
                else if (*s == '(') depth++;
                else if (*s == ')') depth--;
                s++;
            }
            continue;
        }
        if (c == ';') { while (s < end && *s != '\n') s++; continue; }
        if (c == '$') { s++; while (s < end && isdigit((unsigned char)*s)) s++; continue; }
        if (c == ')') { s++; continue; }
        /* token */
        char tok[64];
        int tl = 0;
        while (s < end && !isspace((unsigned char)*s) && *s != '{' && *s != '(' &&
               *s != ')' && *s != ';' && tl < 63)
            tok[tl++] = *s++;
        tok[tl] = 0;
        if (!strcmp(tok, "1-0") || !strcmp(tok, "0-1") || !strcmp(tok, "1/2-1/2") || !strcmp(tok, "*"))
            break;
        /* move numbers: "12." "12..." possibly glued as "12.e4" */
        char *t = tok;
        if (isdigit((unsigned char)*t)) {
            while (isdigit((unsigned char)*t)) t++;
            while (*t == '.') t++;
            if (!*t) continue;
        }
        if (!strcmp(t, "--") || !strcmp(t, "Z0")) {
            set_err(err, errn, "The PGN contains null moves: not supported");
            return false;
        }
        Move m;
        if (!san_to_move(&g->pos[g->n], t, &m)) {
            if (err && errn)
                snprintf(err, errn, "Invalid move in the PGN: \"%s\" (ply %d)", t, ply + 1);
            return false;
        }
        if (!game_push(g, m)) { set_err(err, errn, "Game too long"); return false; }
        ply++;
    }
    /* result: the rules take precedence, then the tag */
    if (g->result == RES_ONGOING) {
        if (!strcmp(ref->result, "1-0")) { g->result = RES_WHITE; strcpy(g->reason, "Result from the PGN"); }
        else if (!strcmp(ref->result, "0-1")) { g->result = RES_BLACK; strcpy(g->reason, "Result from the PGN"); }
        else if (!strcmp(ref->result, "1/2-1/2")) { g->result = RES_DRAW; strcpy(g->reason, "Result from the PGN"); }
        strcpy(g->tag_result, result_str(g->result));
    }
    g->view = 0;
    return true;
}

bool pgn_save(const Game *g, const char *path, char *err, size_t errn) {
    FILE *f = fopen(path, "wb");
    if (!f) { set_err(err, errn, "Cannot create file"); return false; }
    fprintf(f, "[Event \"%s\"]\n", g->tag_event[0] ? g->tag_event : "?");
    fprintf(f, "[Site \"%s\"]\n", g->tag_site[0] ? g->tag_site : "?");
    fprintf(f, "[Date \"%s\"]\n", g->tag_date[0] ? g->tag_date : "????.??.??");
    fprintf(f, "[Round \"%s\"]\n", g->tag_round[0] ? g->tag_round : "-");
    fprintf(f, "[White \"%s\"]\n", g->tag_white);
    fprintf(f, "[Black \"%s\"]\n", g->tag_black);
    fprintf(f, "[Result \"%s\"]\n", result_str(g->result));
    if (g->custom_start) {
        fprintf(f, "[SetUp \"1\"]\n");
        fprintf(f, "[FEN \"%s\"]\n", g->start_fen);
    }
    fprintf(f, "\n");

    char line[100];
    int ll = 0;
    for (int i = 0; i < g->n; i++) {
        char item[32];
        const Pos *p = &g->pos[i];
        if (p->stm > 0)
            snprintf(item, sizeof item, "%d. %s", p->fullmove, g->san[i]);
        else if (i == 0)
            snprintf(item, sizeof item, "%d... %s", p->fullmove, g->san[i]);
        else
            snprintf(item, sizeof item, "%s", g->san[i]);
        int il = (int)strlen(item);
        if (ll + 1 + il > 78 && ll > 0) {
            fprintf(f, "%s\n", line);
            ll = 0;
        }
        if (ll > 0) line[ll++] = ' ';
        memcpy(line + ll, item, (size_t)il + 1);
        ll += il;
    }
    const char *res = result_str(g->result);
    if (ll + 1 + (int)strlen(res) > 78 && ll > 0) {
        fprintf(f, "%s\n", line);
        ll = 0;
    }
    if (ll > 0) fprintf(f, "%s %s\n", line, res);
    else fprintf(f, "%s\n", res);
    fclose(f);
    return true;
}
