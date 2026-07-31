# ♞ ChessTUI

Chess in your terminal, written in C, with **Stockfish built in**: the engine is
downloaded and compiled automatically by the build — no separate install needed.

- Play against Stockfish, picking a **difficulty from 1 to 100** with a slider
- Open a **PGN** and analyze the game move by move
- **Live analysis**: the 3 best moves computed by a dedicated, full-strength
  Stockfish instance (toggle it with a single key)
- **Navigation**: step back and forth through all the moves at any time
- **Notation always visible**, save to **PGN** when the game ends
- Board drawn with UTF-8 characters, truecolor palette (256-color fallback);
  four board sizes with auto-fit — on large boards the pieces become
  **multi-cell block-art sprites** that fill the squares

## Building

Requirements: `make`, a C compiler (for the TUI) and a C++17 one (for
Stockfish), plus `git` and `curl` (to fetch the sources and the neural
network the first time).

```bash
make
```

The first run downloads the Stockfish 17.1 sources, builds them and puts the
binary in `engine/stockfish` (~2 minutes). Subsequent builds are instant.

```bash
./chesstui                    # main menu
./chesstui game.pgn           # jump straight into PGN analysis
```

- **macOS / Linux**: works out of the box.
- **Windows 10+**: build under [MSYS2](https://www.msys2.org)
  (`pacman -S mingw-w64-ucrt-x86_64-gcc make git`), then `make`.
  Windows Terminal is recommended for colors.

Already have your own Stockfish? `./chesstui --engine /path/to/stockfish`
or set the `CHESSTUI_ENGINE` environment variable.

## Keys

| Key | Action |
|---|---|
| `e4`, `Nf3`, `O-O`, `e2e4` + Enter | play a move (SAN or coordinates) |
| `← →` | browse the moves (`Home`/`End` first/last, space = forward) |
| `v` | show/hide Stockfish's 3 best lines |
| `+` / `-` | board size — huge 9×4, large 7×3 (sprite pieces), medium 5×2, compact 3×1 |
| `z` | take back your last move |
| `s` | save the game as PGN |
| `r` | rotate the board |
| `u` | piece style (colored / print / letters) |
| `n` | new game |
| `q` / Esc | back to the menu |
| `?` | help |

## Difficulty

Level 1–100 drives the engine's Elo (`UCI_LimitStrength`), thinking time and
search depth: from ~1300 Elo with near-instant replies up to full-strength
Stockfish (level 95+). The menu label shows the bracket (Beginner…
Grandmaster) and the approximate Elo.

The analysis panel uses a **separate** Stockfish instance always running at
full strength (MultiPV 3), so the suggestions stay reliable even when your
opponent is dialed down.

## Tests

```bash
make test        # move generator perft + engine dialogue
./chesstui --pgn-test examples/immortal-games.pgn
```

## Notes

- Piece letters use English SAN (K, Q, R, B, N), the PGN standard.
- PGN files with several games show a picker; `{}` comments and `()`
  variations are skipped on load.
- Automatic draws: stalemate, insufficient material, fifty-move rule and
  threefold repetition.
- `examples/` contains two historic games to try the analysis on.

## License and attribution

ChessTUI is free software, released under the **GNU General Public License,
version 3 or later** — see [LICENSE](LICENSE). Copyright © 2026 the ChessTUI
authors.

This program would be nothing without **[Stockfish](https://stockfishchess.org)**,
the world's strongest free chess engine — copyright © the Stockfish developers
(see their [AUTHORS](https://github.com/official-stockfish/Stockfish/blob/master/AUTHORS)
file), licensed under the
[GPLv3](https://github.com/official-stockfish/Stockfish/blob/master/Copying.txt).
Thank you to the Stockfish team for making world-class chess analysis freely
available to everyone.

How Stockfish is used here:

- ChessTUI does not embed, link or modify Stockfish. `make engine` downloads
  the **unmodified official sources** from
  [github.com/official-stockfish/Stockfish](https://github.com/official-stockfish/Stockfish)
  (release `sf_17.1`), builds them locally — the Stockfish build also fetches
  its NNUE evaluation networks — and places the binary in `engine/`.
- At runtime the TUI talks to that binary as a **separate process** over the
  standard [UCI protocol](https://backscattering.de/chess/uci/).
- If you redistribute ChessTUI **together with a compiled Stockfish binary**
  (e.g. the `engine/` folder), the GPLv3 requires you to also make the
  corresponding Stockfish source code available to your users.
