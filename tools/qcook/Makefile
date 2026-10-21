CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Werror -D_DEFAULT_SOURCE
SRC      = qcook.c tex.c world.c game.c sound.c ents.c items.c decim.c stream.c bmf.c menu.c spumusic.c
HDR      = qcook.h anorms.h ../../formats/qtex.h ../../formats/qworld.h ../../formats/qgame.h ../../formats/qsound.h \
           ../../formats/qlevels.h ../../formats/qstream.h ../../formats/qmenu.h ../../formats/qspu.h

qcook: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

clean:
	rm -f qcook

.PHONY: clean
