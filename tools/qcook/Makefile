CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Werror -D_DEFAULT_SOURCE
SRC      = qcook.c tex.c world.c game.c
HDR      = qcook.h anorms.h ../../formats/qtex.h ../../formats/qworld.h ../../formats/qgame.h

qcook: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

clean:
	rm -f qcook

.PHONY: clean
