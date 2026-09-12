#!/usr/bin/env python3
"""POSIX end-to-end keyboard tests through a real pseudo-terminal (no packages)."""
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import shutil
import struct
import subprocess
import tempfile
import termios
import time

ROOT = Path(__file__).resolve().parents[1]
ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
PREFIX = b"\x18"
CANCEL = b"\x07"


class Terminal:
    def __init__(self, scratch, *args):
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 22, 78, 0, 0))
        self.process = subprocess.Popen(
            [str(ROOT / "chesstty"), *args], stdin=slave, stdout=slave, stderr=slave,
            cwd=ROOT, start_new_session=True,
            env={**os.environ, "TERM": "xterm-256color", "XDG_DATA_HOME": str(scratch)},
        )
        os.close(slave)
        self.frame = self.read(1.2)

    def read(self, timeout=0.22):
        data = b""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if select.select([self.master], [], [], 0.025)[0]:
                try:
                    data += os.read(self.master, 65536)
                except OSError:
                    break
        return ANSI.sub("", data.decode("utf-8", errors="replace"))

    def key(self, data):
        os.write(self.master, data)
        self.frame = self.read()
        return self.frame

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
        self.process.wait(timeout=3)
        os.close(self.master)


def database(scratch):
    shutil.copy(ROOT / "examples/immortal-games.pgn", scratch / "games.pgn")
    t = Terminal(scratch, "--db", str(scratch))
    try:
        assert "C-x commands" in t.frame and "↑↓ select" not in t.frame
        assert "sorted by file order" in t.key(b"d")  # bare d is no command
        frame = t.key(PREFIX)
        assert "Search all fields" in frame and "Sort by date" in frame
        assert "Unknown key" in t.key(b"?")
        assert "Choose a command" not in t.key(CANCEL)
        t.key(PREFIX + b"s1858")
        assert "1/2 games" in t.frame
        t.key(PREFIX)
        assert "Confirm input" in t.frame and "Search all fields" not in t.frame
        t.key(CANCEL)  # cancel the prefix, preserving the current filter
        assert "1858" in t.frame and "1/2 games" in t.frame
        t.key(PREFIX + b"c")  # apply the search through the editor keymap
        t.key(PREFIX + b"d")
        assert "sorted by date ↓" in t.frame
        t.key(PREFIX + b"d")
        assert "sorted by date ↑" in t.frame
        t.key(PREFIX + b"s" + CANCEL)
        assert "2/2 games" in t.frame
        t.key(PREFIX + b"q")
        assert "Search all fields" not in t.key(PREFIX)  # main-menu keymap
        assert "Quit ChessTTY" in t.frame
        t.key(b"q")
        t.process.wait(timeout=3)
    finally:
        t.close()


def board(scratch):
    t = Terminal(scratch)
    try:
        t.key(PREFIX + b"a")
        t.read(0.5)
        t.key(b"e4\r")
        assert "position 1/1" in t.frame
        t.key(b"r")  # removed bare action; no rotation or promotion command
        t.key(PREFIX)
        assert "Write PGN file" in t.frame and "Take back move" in t.frame
        t.key(CANCEL)
        t.key(PREFIX + b"a")
        t.key(b"A note")
        t.key(PREFIX + b"c")
        assert "A note" in t.frame
        t.key(PREFIX + b"u")
        assert "position 0/0" in t.frame
        t.key(b"e")
        t.key(PREFIX)
        t.key(b"?")  # must not leak '?' into the move field
        t.key(CANCEL)
        t.key(b"4\r")
        assert "position 1/1" in t.frame
        t.key(PREFIX + b"w")
        assert "Save PGN" in t.frame
        t.key(CANCEL)  # do not write a file
        t.key(PREFIX + b"q")
        t.key(PREFIX + b"o")
        t.key(PREFIX)
        assert "Write PGN file" in t.frame and "Take back move" not in t.frame
        t.key(CANCEL)
        t.key(b"\r")
        assert "Opening explorer · ply 1" in t.frame
        t.key(PREFIX + b"q")
        t.key(PREFIX + b"q")
        t.process.wait(timeout=3)
    finally:
        t.close()


def menu_and_picker(scratch):
    t = Terminal(scratch)
    try:
        assert "Open PGN file" in t.key(PREFIX)
        t.key(b"f")
        t.key(str(scratch / "games.pgn").encode())
        assert "Confirm input" in t.key(PREFIX)
        t.key(b"c")
        assert "Game picker" in t.key(PREFIX)
        assert "Back to previous screen" in t.frame
        t.key(CANCEL)
        t.key(b"\r")
        assert "Write PGN file" in t.key(PREFIX)
        t.key(CANCEL)
        t.key(PREFIX + b"q")
        t.key(PREFIX + b"p")
        t.read(0.5)
        assert "New game" in t.key(PREFIX)
        t.key(b"n")
        assert "Start a new game?" in t.frame
        assert "Confirm" in t.key(PREFIX)
        t.key(b"g")
        assert "Start a new game?" not in t.frame
    finally:
        t.close()


def promotion(scratch):
    t = Terminal(scratch, "--fen", "7k/P7/8/8/8/8/8/7K w - - 0 1")
    try:
        t.key(b"a8\r")
        assert "Promote to knight" in t.key(PREFIX)
        t.key(b"n")
        assert "a8=N" in t.frame
    finally:
        t.close()


def local_and_online(scratch):
    t = Terminal(scratch)
    saved = scratch / "local.pgn"
    try:
        # Set 1 minute + 2 seconds from the main menu.
        t.key(b"\x1b[B" * 4 + b"\x1b[D" * 9)
        t.key(b"\x1b[B" + b"\x1b[C" * 2)
        t.key(PREFIX + b"h")
        assert "Two players" in t.frame and "01:00" in t.frame
        t.key(PREFIX)
        assert "Toggle engine analysis" not in t.frame and "Take back move" not in t.frame
        frame = t.read(2.1)  # the command panel must not pause the clock
        assert "00:58" in frame
        t.key(CANCEL)
        t.key(b"e4\r")
        assert "Moves  1/1" in t.frame, t.frame
        t.key(b"e5\r")
        assert "Moves  2/2" in t.frame  # both players use this keyboard
        t.key(PREFIX + b"w")
        t.key(b"\x7f" * 200 + str(saved).encode() + b"\r")
        pgn = saved.read_text()
        assert '[TimeControl "60+2"]' in pgn and pgn.count("[%clk ") == 2
        t.key(PREFIX + b"q")
        t.key(PREFIX + b"y")
        t.key(PREFIX + b"l")
        assert "Not logged in" in t.frame
        t.key(PREFIX + b"s")
        assert "Log in to Lichess first" in t.frame
        t.key(PREFIX)
        assert "Resume / reconnect game" in t.frame and "Log in with Lichess" in t.frame
    finally:
        t.close()
    t = Terminal(scratch, str(saved))
    try:
        t.key(b"\x1b[F")
        frame = t.read(1.2)
        assert re.search(r"Stockfish d\d+ · 00:0[1-9]\.", frame), frame
        assert "01:02" in frame  # Black's recorded clock is not a countdown
        frame = t.read(1.2)
        assert "01:02" in frame
    finally:
        t.close()


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="chesstty-keys-") as folder:
        scratch = Path(folder)
        database(scratch)
        board(scratch)
        menu_and_picker(scratch)
        promotion(scratch)
        local_and_online(scratch)
    print("Keybinding tests passed at 78x22: prefix, contexts, cancellation, search, moves, notes, undo, promotion, local clocks, analysis clocks, Lichess lobby.")
