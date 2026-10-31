CC     ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror -D_DEFAULT_SOURCE

all: pktview

pktview: pktview.c ../../formats/qtex.h ../../formats/qgame.h ../../formats/qworld.h
	$(CC) $(CFLAGS) -o $@ pktview.c

clean:
	rm -f pktview

.PHONY: all clean
