/* chess.c — chess core */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "chess.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const int KNIGHT_D[8][2] = {{1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2}};
static const int KING_D[8][2]   = {{1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1},{0,-1},{1,-1}};
static const int BISHOP_D[4][2] = {{1,1},{-1,1},{-1,-1},{1,-1}};
static const int ROOK_D[4][2]   = {{1,0},{0,1},{-1,0},{0,-1}};

void pos_start(Pos *p) {
    pos_from_fen(p, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

bool pos_from_fen(Pos *p, const char *fen) {
    Pos t;
    memset(&t, 0, sizeof t);
    t.ep = -1;
    const char *s = fen;
    int f = 0, r = 7;
    while (*s && *s != ' ') {
        char c = *s++;
        if (c == '/') { r--; f = 0; if (r < 0) return false; continue; }
        if (c >= '1' && c <= '8') { f += c - '0'; if (f > 8) return false; continue; }
        if (f > 7) return false;
        int8_t pc;
        switch (tolower((unsigned char)c)) {
            case 'p': pc = PAWN; break;   case 'n': pc = KNIGHT; break;
            case 'b': pc = BISHOP; break; case 'r': pc = ROOK; break;
            case 'q': pc = QUEEN; break;  case 'k': pc = KING; break;
            default: return false;
        }
        t.sq[SQ(f, r)] = isupper((unsigned char)c) ? pc : (int8_t)-pc;
        f++;
    }
    while (*s == ' ') s++;
    if (*s == 'w') t.stm = 1; else if (*s == 'b') t.stm = -1; else return false;
    s++;
    while (*s == ' ') s++;
    if (*s == '-') { s++; }
    else {
        while (*s && *s != ' ') {
            switch (*s) {
                case 'K': t.castle |= CR_WK; break; case 'Q': t.castle |= CR_WQ; break;
                case 'k': t.castle |= CR_BK; break; case 'q': t.castle |= CR_BQ; break;
                default: break; /* ignore Chess960-style letters */
            }
            s++;
        }
    }
    while (*s == ' ') s++;
    if (*s == '-') s++;
    else if (*s >= 'a' && *s <= 'h' && s[1] >= '1' && s[1] <= '8') {
        t.ep = SQ(s[0] - 'a', s[1] - '1');
        s += 2;
    }
    while (*s == ' ') s++;
    int hm = 0, fm = 1;
    if (sscanf(s, "%d %d", &hm, &fm) < 1) { hm = 0; fm = 1; }
    if (fm < 1) fm = 1;
    t.halfmove = (int16_t)hm;
    t.fullmove = (int16_t)fm;
    /* minimal sanity: both kings on the board */
    if (king_square(&t, 1) < 0 || king_square(&t, -1) < 0) return false;
    *p = t;
    return true;
}

void pos_to_fen(const Pos *p, char *buf, size_t n) {
    char tmp[160];   /* 71 board + 4 side + 4 castling + 3 ep + clocks */
    char *o = tmp;
    for (int r = 7; r >= 0; r--) {
        int run = 0;
        for (int f = 0; f < 8; f++) {
            int8_t pc = p->sq[SQ(f, r)];
            if (pc == EMPTY) { run++; continue; }
            if (run) *o++ = (char)('0' + run), run = 0;
            const char *w = " PNBRQK", *b = " pnbrqk";
            *o++ = pc > 0 ? w[pc] : b[-pc];
        }
        if (run) *o++ = (char)('0' + run);
        if (r) *o++ = '/';
    }
    *o++ = ' ';
    *o++ = p->stm > 0 ? 'w' : 'b';
    *o++ = ' ';
    if (!p->castle) *o++ = '-';
    else {
        if (p->castle & CR_WK) *o++ = 'K';
        if (p->castle & CR_WQ) *o++ = 'Q';
        if (p->castle & CR_BK) *o++ = 'k';
        if (p->castle & CR_BQ) *o++ = 'q';
    }
    *o++ = ' ';
    if (p->ep >= 0) { *o++ = (char)('a' + FILE_OF(p->ep)); *o++ = (char)('1' + RANK_OF(p->ep)); }
    else *o++ = '-';
    snprintf(o, sizeof tmp - (size_t)(o - tmp), " %d %d", p->halfmove, p->fullmove);
    snprintf(buf, n, "%s", tmp);
}

int king_square(const Pos *p, int side) {
    int8_t k = (int8_t)(KING * side);
    for (int i = 0; i < 64; i++) if (p->sq[i] == k) return i;
    return -1;
}

bool square_attacked(const Pos *p, int sq, int by) {
    int f = FILE_OF(sq), r = RANK_OF(sq);
    /* pawns */
    if (by > 0) {
        if (r > 0) {
            if (f > 0 && p->sq[sq - 9] == PAWN) return true;
            if (f < 7 && p->sq[sq - 7] == PAWN) return true;
        }
    } else {
        if (r < 7) {
            if (f > 0 && p->sq[sq + 7] == -PAWN) return true;
            if (f < 7 && p->sq[sq + 9] == -PAWN) return true;
        }
    }
    /* knights and kings */
    for (int i = 0; i < 8; i++) {
        int nf = f + KNIGHT_D[i][0], nr = r + KNIGHT_D[i][1];
        if (nf >= 0 && nf < 8 && nr >= 0 && nr < 8 && p->sq[SQ(nf, nr)] == (int8_t)(KNIGHT * by))
            return true;
        nf = f + KING_D[i][0]; nr = r + KING_D[i][1];
        if (nf >= 0 && nf < 8 && nr >= 0 && nr < 8 && p->sq[SQ(nf, nr)] == (int8_t)(KING * by))
            return true;
    }
    /* diagonals: bishop/queen */
    for (int i = 0; i < 4; i++) {
        int nf = f, nr = r;
        for (;;) {
            nf += BISHOP_D[i][0]; nr += BISHOP_D[i][1];
            if (nf < 0 || nf > 7 || nr < 0 || nr > 7) break;
            int8_t pc = p->sq[SQ(nf, nr)];
            if (pc == EMPTY) continue;
            if (pc == (int8_t)(BISHOP * by) || pc == (int8_t)(QUEEN * by)) return true;
            break;
        }
    }
    /* files and ranks: rook/queen */
    for (int i = 0; i < 4; i++) {
        int nf = f, nr = r;
        for (;;) {
            nf += ROOK_D[i][0]; nr += ROOK_D[i][1];
            if (nf < 0 || nf > 7 || nr < 0 || nr > 7) break;
            int8_t pc = p->sq[SQ(nf, nr)];
            if (pc == EMPTY) continue;
            if (pc == (int8_t)(ROOK * by) || pc == (int8_t)(QUEEN * by)) return true;
            break;
        }
    }
    return false;
}

bool in_check(const Pos *p, int side) {
    int k = king_square(p, side);
    return k >= 0 && square_attacked(p, k, -side);
}

static void add_move(Move *out, int *n, int from, int to, int promo) {
    if (*n >= MAX_MOVES) return;
    out[*n].from = (uint8_t)from;
    out[*n].to = (uint8_t)to;
    out[*n].promo = (int8_t)promo;
    (*n)++;
}

static void add_pawn_move(Move *out, int *n, int from, int to, int side) {
    int lastr = side > 0 ? 7 : 0;
    if (RANK_OF(to) == lastr) {
        add_move(out, n, from, to, QUEEN);
        add_move(out, n, from, to, ROOK);
        add_move(out, n, from, to, BISHOP);
        add_move(out, n, from, to, KNIGHT);
    } else add_move(out, n, from, to, 0);
}

static int gen_pseudo(const Pos *p, Move *out) {
    int n = 0;
    int side = p->stm;
    for (int s = 0; s < 64; s++) {
        int8_t pc = p->sq[s];
        if (pc == EMPTY || (pc > 0) != (side > 0)) continue;
        int t = pc * side; /* positive piece type */
        int f = FILE_OF(s), r = RANK_OF(s);
        if (t == PAWN) {
            int fwd = s + 8 * side;
            int startr = side > 0 ? 1 : 6;
            if (fwd >= 0 && fwd < 64 && p->sq[fwd] == EMPTY) {
                add_pawn_move(out, &n, s, fwd, side);
                int fwd2 = s + 16 * side;
                if (r == startr && p->sq[fwd2] == EMPTY)
                    add_move(out, &n, s, fwd2, 0);
            }
            for (int df = -1; df <= 1; df += 2) {
                int nf = f + df;
                if (nf < 0 || nf > 7) continue;
                int to = SQ(nf, r + side);
                if (to < 0 || to > 63) continue;
                int8_t cap = p->sq[to];
                if (cap != EMPTY && (cap > 0) != (side > 0))
                    add_pawn_move(out, &n, s, to, side);
                else if (to == p->ep && p->ep >= 0)
                    add_move(out, &n, s, to, 0);
            }
        } else if (t == KNIGHT || t == KING) {
            const int (*D)[2] = t == KNIGHT ? KNIGHT_D : KING_D;
            for (int i = 0; i < 8; i++) {
                int nf = f + D[i][0], nr = r + D[i][1];
                if (nf < 0 || nf > 7 || nr < 0 || nr > 7) continue;
                int8_t cap = p->sq[SQ(nf, nr)];
                if (cap == EMPTY || (cap > 0) != (side > 0))
                    add_move(out, &n, s, SQ(nf, nr), 0);
            }
        } else {
            const int (*D)[2] = t == BISHOP ? BISHOP_D : ROOK_D;
            int ndirs = 4;
            for (int q = 0; q < (t == QUEEN ? 2 : 1); q++) {
                if (t == QUEEN) D = q == 0 ? BISHOP_D : ROOK_D;
                for (int i = 0; i < ndirs; i++) {
                    int nf = f, nr = r;
                    for (;;) {
                        nf += D[i][0]; nr += D[i][1];
                        if (nf < 0 || nf > 7 || nr < 0 || nr > 7) break;
                        int8_t cap = p->sq[SQ(nf, nr)];
                        if (cap == EMPTY) { add_move(out, &n, s, SQ(nf, nr), 0); continue; }
                        if ((cap > 0) != (side > 0)) add_move(out, &n, s, SQ(nf, nr), 0);
                        break;
                    }
                }
            }
        }
    }
    /* castling */
    int base = side > 0 ? 0 : 7;
    int e = SQ(4, base);
    if (p->sq[e] == (int8_t)(KING * side) && !square_attacked(p, e, -side)) {
        int kf = side > 0 ? (p->castle & CR_WK) : (p->castle & CR_BK);
        int qf = side > 0 ? (p->castle & CR_WQ) : (p->castle & CR_BQ);
        if (kf && p->sq[SQ(7, base)] == (int8_t)(ROOK * side) &&
            p->sq[SQ(5, base)] == EMPTY && p->sq[SQ(6, base)] == EMPTY &&
            !square_attacked(p, SQ(5, base), -side) && !square_attacked(p, SQ(6, base), -side))
            add_move(out, &n, e, SQ(6, base), 0);
        if (qf && p->sq[SQ(0, base)] == (int8_t)(ROOK * side) &&
            p->sq[SQ(1, base)] == EMPTY && p->sq[SQ(2, base)] == EMPTY && p->sq[SQ(3, base)] == EMPTY &&
            !square_attacked(p, SQ(3, base), -side) && !square_attacked(p, SQ(2, base), -side))
            add_move(out, &n, e, SQ(2, base), 0);
    }
    return n;
}

int gen_legal(const Pos *p, Move *out) {
    Move tmp[MAX_MOVES];
    int n = gen_pseudo(p, tmp);
    int m = 0;
    for (int i = 0; i < n; i++) {
        Pos t = *p;
        make_move(&t, tmp[i]);
        if (!in_check(&t, p->stm)) out[m++] = tmp[i];
    }
    return m;
}

bool move_is_capture(const Pos *p, Move m) {
    if (p->sq[m.to] != EMPTY) return true;
    int8_t pc = p->sq[m.from];
    return (pc == PAWN || pc == -PAWN) && m.to == p->ep && p->ep >= 0;
}

void make_move(Pos *p, Move m) {
    int8_t pc = p->sq[m.from];
    int side = pc > 0 ? 1 : -1;
    bool is_pawn = pc == (int8_t)(PAWN * side);
    bool is_cap = p->sq[m.to] != EMPTY;

    if (is_pawn && m.to == p->ep && p->ep >= 0 && !is_cap) {
        p->sq[m.to - 8 * side] = EMPTY;
        is_cap = true;
    }
    p->sq[m.to] = m.promo ? (int8_t)(m.promo * side) : pc;
    p->sq[m.from] = EMPTY;

    if (pc == (int8_t)(KING * side) && FILE_OF(m.from) == 4) {
        int r = RANK_OF(m.from);
        if (FILE_OF(m.to) == 6) { p->sq[SQ(5, r)] = p->sq[SQ(7, r)]; p->sq[SQ(7, r)] = EMPTY; }
        else if (FILE_OF(m.to) == 2) { p->sq[SQ(3, r)] = p->sq[SQ(0, r)]; p->sq[SQ(0, r)] = EMPTY; }
    }
    if (pc == (int8_t)(KING * side))
        p->castle &= side > 0 ? (uint8_t)~(CR_WK | CR_WQ) : (uint8_t)~(CR_BK | CR_BQ);
    if (m.from == SQ(7, 0) || m.to == SQ(7, 0)) p->castle &= (uint8_t)~CR_WK;
    if (m.from == SQ(0, 0) || m.to == SQ(0, 0)) p->castle &= (uint8_t)~CR_WQ;
    if (m.from == SQ(7, 7) || m.to == SQ(7, 7)) p->castle &= (uint8_t)~CR_BK;
    if (m.from == SQ(0, 7) || m.to == SQ(0, 7)) p->castle &= (uint8_t)~CR_BQ;

    p->ep = -1;
    if (is_pawn && m.to - m.from == 16 * side) p->ep = (int8_t)(m.from + 8 * side);

    if (is_pawn || is_cap) p->halfmove = 0; else p->halfmove++;
    if (side < 0) p->fullmove++;
    p->stm = (int8_t)-side;
}

void move_to_uci(Move m, char *out) {
    out[0] = (char)('a' + FILE_OF(m.from));
    out[1] = (char)('1' + RANK_OF(m.from));
    out[2] = (char)('a' + FILE_OF(m.to));
    out[3] = (char)('1' + RANK_OF(m.to));
    if (m.promo) { out[4] = "??nbrq"[m.promo]; out[5] = 0; }
    else out[4] = 0;
}

bool uci_to_move(const Pos *p, const char *s, Move *out) {
    size_t len = strlen(s);
    if (len < 4 || len > 5) return false;
    if (s[0] < 'a' || s[0] > 'h' || s[1] < '1' || s[1] > '8' ||
        s[2] < 'a' || s[2] > 'h' || s[3] < '1' || s[3] > '8') return false;
    int from = SQ(s[0] - 'a', s[1] - '1'), to = SQ(s[2] - 'a', s[3] - '1');
    int promo = 0;
    if (len == 5) {
        switch (tolower((unsigned char)s[4])) {
            case 'n': promo = KNIGHT; break; case 'b': promo = BISHOP; break;
            case 'r': promo = ROOK; break;   case 'q': promo = QUEEN; break;
            default: return false;
        }
    }
    Move list[MAX_MOVES];
    int n = gen_legal(p, list);
    for (int i = 0; i < n; i++)
        if (list[i].from == from && list[i].to == to && list[i].promo == promo) {
            *out = list[i];
            return true;
        }
    return false;
}

void move_to_san(const Pos *p, Move m, char *out) {
    char *o = out;
    int side = p->stm;
    int8_t pc = p->sq[m.from];
    int t = pc * side;
    bool cap = move_is_capture(p, m);

    if (t == KING && FILE_OF(m.from) == 4 && (FILE_OF(m.to) == 6 || FILE_OF(m.to) == 2)) {
        strcpy(out, FILE_OF(m.to) == 6 ? "O-O" : "O-O-O");
        o = out + strlen(out);
    } else if (t == PAWN) {
        if (cap) { *o++ = (char)('a' + FILE_OF(m.from)); *o++ = 'x'; }
        *o++ = (char)('a' + FILE_OF(m.to));
        *o++ = (char)('1' + RANK_OF(m.to));
        if (m.promo) { *o++ = '='; *o++ = "??NBRQ"[m.promo]; }
    } else {
        *o++ = "??NBRQK"[t];
        Move list[MAX_MOVES];
        int n = gen_legal(p, list);
        bool amb = false, same_file = false, same_rank = false;
        for (int i = 0; i < n; i++) {
            if (list[i].to == m.to && list[i].from != m.from && p->sq[list[i].from] == pc) {
                amb = true;
                if (FILE_OF(list[i].from) == FILE_OF(m.from)) same_file = true;
                if (RANK_OF(list[i].from) == RANK_OF(m.from)) same_rank = true;
            }
        }
        if (amb) {
            if (!same_file) *o++ = (char)('a' + FILE_OF(m.from));
            else if (!same_rank) *o++ = (char)('1' + RANK_OF(m.from));
            else { *o++ = (char)('a' + FILE_OF(m.from)); *o++ = (char)('1' + RANK_OF(m.from)); }
        }
        if (cap) *o++ = 'x';
        *o++ = (char)('a' + FILE_OF(m.to));
        *o++ = (char)('1' + RANK_OF(m.to));
    }
    Pos after = *p;
    make_move(&after, m);
    if (in_check(&after, -side)) {
        Move l2[MAX_MOVES];
        *o++ = gen_legal(&after, l2) == 0 ? '#' : '+';
    }
    *o = 0;
}

bool san_to_move(const Pos *p, const char *san, Move *out) {
    char s[32];
    size_t n = 0;
    for (const char *c = san; *c && n < sizeof s - 1; c++)
        if (!isspace((unsigned char)*c)) s[n++] = *c;
    s[n] = 0;
    /* strip trailing +, #, !, ? annotations */
    while (n > 0 && (s[n-1] == '+' || s[n-1] == '#' || s[n-1] == '!' || s[n-1] == '?'))
        s[--n] = 0;
    if (n == 0) return false;

    Move list[MAX_MOVES];
    int nl = gen_legal(p, list);

    /* castling (O-O / 0-0 / o-o) */
    char norm[16];
    size_t j = 0;
    for (size_t i = 0; i < n && j < sizeof norm - 1; i++) {
        char c = s[i];
        if (c == '0' || c == 'o') c = 'O';
        norm[j++] = c;
    }
    norm[j] = 0;
    if (!strcmp(norm, "O-O") || !strcmp(norm, "O-O-O")) {
        int wantf = !strcmp(norm, "O-O") ? 6 : 2;
        for (int i = 0; i < nl; i++) {
            int8_t pc = p->sq[list[i].from];
            if ((pc == KING || pc == -KING) && FILE_OF(list[i].from) == 4 && FILE_OF(list[i].to) == wantf) {
                *out = list[i];
                return true;
            }
        }
        return false;
    }

    /* try UCI notation first (e2e4, e7e8q) */
    if (uci_to_move(p, s, out)) return true;

    int promo = 0;
    if (n >= 2 && s[n-2] == '=') {
        switch (s[n-1]) {
            case 'N': promo = KNIGHT; break; case 'B': promo = BISHOP; break;
            case 'R': promo = ROOK; break;   case 'Q': promo = QUEEN; break;
            default: return false;
        }
        n -= 2; s[n] = 0;
    } else if (n >= 3 && strchr("NBRQ", s[n-1]) && s[n-2] >= '1' && s[n-2] <= '8' && s[0] >= 'a' && s[0] <= 'h') {
        /* "e8Q" form without '=' */
        switch (s[n-1]) {
            case 'N': promo = KNIGHT; break; case 'B': promo = BISHOP; break;
            case 'R': promo = ROOK; break;   case 'Q': promo = QUEEN; break;
        }
        n -= 1; s[n] = 0;
    }
    if (n < 2) return false;
    if (s[n-2] < 'a' || s[n-2] > 'h' || s[n-1] < '1' || s[n-1] > '8') return false;
    int to = SQ(s[n-2] - 'a', s[n-1] - '1');
    n -= 2; s[n] = 0;

    int type = PAWN;
    size_t i0 = 0;
    if (n > 0 && strchr("KQRBN", s[0])) {
        switch (s[0]) {
            case 'K': type = KING; break;  case 'Q': type = QUEEN; break;
            case 'R': type = ROOK; break;  case 'B': type = BISHOP; break;
            case 'N': type = KNIGHT; break;
        }
        i0 = 1;
    }
    int from_file = -1, from_rank = -1;
    for (size_t i = i0; i < n; i++) {
        char c = s[i];
        if (c == 'x' || c == '-') continue;
        else if (c >= 'a' && c <= 'h') from_file = c - 'a';
        else if (c >= '1' && c <= '8') from_rank = c - '1';
        else return false;
    }

    Move found = { 0, 0, 0 };
    int count = 0;
    for (int i = 0; i < nl; i++) {
        Move m = list[i];
        int8_t pc = p->sq[m.from];
        if (pc * p->stm != type) continue;
        if (m.to != to) continue;
        if ((int)m.promo != promo) continue;
        if (from_file >= 0 && FILE_OF(m.from) != from_file) continue;
        if (from_rank >= 0 && RANK_OF(m.from) != from_rank) continue;
        /* a pushing pawn (no explicit source file) stays on its file */
        if (type == PAWN && from_file < 0 && FILE_OF(m.from) != FILE_OF(m.to)) continue;
        found = m;
        count++;
    }
    if (count != 1) return false;
    *out = found;
    return true;
}

GameState pos_state(const Pos *p) {
    Move list[MAX_MOVES];
    if (gen_legal(p, list) == 0)
        return in_check(p, p->stm) ? GS_CHECKMATE : GS_STALEMATE;
    if (p->halfmove >= 100) return GS_DRAW_FIFTY;
    /* insufficient material */
    int knights = 0, bishops_light = 0, bishops_dark = 0;
    for (int i = 0; i < 64; i++) {
        int t = p->sq[i] > 0 ? p->sq[i] : -p->sq[i];
        if (t == PAWN || t == ROOK || t == QUEEN) return GS_ONGOING;
        if (t == KNIGHT) knights++;
        if (t == BISHOP) {
            if ((FILE_OF(i) + RANK_OF(i)) & 1) bishops_light++; else bishops_dark++;
        }
    }
    int bishops = bishops_light + bishops_dark;
    if (knights + bishops <= 1) return GS_DRAW_MATERIAL;         /* KK, KNK, KBK */
    if (knights == 0 && (bishops_light == 0 || bishops_dark == 0))
        return GS_DRAW_MATERIAL;                                 /* bishops all on one colour */
    return GS_ONGOING;
}

void pos_key(const Pos *p, PosKey *out) {
    for (int i = 0; i < 32; i++)
        out->k[i] = (uint8_t)(((p->sq[2 * i] & 0xF) << 4) | (p->sq[2 * i + 1] & 0xF));
    out->k[32] = (uint8_t)(((p->stm > 0 ? 1 : 0) << 4) | p->castle);
    /* the ep square only counts if an en-passant capture is actually possible */
    uint8_t ep = 0xFF;
    if (p->ep >= 0) {
        int r = RANK_OF(p->ep) - p->stm;
        if (r >= 0 && r <= 7) {
            for (int df = -1; df <= 1; df += 2) {
                int f = FILE_OF(p->ep) + df;
                if (f >= 0 && f <= 7 && p->sq[SQ(f, r)] == (int8_t)(PAWN * p->stm))
                    ep = (uint8_t)p->ep;
            }
        }
    }
    out->k[33] = ep;
}

bool poskey_eq(const PosKey *a, const PosKey *b) {
    return memcmp(a->k, b->k, sizeof a->k) == 0;
}

unsigned long long perft(const Pos *p, int depth) {
    if (depth <= 0) return 1;
    Move list[MAX_MOVES];
    int n = gen_legal(p, list);
    if (depth == 1) return (unsigned long long)n;
    unsigned long long tot = 0;
    for (int i = 0; i < n; i++) {
        Pos t = *p;
        make_move(&t, list[i]);
        tot += perft(&t, depth - 1);
    }
    return tot;
}
