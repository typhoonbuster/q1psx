/*
 * q1psx — the memory card (slot 1): the game's save file, one block
 * (docs/porting-notes.md §120).
 *
 * The file holds Quake's twelve save slots (menu.c MAX_SAVEGAMES), the
 * autosave's slot and the settings. A slot keeps the start of the level
 * it was saved in (engine/game.h game_start_t: the parms the level was
 * entered with, the skill, the runes): loading it plays that level again
 * from its start, as a death does. The autosave is written at each new
 * level's start (a trigger_changelevel's map); the settings with every
 * write, and when the Options menu is left changed.
 *
 * The card is driven directly on SIO0 (engine/pad.c pad_sio_exchange, the
 * pad's poll suspended): the BIOS's card functions need the kernel's
 * interrupt dispatch, which engine/irq.s replaces. The format is the
 * BIOS's (psx-spx "Memory Card Data Format"): block 0's directory, the
 * file in one block: its title frame (the title in Shift-JIS, the icon's
 * CLUT), one icon frame (formats/qgame.h qg_icon: GAME.DAT's), then the
 * data frames.
 */

#ifndef Q1PSX_CARD_H
#define Q1PSX_CARD_H

#include <stdint.h>

#include "game.h"

/* what the card holds, or what an operation did */
enum {
	CARD_OK,            /* the file read, or written                         */
	CARD_NONE,          /* no card in slot 1                                 */
	CARD_UNFORMATTED,   /* a card, not formatted                             */
	CARD_NO_FILE,       /* formatted, no save file of the game's on it       */
	CARD_FULL,          /* no save file and no free block for one            */
	CARD_ERROR          /* a read or a write failed (a card pulled out...)   */
};

#define CARD_SLOTS    12             /* menu.c MAX_SAVEGAMES */
#define CARD_SETTINGS 24             /* bytes (engine/settings.c settings_pack) */

typedef struct {
	char         map[8];             /* the disc's map name ("E1M3"), "" empty */
	game_start_t start;
	uint8_t      pad[4];
} card_slot_t;                       /* 32 bytes */

typedef struct {
	char        magic[4];            /* "Q1SV"                                */
	uint16_t    version;             /* CARD_VERSION                          */
	uint16_t    sum;                 /* the bytes after it, summed            */
	uint8_t     settings[CARD_SETTINGS];
	card_slot_t slot[1 + CARD_SLOTS];   /* [0] the autosave                    */
} card_save_t;                       /* 448 bytes: four frames */

extern card_save_t card_save;        /* the last read's (or written)          */

/* the card read: the file into card_save (CARD_OK) or the card's state
   (card_save cleared: no slot, no settings; also CARD_OK with a file of
   another version or a damaged one: its magic then empty, the next write
   replaces it) */
int card_load(void);

/* card_save written, the settings first packed into it: into the file,
   or a new file in the first free block (CARD_NO_FILE before); the result */
int card_store(void);

/* the card formatted (block 0 written as the BIOS writes it: every block
   free), everything on it lost; the result */
int card_format(void);

#endif
