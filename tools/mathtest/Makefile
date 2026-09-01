# Host-side unit tests for the engine's fixed-point math layer.
# Plain host gcc — nothing PS1 here. Run:  make -C tools/mathtest run

CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Werror -I../../engine
BIN      = mathtest

SRC = main.c

DEPS = ../../engine/q_fixed.h ../../engine/mathlib.c ../../engine/mathlib.h \
       ../../engine/q_trig_table.c ../../engine/q_trig_table.h

$(BIN): $(SRC) $(DEPS)
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

.PHONY: run clean
run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
