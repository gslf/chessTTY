# Lichess opening names

Source: https://github.com/lichess-org/chess-openings

Pinned commit: `4b8622759e7ae6f93f011cc6c83a3823401ab45e`

The five TSV files are unmodified upstream source data, released under CC0
(see COPYING.txt). Names are English, as published by Lichess. These are named
opening lines, not game statistics, popularity rankings or engine evaluations.

To update, replace all five TSVs from one upstream commit, update this revision,
then run `python3 tools/build_openings.py` on a POSIX host with a C compiler.
Use `--check` to verify the checked-in output without modifying it, or
`--output PATH` to generate a separate file. Importing the module does not run it.
Commit the generated `src/openings_data.inc` as well. Normal builds are offline
and do not run Python. Run `./chesstty --openings-test` after rebuilding.

The generated data contains an 8-byte-per-node move trie, a sorted index of full
34-byte position keys (no hash collisions), interned names and string offsets.
Positions use the chess core's key semantics, excluding move clocks. Classification
uses the most recent named position, including transpositions and custom FENs;
it preserves the name after leaving the book. Explorer branches follow upstream
move sequences. If names share a position, the first source entry wins.

No heap allocation, file I/O, engine process, network access, or runtime parsing
is needed by the index. Recognition is O(log N) per move; each history entry
caches a 16-bit label for O(1) display and navigation. The full TSVs are used only
for regeneration and are not required next to the executable.
