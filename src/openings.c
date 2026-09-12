/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "openings.h"
#include <string.h>

typedef struct { uint16_t child, next, move, label; } OpeningNode;
typedef struct { PosKey key; uint16_t label; } OpeningPosition;
#include "openings_data.inc"

uint16_t opening_lookup(const PosKey *key) {
    size_t lo = 0, hi = sizeof opening_positions / sizeof *opening_positions;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = memcmp(key->k, opening_positions[mid].key.k, sizeof key->k);
        if (!cmp) return opening_positions[mid].label;
        if (cmp < 0) hi = mid;
        else lo = mid + 1;
    }
    return 0;
}
const char *opening_name(uint16_t label) {
    return label < sizeof opening_offsets / sizeof *opening_offsets
        ? opening_strings + opening_offsets[label] : "";
}
uint16_t opening_child(uint16_t node) { return opening_nodes[node].child; }
uint16_t opening_next(uint16_t node) { return opening_nodes[node].next; }
uint16_t opening_label(uint16_t node) { return opening_nodes[node].label; }
Move opening_move(uint16_t node) {
    unsigned m = opening_nodes[node].move;
    return (Move){ m & 63, (m >> 6) & 63, (m >> 12) & 7 };
}
size_t opening_storage_bytes(void) {
    return sizeof opening_nodes + sizeof opening_positions +
           sizeof opening_offsets + sizeof opening_strings;
}
