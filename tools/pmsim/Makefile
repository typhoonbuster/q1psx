CC     ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror -D_DEFAULT_SOURCE
ENGINE  = ../../engine
SRC     = pmsim.c $(ENGINE)/clip.c $(ENGINE)/pmove.c

pmsim: $(SRC) $(ENGINE)/clip.h $(ENGINE)/pmove.h $(ENGINE)/q_fixed.h $(ENGINE)/world.h ../../formats/qworld.h
	$(CC) $(CFLAGS) -I$(ENGINE) -I../../formats -o $@ $(SRC) -lm

clean:
	rm -f pmsim

.PHONY: clean
