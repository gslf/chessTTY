/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OPENINGS_H
#define OPENINGS_H
#include "chess.h"

/* Zero is the unnamed label / absent node; node 0 is the tree root. */
uint16_t opening_lookup(const PosKey *key);
const char *opening_name(uint16_t label); /* ECO and English name */
uint16_t opening_child(uint16_t node);
uint16_t opening_next(uint16_t node);
uint16_t opening_label(uint16_t node);
Move opening_move(uint16_t node);
size_t opening_storage_bytes(void);
int openings_test(void);
#endif
