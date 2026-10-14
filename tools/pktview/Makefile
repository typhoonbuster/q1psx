CC     ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror

pktview: pktview.c ../../formats/qtex.h
	$(CC) $(CFLAGS) -o $@ pktview.c

clean:
	rm -f pktview

.PHONY: clean
