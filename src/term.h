/* term.h — terminal: raw mode, key input, colours, frame builder */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TERM_H
#define TERM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    K_NONE = 0,
    K_CHAR,        /* ASCII character in .ch */
    K_ENTER, K_BACKSPACE, K_ESC, K_TAB,
    K_UP, K_DOWN, K_LEFT, K_RIGHT,
    K_HOME, K_END, K_PGUP, K_PGDN, K_DEL,
    K_RESIZE,
} KeyType;

typedef struct {
    KeyType type;
    char ch;
} Key;

bool term_init(void);          /* raw mode + alternate screen */
void term_restore(void);
void term_size(int *w, int *h);
Key  term_read_key(void);      /* non-blocking: K_NONE when idle */

/* ---- frame builder ---- */
typedef struct {
    char *s;
    size_t len, cap;
} SB;

void sb_reset(SB *b);
void sb_put(SB *b, const char *s);
void sb_putn(SB *b, const char *s, size_t n);
void sb_printf(SB *b, const char *fmt, ...);
void sb_free(SB *b);

/* colours as 0xRRGGBB; truecolor when available, xterm-256 otherwise */
enum { A_BOLD = 1, A_DIM = 2 };
void t_moveto(SB *b, int row, int col);          /* 1-based */
void t_style(SB *b, uint32_t fg, uint32_t bg, int attrs);
void t_style_fg(SB *b, uint32_t fg, int attrs);  /* terminal default bg */
void t_reset(SB *b);
void t_clear_eol(SB *b);
void t_clear_all(SB *b);

void term_frame_flush(SB *b, int cur_row, int cur_col); /* cur_row<=0: hide cursor */

#endif
