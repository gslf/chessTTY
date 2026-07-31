# ChessTUI — chess in the terminal with Stockfish built in
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   make            builds the TUI and, if missing, Stockfish too
#   make engine     downloads and builds Stockfish only (into engine/)
#   make test       move generator self-test + engine communication test
#   make run        build and launch
#   make clean      removes the TUI binaries
#   make distclean  also removes Stockfish and the downloaded sources

CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra
LDLIBS   = -lm

SF_REPO  = https://github.com/official-stockfish/Stockfish.git
SF_TAG   = sf_17.1
SF_DIR   = third_party/stockfish

UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)
UNAME_M := $(shell uname -m 2>/dev/null || echo x86_64)

ifeq ($(OS),Windows_NT)
  EXE = .exe
else
  EXE =
endif

ENGINE_BIN = engine/stockfish$(EXE)

# architecture selection for the Stockfish build (with a fallback chain)
ifeq ($(UNAME_M),arm64)
  ifeq ($(UNAME_S),Darwin)
    SF_ARCHS = apple-silicon armv8
  else
    SF_ARCHS = armv8-dotprod armv8
  endif
else ifeq ($(UNAME_M),aarch64)
  SF_ARCHS = armv8-dotprod armv8
else
  SF_ARCHS = x86-64-avx2 x86-64-sse41-popcnt x86-64
endif

NPROC := $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

SRC = src/main.c src/draw.c src/chess.c src/game.c src/pgn.c src/engine.c \
      src/term.c src/platform.c
OBJ = $(SRC:.c=.o)
HDR = $(wildcard src/*.h)

all: chesstui$(EXE) $(ENGINE_BIN)

chesstui$(EXE): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c $(HDR)
	$(CC) $(CFLAGS) -c -o $@ $<

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
	  if $(MAKE) -C $(SF_DIR)/src -j$(NPROC) build ARCH=$$arch >/dev/null 2>&1; then ok=1; break; \
	  else $(MAKE) -C $(SF_DIR)/src clean >/dev/null 2>&1; fi; \
	done; \
	if [ $$ok -eq 1 ]; then \
	  cp $(SF_DIR)/src/stockfish$(EXE) $(ENGINE_BIN) && echo "Stockfish ready: $(ENGINE_BIN)"; \
	else \
	  echo "ERROR: the Stockfish build failed. A C++17 compiler, curl and network access are required."; \
	  exit 1; \
	fi

# ---------------- utilities ----------------
run: all
	./chesstui$(EXE)

test: chesstui$(EXE)
	./chesstui$(EXE) --perft
	./chesstui$(EXE) --engine-test

clean:
	rm -f $(OBJ) chesstui$(EXE)

distclean: clean
	rm -rf engine third_party

.PHONY: all engine run test clean distclean
