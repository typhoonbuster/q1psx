# Host-side unit tests for the engine's fixed-point math layer.
# Plain host gcc — nothing PS1 here. Run:  make -C tools/mathtest run

CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Werror -I../../engine
BIN      = mathtest

SRC = main.c

$(BIN): $(SRC) ../../engine/q_fixed.h ../../engine/mathlib.c ../../engine/mathlib.h
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

.PHONY: run clean
run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
