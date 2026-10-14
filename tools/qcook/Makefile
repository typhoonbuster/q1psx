CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Werror -D_DEFAULT_SOURCE
SRC      = qcook.c tex.c world.c
HDR      = qcook.h ../../formats/qtex.h ../../formats/qworld.h

qcook: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

clean:
	rm -f qcook

.PHONY: clean
