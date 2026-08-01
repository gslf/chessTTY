# ♞ ChessTUI

![ChessTUI Screenshot](res/ss.png)

Chess in terminal terminal, written in C, with **Stockfish built in**: the engine is
downloaded and compiled automatically by the build.

- Play against Stockfish, picking a **difficulty from 1 to 100** with a slider
- Open a **PGN** and analyze the game move by move
- **Live analysis**: the 3 best moves computed by a dedicated, full-strength
  Stockfish instance
- Board drawn with UTF-8 characters, truecolor palette (256-color fallback).
  Four board sizes with auto-fit, on large boards the pieces become
  **multi-cell block-art sprites** that fill the squares

## Download

Ready-to-run archives are on the
[Releases](https://github.com/gslf/ChessTUI/releases) page — Linux (x86_64,
arm64), macOS (universal) and Windows (x86_64). The engine travels with the
binary, so there is nothing to install:

```bash
tar xzf chesstui-1.0.0-linux-x86_64.tar.gz
cd chesstui-1.0.0-linux-x86_64
./chesstui
```

`SHA256SUMS.txt` in the release checks the archives. On macOS the binaries are
unsigned, so Gatekeeper quarantines them: run
`xattr -dr com.apple.quarantine chesstui engine/stockfish` once in the
extracted folder.

## Building

Requirements: `make`, a C compiler (for the TUI) and a C++17 one (for
Stockfish), plus `git` and `curl` (to fetch the sources and the neural
network the first time).

```bash
make
```

The first run downloads the Stockfish 17.1 sources, builds them and puts the
binary in `engine/stockfish`. Subsequent builds are instant.

`sudo make install` puts it in `PATH` (`/usr/local/bin/chesstui` plus
`/usr/local/lib/chesstui/stockfish`; set `PREFIX=` for somewhere else,
`make uninstall` to remove it). `make dist` builds the release archive.

Already have your own Stockfish? `./chesstui --engine /path/to/stockfish`
or set the `CHESSTUI_ENGINE` environment variable.


## Using

```bash
./chesstui                    # main menu
./chesstui game.pgn           # jump straight into PGN analysis
```

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

- Piece letters use English SAN (K, Q, R, B, N), the PGN standard
- PGN files with several games show a picker
- Automatic draws: stalemate, insufficient material, fifty-move rule and
  threefold repetition.

Level 1–100 drives the engine's Elo (`UCI_LimitStrength`), thinking time and
search depth: from ~1300 Elo with near-instant replies up to full-strength
Stockfish (level 95+). The menu label shows the bracket (Beginner…
Grandmaster) and the approximate Elo.

The analysis panel uses a **separate** Stockfish instance always running at
full strength (MultiPV 3), so the suggestions stay reliable even when your
opponent is dialed down.


## License and attribution

ChessTUI is free software, released under the **GNU General Public License,
version 3 or later**, see [LICENSE](LICENSE).

This TUI is based on **[Stockfish](https://stockfishchess.org)** chess engine,
(see their [AUTHORS](https://github.com/official-stockfish/Stockfish/blob/master/AUTHORS)
file), licensed under the
[GPLv3](https://github.com/official-stockfish/Stockfish/blob/master/Copying.txt).
