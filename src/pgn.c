/* pgn.c — PGN reading and writing */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pgn.h"
#include "clocks.h"
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

/* True for a character that can open a move token (pawn file, piece, castling).
   Used only by the indexing pass, which counts plies without a board. */
static bool tok_is_move_start(char c) {
    return (c >= 'a' && c <= 'h') || strchr("KQRBNO", c) != NULL;
}

PgnList *pgn_scan_mem(char *buf, size_t len, char *err, size_t errn) {
    PgnList *L = calloc(1, sizeof *L);
    if (!L) { free(buf); set_err(err, errn, "Out of memory"); return NULL; }
    L->buf = buf;
    L->len = len;
    int cap = 16;
    L->games = malloc((size_t)cap * sizeof *L->games);
    if (!L->games) { pgn_list_free(L); set_err(err, errn, "Out of memory"); return NULL; }
    L->n = 0;

    const char *s = buf, *end = buf + len;
    /* skip a possible BOM */
    if (len >= 3 && !memcmp(s, "\xEF\xBB\xBF", 3)) s += 3;

    while (s < end) {
        while (s < end && isspace((unsigned char)*s)) s++;
        if (s >= end) break;
        PgnRef ref;
        memset(&ref, 0, sizeof ref);
        strcpy(ref.result, "*");
        ref.rec_start = s - buf;
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
            else if (!strcmp(key, "Site")) field_set(ref.site, sizeof ref.site, val);
            else if (!strcmp(key, "Round")) field_set(ref.round, sizeof ref.round, val);
            else if (!strcmp(key, "Date")) field_set(ref.date, sizeof ref.date, val);
            else if (!strcmp(key, "Result")) field_set(ref.result, sizeof ref.result, val);
            else if (!strcmp(key, "TimeControl")) field_set(ref.timecontrol, sizeof ref.timecontrol, val);
            else if (!strcmp(key, "ECO")) field_set(ref.eco, sizeof ref.eco, val);
            else if (!strcmp(key, "FEN")) field_set(ref.fen, sizeof ref.fen, val);
            else if (!strcmp(key, "WhiteElo")) ref.white_elo = atoi(val);
            else if (!strcmp(key, "BlackElo")) ref.black_elo = atoi(val);
            s = nx;
            while (s < end && isspace((unsigned char)*s)) s++;
        }
        /* movetext: up to the next line that starts with '[' (outside comments).
           The same pass counts plies, so building a database index never needs
           a second walk over the file. */
        ref.mt_start = s - buf;
        bool in_brace = false, at_bol = true, at_tok = true;
        int depth = 0;
        const char *mt_end = end;
        while (s < end) {
            char c = *s;
            if (in_brace) {
                if (c == '}') in_brace = false;
            } else {
                if (c == '{') in_brace = true;
                else if (c == '(') depth++;
                else if (c == ')') { if (depth > 0) depth--; }
                else if (c == ';') { while (s < end && *s != '\n') s++; if (s == end) break; c = '\n'; }
                else if (c == '[' && at_bol) { mt_end = s; break; }
                else if (depth == 0 && at_tok && tok_is_move_start(c)) ref.plies++;
            }
            at_bol = (c == '\n');
            at_tok = isspace((unsigned char)c) || c == '.' || c == ')' ||
                     c == '(' || c == '}';
            s++;
        }
        ref.mt_end = mt_end - buf;
        if (saw_tag || ref.mt_end > ref.mt_start) {
            if (L->n >= cap) {
                cap *= 2;
                PgnRef *grown = realloc(L->games, (size_t)cap * sizeof *L->games);
                if (!grown) { pgn_list_free(L); set_err(err, errn, "Out of memory"); return NULL; }
                L->games = grown;
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

PgnList *pgn_scan_file(const char *path, char *err, size_t errn) {
    FILE *f = fopen(path, "rb");
    if (!f) { set_err(err, errn, "Cannot open file"); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 512L * 1024 * 1024) {
        fclose(f);
        set_err(err, errn, sz <= 0 ? "Empty file" : "File too large (max 512 MB)");
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
    return pgn_scan_mem(buf, (size_t)sz, err, errn);
}

/* Appends a PGN comment to the annotation of `ply`, collapsing runs of
   whitespace so a comment wrapped over several lines stays one line. */
static void note_append(Game *g, int ply, const char *s, const char *end) {
    char tmp[NOTE_MAX];
    size_t n = 0;
    const char *old = game_note(g, ply);
    if (*old) {
        n = strlen(old);
        if (n > sizeof tmp - 2) n = sizeof tmp - 2;
        memcpy(tmp, old, n);
        tmp[n++] = ' ';
    }
    bool space = n > 0 && tmp[n - 1] == ' ';
    for (; s < end; s++) {
        if (end - s >= 6 && !memcmp(s, "[%clk ", 6)) {
            const char *close = memchr(s, ']', (size_t)(end - s));
            char stamp[48];
            int64_t ms;
            if (close && close - (s + 6) < (long)sizeof stamp) {
                size_t len = (size_t)(close - s - 6);
                memcpy(stamp, s + 6, len); stamp[len] = 0;
                if (ply > 0 && clock_parse(stamp, &ms)) {
                    g->clocks[ply][clock_index(g->pos[ply - 1].stm)] = ms;
                    s = close;
                    continue;
                }
            }
        }
        if (n >= sizeof tmp - 1) continue;
        unsigned char c = (unsigned char)*s;
        if (isspace(c)) { if (!space && n > 0) { tmp[n++] = ' '; space = true; } continue; }
        tmp[n++] = (char)c;
        space = false;
    }
    while (n > 0 && tmp[n - 1] == ' ') n--;
    tmp[n] = 0;
    if (n) game_set_note(g, ply, tmp);
}

/* Maps the `!`/`?` suffix of a SAN token to a numeric annotation glyph and
   trims it off the token. */
static int strip_suffix_nag(char *tok) {
    size_t n = strlen(tok);
    size_t k = n;
    while (k > 0 && (tok[k - 1] == '!' || tok[k - 1] == '?')) k--;
    if (k == n) return NAG_NONE;
    const char *suf = tok + k;
    int nag = NAG_NONE;
    if (!strcmp(suf, "!")) nag = NAG_GOOD;
    else if (!strcmp(suf, "?")) nag = NAG_MISTAKE;
    else if (!strcmp(suf, "!!")) nag = NAG_BRILLIANT;
    else if (!strcmp(suf, "??")) nag = NAG_BLUNDER;
    else if (!strcmp(suf, "!?")) nag = NAG_INTERESTING;
    else if (!strcmp(suf, "?!")) nag = NAG_DUBIOUS;
    tok[k] = 0;
    return nag;
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
    /* Only sudden-death seconds with an optional Fischer increment. */
    const char *tc = ref->timecontrol;
    int values[2] = {0, 0};
    bool valid_tc = *tc >= '0' && *tc <= '9';
    for (int field = 0; field < 2 && valid_tc; field++) {
        int limit = field ? 3600 : 86400;
        while (*tc >= '0' && *tc <= '9') {
            int digit = *tc++ - '0';
            if (values[field] > (limit - digit) / 10) { valid_tc = false; break; }
            values[field] = values[field] * 10 + digit;
        }
        if (!field && *tc == '+') {
            tc++;
            valid_tc = *tc >= '0' && *tc <= '9';
        } else break;
    }
    if (valid_tc && !*tc && values[0] > 0) {
        g->initial_ms = (int64_t)values[0] * 1000; g->increment_ms = (int64_t)values[1] * 1000;
        g->clocks[0][0] = g->clocks[0][1] = g->initial_ms;
    }
    snprintf(g->tag_white, sizeof g->tag_white, "%s", ref->white);
    snprintf(g->tag_black, sizeof g->tag_black, "%s", ref->black);
    if (ref->event[0]) snprintf(g->tag_event, sizeof g->tag_event, "%s", ref->event);
    if (ref->site[0]) snprintf(g->tag_site, sizeof g->tag_site, "%s", ref->site);
    if (ref->round[0]) snprintf(g->tag_round, sizeof g->tag_round, "%s", ref->round);
    if (ref->date[0]) snprintf(g->tag_date, sizeof g->tag_date, "%s", ref->date);
    snprintf(g->tag_result, sizeof g->tag_result, "%s", ref->result);
    field_set(g->tag_eco, sizeof g->tag_eco, ref->eco);
    g->tag_white_elo = ref->white_elo > 0 && ref->white_elo < 4000 ? (uint16_t)ref->white_elo : 0;
    g->tag_black_elo = ref->black_elo > 0 && ref->black_elo < 4000 ? (uint16_t)ref->black_elo : 0;

    const char *s = L->buf + ref->mt_start, *end = L->buf + ref->mt_end;
    const char *result = ref->result;
    char movetext_result[8];
    int ply = 0;
    while (s < end) {
        char c = *s;
        if (isspace((unsigned char)c)) { s++; continue; }
        if (c == '{') {
            s++;
            const char *c0 = s;
            while (s < end && *s != '}') s++;
            note_append(g, g->n, c0, s);
            if (s < end) s++;
            continue;
        }
        if (c == '(') {
            /* Recursive variations are skipped: the board holds one line. */
            int depth = 1;
            s++;
            bool brace = false;
            while (s < end && depth > 0) {
                if (brace) { if (*s == '}') brace = false; }
                else if (*s == '{') brace = true;
                else if (*s == ';') { while (s < end && *s != '\n') s++; if (s == end) break; }
                else if (*s == '(') depth++;
                else if (*s == ')') depth--;
                s++;
            }
            continue;
        }
        if (c == ';') {
            s++;
            const char *c0 = s;
            while (s < end && *s != '\n') s++;
            note_append(g, g->n, c0, s);
            continue;
        }
        if (c == '$') {
            s++;
            int nag = 0;
            while (s < end && isdigit((unsigned char)*s)) {
                if (nag <= NAG_DUBIOUS) nag = nag * 10 + (*s - '0');
                s++;
            }
            if (g->n > 0 && nag >= NAG_GOOD && nag <= NAG_DUBIOUS)
                g->nag[g->n - 1] = (uint8_t)nag;
            continue;
        }
        if (c == ')') { s++; continue; }
        /* token */
        char tok[64];
        int tl = 0;
        while (s < end && !isspace((unsigned char)*s) && *s != '{' && *s != '(' &&
               *s != ')' && *s != ';' && *s != '$' && tl < 63)
            tok[tl++] = *s++;
        tok[tl] = 0;
        if (!strcmp(tok, "1-0") || !strcmp(tok, "0-1") || !strcmp(tok, "1/2-1/2") || !strcmp(tok, "*")) {
            field_set(movetext_result, sizeof movetext_result, tok);
            result = movetext_result;
            break;
        }
        /* move numbers: "12." "12..." possibly glued as "12.e4" */
        char *t = tok;
        if (isdigit((unsigned char)*t)) {
            char *digits = t;
            while (*digits >= '0' && *digits <= '9') digits++;
            if (*digits == '.') {
                t = digits;
                while (*t == '.') t++;
                if (!*t) continue;
            }
        }
        if (!strcmp(t, "--") || !strcmp(t, "Z0")) {
            set_err(err, errn, "The PGN contains null moves: not supported");
            return false;
        }
        int nag = strip_suffix_nag(t);
        Move m;
        if (!san_to_move(&g->pos[g->n], t, &m)) {
            if (err && errn)
                snprintf(err, errn, "Invalid move in the PGN: \"%s\" (ply %d)", t, ply + 1);
            return false;
        }
        if (!game_push(g, m)) { set_err(err, errn, "Game too long"); return false; }
        if (nag) g->nag[g->n - 1] = (uint8_t)nag;
        ply++;
    }
    /* result: the rules take precedence, then the tag */
    if (g->result == RES_ONGOING) {
        if (!strcmp(result, "1-0")) { g->result = RES_WHITE; strcpy(g->reason, "Result from the PGN"); }
        else if (!strcmp(result, "0-1")) { g->result = RES_BLACK; strcpy(g->reason, "Result from the PGN"); }
        else if (!strcmp(result, "1/2-1/2")) { g->result = RES_DRAW; strcpy(g->reason, "Result from the PGN"); }
        strcpy(g->tag_result, result_str(g->result));
    }
    g->view = 0;
    return true;
}

bool pgn_load_at(const char *path, long off, long len, Game *g, char *err, size_t errn) {
    if (off < 0 || len <= 0 || len > 8L * 1024 * 1024) {
        set_err(err, errn, "Invalid game range");
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (!f) { set_err(err, errn, "Cannot open file"); return false; }
    if (fseek(f, off, SEEK_SET) != 0) {
        fclose(f);
        set_err(err, errn, "Seek error");
        return false;
    }
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); set_err(err, errn, "Out of memory"); return false; }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) { free(buf); set_err(err, errn, "Read error"); return false; }
    buf[got] = 0;
    PgnList *L = pgn_scan_mem(buf, got, err, errn);   /* takes over `buf` */
    if (!L) return false;
    bool ok = pgn_load_game(L, 0, g, err, errn);
    pgn_list_free(L);
    return ok;
}

/* ------------------------------ writing ------------------------------ */

/* PGN comments are delimited by braces, so a stray '}' would truncate the
   record; newlines are folded so one annotation stays one comment. */
static void write_comment(FILE *f, const char *s) {
    fputs("{", f);
    for (; *s; s++) {
        char c = *s;
        if (c == '}') c = ')';
        else if (c == '{') c = '(';
        else if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        fputc(c, f);
    }
    fputs("}", f);
}

static void write_tag(FILE *f, const char *key, const char *value) {
    fprintf(f, "[%s \"", key);
    for (; *value; value++) {
        unsigned char c = (unsigned char)*value;
        if (c == '"' || c == '\\') fputc('\\', f);
        fputc(c < 0x20 || c == 0x7f ? ' ' : c, f);
    }
    fputs("\"]\n", f);
}

static void pgn_write(FILE *f, const Game *g) {
    write_tag(f, "Event", g->tag_event[0] ? g->tag_event : "?");
    write_tag(f, "Site", g->tag_site[0] ? g->tag_site : "?");
    write_tag(f, "Date", g->tag_date[0] ? g->tag_date : "????.??.??");
    write_tag(f, "Round", g->tag_round[0] ? g->tag_round : "-");
    write_tag(f, "White", g->tag_white);
    write_tag(f, "Black", g->tag_black);
    write_tag(f, "Result", result_str(g->result));
    if (g->initial_ms > 0) fprintf(f, "[TimeControl \"%lld+%lld\"]\n",
        (long long)(g->initial_ms / 1000), (long long)(g->increment_ms / 1000));
    if (*g->tag_eco) write_tag(f, "ECO", g->tag_eco);
    if (g->tag_white_elo) fprintf(f, "[WhiteElo \"%u\"]\n", g->tag_white_elo);
    if (g->tag_black_elo) fprintf(f, "[BlackElo \"%u\"]\n", g->tag_black_elo);
    if (g->custom_start) {
        write_tag(f, "SetUp", "1");
        write_tag(f, "FEN", g->start_fen);
    }
    fprintf(f, "\n");

    /* An annotation on ply 0 comments the starting position, so it is written
       before the first move. */
    int col = 0;
    if (*game_note(g, 0)) {
        write_comment(f, game_note(g, 0));
        fputs("\n", f);
    }
    bool need_number = true;   /* black's move needs "12..." after a comment */
    for (int i = 0; i < g->n; i++) {
        char item[64];
        const Pos *p = &g->pos[i];
        const char *glyph = nag_glyph(g->nag[i]);
        if (p->stm > 0)
            snprintf(item, sizeof item, "%d. %s%s", p->fullmove, g->san[i], glyph);
        else if (need_number)
            snprintf(item, sizeof item, "%d... %s%s", p->fullmove, g->san[i], glyph);
        else
            snprintf(item, sizeof item, "%s%s", g->san[i], glyph);
        int il = (int)strlen(item);
        if (col + 1 + il > 78 && col > 0) { fputs("\n", f); col = 0; }
        else if (col > 0) { fputs(" ", f); col++; }
        fputs(item, f);
        col += il;
        int64_t clock_ms = g->clocks[i + 1][clock_index(p->stm)];
        if (clock_ms >= 0) {
            fprintf(f, " {[%%clk %lld:%02lld:%02lld.%03lld]}",
                    (long long)(clock_ms / 3600000), (long long)(clock_ms / 60000 % 60),
                    (long long)(clock_ms / 1000 % 60), (long long)(clock_ms % 1000));
            fputc('\n', f); col = 0;
        }
        const char *note = game_note(g, i + 1);
        need_number = clock_ms >= 0;
        if (*note) {
            if (col > 0) { fputs("\n", f); col = 0; }
            write_comment(f, note);
            fputs("\n", f);
            need_number = true;
        }
    }
    const char *res = result_str(g->result);
    if (col > 0 && col + 1 + (int)strlen(res) > 78) { fputs("\n", f); col = 0; }
    if (col > 0) fprintf(f, " %s\n", res);
    else fprintf(f, "%s\n", res);
}

bool pgn_save(const Game *g, const char *path, char *err, size_t errn) {
    FILE *f = fopen(path, "wb");
    if (!f) { set_err(err, errn, "Cannot create file"); return false; }
    pgn_write(f, g);
    bool ok = ferror(f) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok) set_err(err, errn, "Write error");
    return ok;
}

bool pgn_append(const Game *g, const char *path, long *off, long *len,
                char *err, size_t errn) {
    FILE *f = fopen(path, "ab");
    if (!f) { set_err(err, errn, "Cannot open the collection for writing"); return false; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); set_err(err, errn, "Cannot append"); return false; }
    long start = ftell(f);
    if (start < 0) { fclose(f); set_err(err, errn, "Cannot append"); return false; }
    if (start > 0) { fputs("\n", f); start++; }   /* blank line between records */
    pgn_write(f, g);
    long endpos = ftell(f);
    bool ok = ferror(f) == 0 && endpos > start;
    if (fclose(f) != 0) ok = false;
    if (!ok) { set_err(err, errn, "Write error"); return false; }
    if (off) *off = start;
    if (len) *len = endpos - start;
    return true;
}
