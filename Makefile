# ChessTTY — chess in the terminal with Stockfish built in
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   make            builds the TUI and, if missing, Stockfish too
#   make engine     downloads and builds Stockfish only (into engine/)
#   make test       move generator, openings, PGN, database and engine self-tests
#   make run        build and launch
#   make dist       portable archive (TUI + engine) in dist/
#   make install    installs into $(PREFIX) (default /usr/local)
#   make clean      removes the TUI binaries
#   make distclean  also removes Stockfish and the downloaded sources

# Version: taken from the git tag, overridden by CI with the tag being released.
VERSION ?= $(shell git describe --tags --dirty --always 2>/dev/null || echo dev)

PREFIX ?= /usr/local
BINDIR  = $(DESTDIR)$(PREFIX)/bin
LIBDIR  = $(DESTDIR)$(PREFIX)/lib/chesstty

CC       ?= cc
CFLAGS   ?= -O2
CPPFLAGS ?=
LDFLAGS  ?=
LDLIBS    = -lm -lcurl

# Kept out of CFLAGS so that overriding CFLAGS (distro packaging, CI) cannot
# silently drop the language standard or the warnings.
CSTD      = -std=c11
CWARN     = -Wall -Wextra
ALL_CFLAGS = $(CSTD) $(CWARN) $(CFLAGS)
VERSION_DEF = -DCHESSTTY_VERSION='"$(VERSION)"'

SF_REPO  = https://github.com/official-stockfish/Stockfish.git
SF_TAG   = sf_17.1
SF_COMP ?= gcc
SF_DIR   = third_party/stockfish

UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)
UNAME_M := $(shell uname -m 2>/dev/null || echo x86_64)

ifeq ($(OS),Windows_NT)
  EXE = .exe
else
  EXE =
endif

ifeq ($(OS),Windows_NT)
  LDFLAGS += -static
  CPPFLAGS += -DCURL_STATICLIB
  LDLIBS := -lm $(shell pkg-config --static --libs libcurl)
  LDLIBS += -lbcrypt -lws2_32 -lshell32
else ifneq ($(UNAME_S),Darwin)
  LDLIBS += -lcrypto
endif

ENGINE_BIN = engine/stockfish$(EXE)

# Architecture selection for the Stockfish build (with a fallback chain).
# `native` first: it detects the host CPU, so we never end up with a binary
# that builds fine and then dies with SIGILL on a machine without AVX2.
ifeq ($(UNAME_M),arm64)
  ifeq ($(UNAME_S),Darwin)
    SF_ARCHS = native apple-silicon armv8
  else
    SF_ARCHS = native armv8-dotprod armv8
  endif
else ifeq ($(UNAME_M),aarch64)
  SF_ARCHS = native armv8-dotprod armv8
else
  SF_ARCHS = native x86-64-avx2 x86-64-sse41-popcnt x86-64
endif

NPROC := $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

SRC = src/main.c src/online.c src/auth.c src/json.c src/clocks.c src/commands.c src/draw.c src/chess.c src/game.c src/pgn.c src/db.c \
      src/engine.c src/term.c src/platform.c src/openings.c src/openings_test.c
