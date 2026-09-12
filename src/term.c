/* term.c — cross-platform terminal layer */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "portable.h"   /* must come first: feature-test macros */

#include "term.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool g_truecolor = false;
static bool g_inited = false;

/* ------------------------------ string builder ------------------------------ */
static bool sb_grow(SB *b, size_t need) {
    if (need > SIZE_MAX - b->len - 1) return false;
    if (b->len + need + 1 <= b->cap) return true;
    size_t nc = b->cap ? b->cap * 2 : 8192;
    while (nc < b->len + need + 1) {
        if (nc > SIZE_MAX / 2) { nc = b->len + need + 1; break; }
        nc *= 2;
    }
    char *grown = realloc(b->s, nc);
    if (!grown) return false;
    b->s = grown;
    b->cap = nc;
    return true;
}
void sb_reset(SB *b) { b->len = 0; if (b->s) b->s[0] = 0; }
void sb_putn(SB *b, const char *s, size_t n) {
    if (!sb_grow(b, n)) return;
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}
void sb_put(SB *b, const char *s) { sb_putn(b, s, strlen(s)); }
void sb_printf(SB *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) sb_putn(b, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}
void sb_free(SB *b) { free(b->s); b->s = NULL; b->len = b->cap = 0; }

/* ------------------------------ colours ------------------------------ */
static int rgb_to_256(uint32_t rgb) {
    int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    if (r == g && g == b) {
        if (r < 8) return 16;
        if (r > 248) return 231;
        return 232 + (r - 8) / 10;
    }
    int ri = r < 48 ? 0 : r < 115 ? 1 : (r - 35) / 40;
    int gi = g < 48 ? 0 : g < 115 ? 1 : (g - 35) / 40;
    int bi = b < 48 ? 0 : b < 115 ? 1 : (b - 35) / 40;
    return 16 + 36 * ri + 6 * gi + bi;
}

void t_moveto(SB *b, int row, int col) { sb_printf(b, "\x1b[%d;%dH", row, col); }

static void emit_color(SB *b, uint32_t rgb, bool is_bg) {
    if (g_truecolor)
        sb_printf(b, "\x1b[%d;2;%u;%u;%um", is_bg ? 48 : 38,
                  (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    else
        sb_printf(b, "\x1b[%d;5;%dm", is_bg ? 48 : 38, rgb_to_256(rgb));
}

void t_style(SB *b, uint32_t fg, uint32_t bg, int attrs) {
    sb_put(b, "\x1b[0m");
    if (attrs & A_BOLD) sb_put(b, "\x1b[1m");
    if (attrs & A_DIM) sb_put(b, "\x1b[2m");
    emit_color(b, fg, false);
    emit_color(b, bg, true);
}

void t_style_fg(SB *b, uint32_t fg, int attrs) {
    sb_put(b, "\x1b[0m");
    if (attrs & A_BOLD) sb_put(b, "\x1b[1m");
    if (attrs & A_DIM) sb_put(b, "\x1b[2m");
    emit_color(b, fg, false);
}

void t_reset(SB *b) { sb_put(b, "\x1b[0m"); }
void t_clear_eol(SB *b) { sb_put(b, "\x1b[K"); }
void t_clear_all(SB *b) { sb_put(b, "\x1b[2J"); }

/* ------------------------------ platform ------------------------------ */
#ifdef _WIN32
#include <conio.h>
#include <io.h>
#include <windows.h>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef DISABLE_NEWLINE_AUTO_RETURN
#define DISABLE_NEWLINE_AUTO_RETURN 0x0008
#endif

static DWORD g_saved_outmode;
static UINT g_saved_cp;
static bool g_saved_ok = false;

bool term_init(void) {
    HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (!GetConsoleMode(hout, &mode)) return false;
    g_saved_outmode = mode;
    g_saved_cp = GetConsoleOutputCP();
    g_saved_ok = true;
    if (!SetConsoleMode(hout, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                                  DISABLE_NEWLINE_AUTO_RETURN)) {
        /* DISABLE_NEWLINE_AUTO_RETURN may be unsupported: retry without it */
        if (!SetConsoleMode(hout, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
            return false;
    }
    SetConsoleOutputCP(CP_UTF8);
    g_truecolor = true; /* Windows 10+ with VT supports 24-bit colour */
    fputs("\x1b[?1049h\x1b[?25l", stdout);
    fflush(stdout);
    g_inited = true;
    return true;
}

void term_restore(void) {
    if (!g_inited) return;
    fputs("\x1b[0m\x1b[?25h\x1b[?1049l", stdout);
    fflush(stdout);
    if (g_saved_ok) {
        SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), g_saved_outmode);
        SetConsoleOutputCP(g_saved_cp);
    }
    g_inited = false;
}

void term_size(int *w, int *h) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        *w = info.srWindow.Right - info.srWindow.Left + 1;
        *h = info.srWindow.Bottom - info.srWindow.Top + 1;
    } else { *w = 80; *h = 24; }
}

