/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Deterministic UCI peer for keyboard/UI tests. No search or external assets.
   Automated tests never launch real Stockfish. */
#include "chess.h"
#include <stdio.h>
#include <string.h>

static void position(Pos *pos, char *command) {
    char *moves = strstr(command, " moves ");
    if (moves) { *moves = 0; moves += 7; }
    if (!strcmp(command, "position startpos")) pos_start(pos);
    else if (!strncmp(command, "position fen ", 13)) {
        if (!pos_from_fen(pos, command + 13)) return;
    } else return;
    for (char *uci = moves ? strtok(moves, " ") : NULL; uci; uci = strtok(NULL, " ")) {
        Move move;
        if (!uci_to_move(pos, uci, &move)) return;
        make_move(pos, move);
    }
}

int main(void) {
    Pos pos; pos_start(&pos);
    char line[10000], best[8] = "0000";
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strcmp(line, "uci")) puts("id name ChessTTY UI fixture\nuciok");
        else if (!strcmp(line, "isready")) puts("readyok");
        else if (!strcmp(line, "quit")) break;
        else if (!strncmp(line, "position ", 9)) position(&pos, line);
        else if (!strcmp(line, "ucinewgame")) pos_start(&pos);
        else if (!strncmp(line, "go ", 3)) {
            Move legal[MAX_MOVES];
            if (gen_legal(&pos, legal)) move_to_uci(legal[0], best);
            else strcpy(best, "0000");
            printf("info depth 1 multipv 1 score cp 0 pv %s\n", best);
            if (!strstr(line, "infinite")) printf("bestmove %s\n", best);
        } else if (!strcmp(line, "stop")) printf("bestmove %s\n", best);
        /* Windows treats _IOLBF as full buffering. Deliver each response before
           waiting for the next command, even when stdout is a pipe. */
        if (fflush(stdout) == EOF) return 1;
    }
    return 0;
}