OBJ = $(SRC:.c=.o)
HDR = $(wildcard src/*.h)

all: chesstty$(EXE) $(ENGINE_BIN)

# Alias that works on every platform: on Windows the real target carries the
# .exe suffix, so `make chesstty` would have no rule to build.
tui: chesstty$(EXE)

chesstty$(EXE): $(OBJ)
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/openings.o: src/openings_data.inc

%.o: %.c $(HDR) .version
	$(CC) $(ALL_CFLAGS) $(CPPFLAGS) $(VERSION_DEF) -c -o $@ $<

# Rewritten only when VERSION actually changes, so that changing the tag
# forces a rebuild but repeated builds do not.
.version: FORCE
	@echo '$(VERSION)' | cmp -s - $@ 2>/dev/null || echo '$(VERSION)' > $@
FORCE:

# ---------------- Stockfish ----------------
$(ENGINE_BIN):
	@$(MAKE) engine

engine:
	@if [ -x "$(ENGINE_BIN)" ]; then echo "Stockfish already present: $(ENGINE_BIN)"; exit 0; fi; \
	mkdir -p engine third_party; \
	if [ ! -d "$(SF_DIR)/src" ]; then \
	  echo "Downloading the Stockfish sources ($(SF_TAG))..."; \
	  git clone --depth 1 --branch $(SF_TAG) $(SF_REPO) $(SF_DIR) || exit 1; \
	fi; \
	ok=0; \
	for arch in $(SF_ARCHS); do \
	  echo "Building Stockfish (ARCH=$$arch, $(NPROC) jobs)..."; \
	  $(MAKE) -C $(SF_DIR)/src clean >/dev/null 2>&1; \
	  if $(MAKE) -C $(SF_DIR)/src -j$(NPROC) build ARCH=$$arch COMP=$(SF_COMP) >$(SF_DIR)/build-$$arch.log 2>&1; then ok=1; break; fi; \
	  tail -80 $(SF_DIR)/build-$$arch.log; \
	done; \
	if [ $$ok -eq 1 ]; then \
	  cp $(SF_DIR)/src/stockfish$(EXE) $(ENGINE_BIN) && echo "Stockfish ready: $(ENGINE_BIN)"; \
	else \
	  echo "ERROR: the Stockfish build failed. A C++17 compiler, curl and network access are required."; \
	  exit 1; \
	fi

# ---------------- release packaging ----------------
# `make dist` produces an archive: the TUI plus the engine, in
# the layout the binary already looks for (engine/stockfish next to it), so it
# runs straight out of the extracted folder with no install step.
ifeq ($(OS),Windows_NT)
  DIST_OS = windows
else ifeq ($(UNAME_S),Darwin)
  DIST_OS = macos
else ifeq ($(UNAME_S),Linux)
  DIST_OS = linux
else
  DIST_OS = $(UNAME_S)
endif

ifeq ($(UNAME_M),aarch64)
  DIST_ARCH = arm64
else
  DIST_ARCH = $(UNAME_M)
endif

DIST_NAME = chesstty-$(VERSION)-$(DIST_OS)-$(DIST_ARCH)
DIST_DIR  = dist/$(DIST_NAME)

dist: all
	test -f $(SF_DIR)/Copying.txt && test -f $(SF_DIR)/AUTHORS
	rm -rf $(DIST_DIR)
	mkdir -p $(DIST_DIR)/engine $(DIST_DIR)/examples $(DIST_DIR)/data/openings
	cp chesstty$(EXE) $(DIST_DIR)/
	cp $(ENGINE_BIN) $(DIST_DIR)/engine/
	cp README.md LICENSE $(DIST_DIR)/
	cp data/openings/README.md data/openings/COPYING.txt $(DIST_DIR)/data/openings/
	cp examples/*.pgn $(DIST_DIR)/examples/
	cp $(SF_DIR)/Copying.txt $(DIST_DIR)/engine/STOCKFISH-COPYING.txt
	cp $(SF_DIR)/AUTHORS $(DIST_DIR)/engine/STOCKFISH-AUTHORS.txt
ifeq ($(OS),Windows_NT)
	test -d "$(MINGW_PREFIX)/share/licenses"
	cp -R "$(MINGW_PREFIX)/share/licenses" $(DIST_DIR)/third-party-licenses
endif
	@printf '%s\n' \
	  'The stockfish binary in this folder was built from the unmodified' \
	  'official sources, release $(SF_TAG):' \
	  '' \
	  '    $(SF_REPO)  (tag $(SF_TAG))' \
	  '' \
	  'Stockfish is free software under the GNU GPL v3 (STOCKFISH-COPYING.txt),' \
	  'copyright the Stockfish developers (STOCKFISH-AUTHORS.txt).  ChessTTY' \
	  'does not link it: it launches it as a separate process and talks to it' \
	  'over the UCI protocol.' \
	  > $(DIST_DIR)/engine/STOCKFISH-README.txt
ifeq ($(OS),Windows_NT)
	rm -f dist/$(DIST_NAME).zip
	cd dist && zip -qr $(DIST_NAME).zip $(DIST_NAME)
	@echo "Created dist/$(DIST_NAME).zip"
else
	cd dist && COPYFILE_DISABLE=1 tar czf $(DIST_NAME).tar.gz $(DIST_NAME)
	@echo "Created dist/$(DIST_NAME).tar.gz"
endif

# ---------------- utilities ----------------
run: all
	./chesstty$(EXE)

regression-test$(EXE): tests/regression.c src/clocks.c src/chess.c src/game.c src/pgn.c src/db.c src/platform.c src/openings.c $(HDR) src/openings_data.inc
	$(CC) $(ALL_CFLAGS) $(CPPFLAGS) -Isrc $(LDFLAGS) -o $@ tests/regression.c src/clocks.c src/chess.c src/game.c src/pgn.c src/db.c src/platform.c src/openings.c $(LDLIBS)

online-test$(EXE): tests/online.c src/online.c src/auth.c tests/auth_mock.c tests/no_network.c src/json.c src/clocks.c src/chess.c src/game.c src/pgn.c src/platform.c src/openings.c src/commands.c $(HDR) tests/auth_mock.h
	$(CC) $(ALL_CFLAGS) $(CPPFLAGS) -DCHESSTTY_TEST_MOCKS -Isrc $(LDFLAGS) -o $@ tests/online.c src/online.c src/auth.c tests/auth_mock.c tests/no_network.c src/json.c src/clocks.c src/chess.c src/game.c src/pgn.c src/platform.c src/openings.c src/commands.c $(LDLIBS)

test-online: online-test$(EXE)
	@mkdir -p .online-test
	./online-test$(EXE) .online-test/game.pgn
	@rmdir .online-test

# UI tests must not accidentally depend on a locally installed Stockfish.
keybindings-engine$(EXE): tests/uci_fixture.c src/chess.c src/chess.h
	$(CC) $(ALL_CFLAGS) $(CPPFLAGS) -Isrc $(LDFLAGS) -o $@ tests/uci_fixture.c src/chess.c

# The UI keeps its real event loop and drawing, with a mocked online boundary.
UI_TEST_SRC = $(filter-out src/online.c src/auth.c,$(SRC)) tests/online_mock.c
chesstty-test$(EXE): $(UI_TEST_SRC) $(HDR) src/openings_data.inc .version
	$(CC) $(ALL_CFLAGS) $(CPPFLAGS) $(VERSION_DEF) -Isrc $(LDFLAGS) -o $@ $(UI_TEST_SRC) $(LDLIBS)

test-keybindings: chesstty-test$(EXE) keybindings-engine$(EXE)
	python3 tests/keybindings.py

test-regression: regression-test$(EXE)
	@mkdir -p .regression-test
	./regression-test$(EXE) .regression-test/game.pgn
	@rmdir .regression-test

test: chesstty$(EXE) keybindings-engine$(EXE) test-regression test-online
	./chesstty$(EXE) --perft
	./chesstty$(EXE) --openings-test
	./chesstty$(EXE) --pgn-test examples/immortal-games.pgn
	@rm -rf .dbtest && mkdir -p .dbtest && cp examples/*.pgn .dbtest/
	./chesstty$(EXE) --db-test .dbtest
	@rm -rf .dbtest
	./chesstty$(EXE) --engine-test "$(CURDIR)/keybindings-engine$(EXE)"

install: all
	install -d "$(BINDIR)" "$(LIBDIR)"
	install -m 0755 chesstty$(EXE) "$(BINDIR)/"
	install -m 0755 $(ENGINE_BIN) "$(LIBDIR)/"
	@echo "Installed $(BINDIR)/chesstty and $(LIBDIR)/stockfish"

uninstall:
	rm -f "$(BINDIR)/chesstty$(EXE)"
	rm -rf "$(LIBDIR)"

clean:
	rm -f $(OBJ) chesstty$(EXE) regression-test$(EXE) online-test$(EXE) keybindings-engine$(EXE) chesstty-test$(EXE) .version
	rm -rf dist .dbtest

distclean: clean
	rm -rf engine third_party

.PHONY: all tui engine run test dist install uninstall clean distclean FORCE test-regression test-keybindings test-online
