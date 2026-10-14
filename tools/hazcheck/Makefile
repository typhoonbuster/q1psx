CC     ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror

hazcheck: hazcheck.c
	$(CC) $(CFLAGS) -o $@ hazcheck.c

clean:
	rm -f hazcheck

.PHONY: clean
