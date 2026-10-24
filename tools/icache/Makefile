CC     ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L

icache: icache.c
	$(CC) $(CFLAGS) -o $@ icache.c

clean:
	rm -f icache

.PHONY: clean