Key term_read_key(void) {
    Key k = { K_NONE, 0 };
    if (!_kbhit()) return k;
    int c = _getch();
    if (c == 0 || c == 0xE0) {
        int c2 = _kbhit() ? _getch() : 0;
        switch (c2) {
            case 72: k.type = K_UP; break;
            case 80: k.type = K_DOWN; break;
            case 75: k.type = K_LEFT; break;
            case 77: k.type = K_RIGHT; break;
            case 71: k.type = K_HOME; break;
            case 79: k.type = K_END; break;
            case 73: k.type = K_PGUP; break;
            case 81: k.type = K_PGDN; break;
            case 83: k.type = K_DEL; break;
            default: break;
        }
        return k;
    }
    if (c == '\r' || c == '\n') { k.type = K_ENTER; return k; }
    if (c == 8) { k.type = K_BACKSPACE; return k; }
    if (c == 27) { k.type = K_ESC; return k; }
    if (c == '\t') { k.type = K_TAB; return k; }
    if (c >= 1 && c <= 26) { k.type = K_CTRL; k.ch = (char)('a' + c - 1); return k; }
    if (c >= 32 && c < 127) { k.type = K_CHAR; k.ch = (char)c; return k; }
    return k;
}

void term_frame_flush(SB *b, int cur_row, int cur_col) {
    if (cur_row > 0) {
        t_moveto(b, cur_row, cur_col);
        sb_put(b, "\x1b[?25h");
    } else sb_put(b, "\x1b[?25l");
    DWORD written;
    WriteConsoleA(GetStdHandle(STD_OUTPUT_HANDLE), b->s, (DWORD)b->len, &written, NULL);
}

#else
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

static struct termios g_saved_tio;
static bool g_saved_ok = false;

bool term_init(void) {
    if (!isatty(0) || !isatty(1)) return false;
    struct termios tio;
    if (tcgetattr(0, &tio) != 0) return false;
    g_saved_tio = tio;
    g_saved_ok = true;
    tio.c_lflag &= ~(ICANON | ECHO);
    tio.c_iflag &= ~(IXON | ICRNL);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &tio) != 0) return false;
    const char *ct = getenv("COLORTERM");
    g_truecolor = ct && (strstr(ct, "truecolor") || strstr(ct, "24bit"));
    if (!g_truecolor) {
        const char *tp = getenv("TERM_PROGRAM");
        if (tp && (strstr(tp, "iTerm") || strstr(tp, "vscode") || strstr(tp, "WezTerm") ||
                   strstr(tp, "ghostty") || strstr(tp, "Hyper")))
            g_truecolor = true;
    }
    fputs("\x1b[?1049h\x1b[?25l", stdout);
    fflush(stdout);
    g_inited = true;
    return true;
}

void term_restore(void) {
    if (!g_inited) return;
    fputs("\x1b[0m\x1b[?25h\x1b[?1049l", stdout);
    fflush(stdout);
    if (g_saved_ok) tcsetattr(0, TCSANOW, &g_saved_tio);
    g_inited = false;
}

