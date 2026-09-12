#!/usr/bin/env python3
"""POSIX keyboard smoke tests: emitted UI responses, not a terminal emulator.

Chess/clock/database logic is covered by the C tests. Engine and online
boundaries in chesstty-test use fixtures; no external services are needed.
"""
from contextlib import contextmanager
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import struct
import subprocess
import tempfile
import termios
import time

ROOT = Path(__file__).resolve().parents[1]
ANSI = re.compile(rb"\x1b\[[0-?]*[ -/]*[@-~]")
PREFIX = b"\x18"
CANCEL = b"\x07"
ENGINE = ROOT / "keybindings-engine"


class Terminal:
    def __init__(self, master, process):
        self.master, self.process = master, process

    def expect(self, text, timeout=10):
        output = b""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if not select.select([self.master], [], [], max(0, deadline - time.monotonic()))[0]:
                break
            try:
                chunk = os.read(self.master, 65536)
            except OSError:
                break
            if not chunk:
                break
            output += chunk
            if text in ANSI.sub(b"", output).decode("utf-8", errors="replace"):
                return
        raise AssertionError(f"Expected {text!r}; exit={self.process.poll()}\n"
                             + output[-8000:].decode("utf-8", errors="replace"))

    def key(self, data, expected):
        # Discard earlier output so each assertion observes a new response.
        while select.select([self.master], [], [], 0)[0]:
            if not os.read(self.master, 65536):
                break
        os.write(self.master, data)
        self.expect(expected)


@contextmanager
def terminal(scratch):
    master, slave = pty.openpty()
    process = None
    try:
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 22, 78, 0, 0))
        process = subprocess.Popen(
            [str(ROOT / "chesstty-test"), "--engine", str(ENGINE)],
            stdin=slave, stdout=slave, stderr=slave, cwd=scratch,
            start_new_session=True,
            env={**os.environ, "TERM": "xterm-256color", "XDG_DATA_HOME": str(scratch)},
        )
        os.close(slave)
        slave = None
        t = Terminal(master, process)
        t.expect("C-x commands")
        yield t
    finally:
        try:
            if process is not None:
                if process.poll() is None:
                    process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)
        finally:
            os.close(master)
            if slave is not None:
                os.close(slave)


def smoke(scratch):
    with terminal(scratch) as t:
        t.key(PREFIX, "Open PGN file")
        t.key(b"?", "Unknown key")
        t.key(CANCEL, "C-x commands")
        # Cancellation must restore the prefix key, then dispatch analysis.
        t.key(PREFIX, "Analysis board")
        t.key(b"a", "position 0/0")
        t.key(PREFIX, "Take back move")
        t.key(CANCEL, "C-x commands")
        t.key(b"e4\r", "position 1/1")
        t.key(PREFIX + b"u", "position 0/0")
        t.key(PREFIX + b"q", "C-x commands")
        # Text entry uses its own command context.
        t.key(PREFIX + b"f", "PGN file ›")
        t.key(PREFIX, "Confirm input")
        t.key(CANCEL, "PGN file ›")


if __name__ == "__main__":
    if not ENGINE.is_file() or not os.access(ENGINE, os.X_OK):
        raise SystemExit("Missing UCI fixture. Run make test-keybindings.")
    with tempfile.TemporaryDirectory(prefix="chesstty-keys-") as folder:
        smoke(Path(folder))
    print("Keyboard smoke tests passed: prefix, cancellation, board and input contexts.")
