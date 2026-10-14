CC     ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror -D_DEFAULT_SOURCE

all: pktview wldref

pktview: pktview.c ../../formats/qtex.h
	$(CC) $(CFLAGS) -o $@ pktview.c

wldref: wldref.c ../../formats/qtex.h ../../formats/qworld.h
	$(CC) $(CFLAGS) -o $@ wldref.c -lm

clean:
	rm -f pktview wldref

.PHONY: all clean