void term_size(int *w, int *h) {
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        *w = ws.ws_col;
        *h = ws.ws_row;
    } else { *w = 80; *h = 24; }
}

/* input buffer for escape sequences */
static char g_ibuf[64];
static int g_ilen = 0;
static long long g_esc_time = 0;

static long long tnow_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void ibuf_shift(int n) {
    if (n >= g_ilen) { g_ilen = 0; return; }
    memmove(g_ibuf, g_ibuf + n, (size_t)(g_ilen - n));
    g_ilen -= n;
}

Key term_read_key(void) {
    Key k = { K_NONE, 0 };
    if (g_ilen < (int)sizeof g_ibuf - 1) {
        ssize_t got = read(0, g_ibuf + g_ilen, sizeof g_ibuf - 1 - (size_t)g_ilen);
        if (got > 0) g_ilen += (int)got;
    }
    if (g_ilen == 0) return k;

    unsigned char c0 = (unsigned char)g_ibuf[0];
    if (c0 != 0x1b) {
        ibuf_shift(1);
        if (c0 == '\r' || c0 == '\n') { k.type = K_ENTER; return k; }
        if (c0 == 127 || c0 == 8) { k.type = K_BACKSPACE; return k; }
        if (c0 == '\t') { k.type = K_TAB; return k; }
        if (c0 >= 1 && c0 <= 26) { k.type = K_CTRL; k.ch = (char)('a' + c0 - 1); return k; }
        if (c0 >= 32 && c0 < 127) { k.type = K_CHAR; k.ch = (char)c0; return k; }
        return k; /* UTF-8 or control byte: ignore */
    }
    /* ESC: wait a moment to see whether a sequence follows */
    if (g_ilen == 1) {
        if (g_esc_time == 0) { g_esc_time = tnow_ms(); return k; }
        if (tnow_ms() - g_esc_time < 30) return k;
        g_esc_time = 0;
        ibuf_shift(1);
        k.type = K_ESC;
        return k;
    }
    g_esc_time = 0;
    char c1 = g_ibuf[1];
    if (c1 != '[' && c1 != 'O') {
        /* ESC + any other character: treat as ESC */
        ibuf_shift(1);
        k.type = K_ESC;
        return k;
    }
    /* CSI: find the final byte (@..~) */
    int end = -1;
    for (int i = 2; i < g_ilen; i++) {
        unsigned char c = (unsigned char)g_ibuf[i];
        if (c >= 0x40 && c <= 0x7E) { end = i; break; }
    }
    if (end < 0) {
        if (g_ilen >= (int)sizeof g_ibuf - 2) g_ilen = 0; /* corrupt sequence */
        return k;
    }
    char fin = g_ibuf[end];
    char num = end > 2 ? g_ibuf[2] : 0;
    ibuf_shift(end + 1);
    switch (fin) {
        case 'A': k.type = K_UP; break;
        case 'B': k.type = K_DOWN; break;
        case 'C': k.type = K_RIGHT; break;
        case 'D': k.type = K_LEFT; break;
        case 'H': k.type = K_HOME; break;
        case 'F': k.type = K_END; break;
        case '~':
            switch (num) {
                case '1': case '7': k.type = K_HOME; break;
                case '4': case '8': k.type = K_END; break;
                case '3': k.type = K_DEL; break;
                case '5': k.type = K_PGUP; break;
                case '6': k.type = K_PGDN; break;
                default: break;
            }
            break;
        default: break;
    }
    return k;
}

void term_frame_flush(SB *b, int cur_row, int cur_col) {
    if (cur_row > 0) {
        t_moveto(b, cur_row, cur_col);
        sb_put(b, "\x1b[?25h");
    } else sb_put(b, "\x1b[?25l");
    size_t off = 0;
    while (off < b->len) {
        ssize_t w = write(1, b->s + off, b->len - off);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        off += (size_t)w;
    }
}
#endif
