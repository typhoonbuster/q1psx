# q1psx asset cooker — host build (plain C, no PS1 toolchain).
#   make            build ./qcook
#   make run        cook maps/start.bsp from $QUAKE_PAK0 into cooked/

CC     ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -I../../formats
BIN     = qcook

# QUAKE_PAK0 comes from ../../local.env if not already set
ifndef QUAKE_PAK0
QUAKE_PAK0 := $(shell sed -n 's/^[[:space:]]*QUAKE_PAK0[[:space:]]*=[[:space:]]*//p' ../../local.env 2>/dev/null)
endif

$(BIN): qcook.c ../../formats/fidelity.h ../../formats/qtx.h
	$(CC) $(CFLAGS) -o $@ qcook.c

.PHONY: run info clean
run: $(BIN)
	@test -n "$(QUAKE_PAK0)" || { echo "set QUAKE_PAK0 (or ../../local.env)"; exit 1; }
	mkdir -p ../../cooked/id1/high
	./$(BIN) textures --pak "$(QUAKE_PAK0)" --map maps/start.bsp \
	         --fidelity high --out ../../cooked/id1/high/start.qtx
	./$(BIN) info ../../cooked/id1/high/start.qtx | head -30

clean:
	rm -f $(BIN)
