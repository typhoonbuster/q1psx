/*
 * q1psx — the memory card. See engine/card.h.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxapi.h>
#include <psxgpu.h>

#include "card.h"
#include "gdata.h"
#include "mem.h"
#include "pad.h"
#include "settings.h"

#define CARD_PORT    0               /* slot 1: port 1's /SEL, the card's address 81h */
#define FRAME        128
#define BLOCK_FRAMES 64
#define CARD_VERSION 1
#define DATA_FRAMES  ((int) ((sizeof (card_save_t) + FRAME - 1) / FRAME))
#define TRIES        3

/* the file's name: a region, a product code, the game's own (psx-spx: "BA"
   America, "SLUS-nnnnn", up to 8 more characters) */
static const char file_name[] = "BASLUS-99999QUAKE";

card_save_t card_save;

_Static_assert(sizeof (card_slot_t) == 32 && sizeof (card_save_t) == 448, "card_save_t's layout");

/* ------------------------------------------------------------ a frame -- */

/* frame `sector` read: 81h, 'R', the address; two acks, the address
   confirmed, the 128 bytes, their checksum, 'G' (psx-spx) */
static int frame_read(int sector, uint8_t *buf)
{
	uint8_t tx[140], rx[140];
	for (int t = 0; t < TRIES; t++) {
		memset(tx, 0, sizeof tx);
		tx[0] = 0x81;
		tx[1] = 'R';
		tx[4] = (uint8_t) (sector >> 8);
		tx[5] = (uint8_t) sector;
		int n = pad_sio_exchange(CARD_PORT, tx, 140, rx);
		if (n < 4 || rx[2] != 0x5a || rx[3] != 0x5d)
			return CARD_NONE;                    /* (nothing answers at 81h) */
		if (n == 140 && rx[6] == 0x5c && rx[7] == 0x5d && rx[8] == tx[4] && rx[9] == tx[5] && rx[139] == 'G') {
			uint8_t chk = rx[8] ^ rx[9];
			for (int i = 0; i < FRAME; i++)
				chk ^= rx[10 + i];
			if (chk == rx[138]) {
				memcpy(buf, rx + 10, FRAME);
				return CARD_OK;
			}
		}
		VSync(0);                                /* (the card busy: a vblank, again) */
	}
	return CARD_ERROR;
}

/* frame `sector` written: 81h, 'W', the address, the 128 bytes, their
   checksum; two acks and 'G' (4Eh: a bad checksum, FFh: a bad sector);
   then a vblank for the card's own write */
static int frame_write(int sector, const uint8_t *buf)
{
	uint8_t tx[138], rx[138];
	memset(tx, 0, sizeof tx);
	tx[0] = 0x81;
	tx[1] = 'W';
	tx[4] = (uint8_t) (sector >> 8);
	tx[5] = (uint8_t) sector;
	memcpy(tx + 6, buf, FRAME);
	uint8_t chk = tx[4] ^ tx[5];
	for (int i = 0; i < FRAME; i++)
		chk ^= buf[i];
	tx[134] = chk;
	for (int t = 0; t < TRIES; t++) {
		int n = pad_sio_exchange(CARD_PORT, tx, 138, rx);
		if (n < 4 || rx[2] != 0x5a || rx[3] != 0x5d)
			return CARD_NONE;
		VSync(0);
		if (n == 138 && rx[135] == 0x5c && rx[136] == 0x5d && rx[137] == 'G')
			return CARD_OK;
	}
	return CARD_ERROR;
}

/* ------------------------------------------------------- the directory -- */

static uint8_t xor_sum(const uint8_t *f)
{
	uint8_t x = 0;
	for (int i = 0; i < FRAME - 1; i++)
		x ^= f[i];
	return x;
}

static uint32_t rd32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24;
}

/* block 0 read: the save file's block (0: none) and the first free one
   (0: none); CARD_OK, or the card's state */
static int scan(uint8_t *f, int *ours, int *free_block)
{
	int r = frame_read(0, f);
	if (r != CARD_OK)
		return r;
	if (f[0] != 'M' || f[1] != 'C')
		return CARD_UNFORMATTED;
	*ours = *free_block = 0;
	for (int b = 1; b < 16; b++) {
		if ((r = frame_read(b, f)) != CARD_OK)
			return r;
		if (xor_sum(f) != f[FRAME - 1])
			continue;                            /* (a broken entry: neither) */
		uint32_t state = rd32(f);
		if (state == 0x51 && !*ours && !strncmp((const char *) f + 10, file_name, 21))
			*ours = b;
		if (state >= 0xa0 && state <= 0xa3 && !*free_block)
			*free_block = b;
	}
	return CARD_OK;
}

/* --------------------------------------------------------- the file -- */

static uint16_t data_sum(const card_save_t *s)
{
	const uint8_t *p = (const uint8_t *) s + 8;
	uint16_t sum = 0;
	for (uint32_t i = 8; i < sizeof *s; i++)
		sum = (uint16_t) (sum + *p++);
	return sum;
}

/* a character in full-width Shift-JIS (the title: letters, digits, space) */
static int sjis(uint8_t *d, char c)
{
	uint16_t w = c >= 'A' && c <= 'Z' ? 0x8260 + (c - 'A') : c >= '0' && c <= '9' ? 0x824f + (c - '0')
	           : c >= 'a' && c <= 'z' ? 0x8281 + (c - 'a') : 0x8140;
	d[0] = (uint8_t) (w >> 8);
	d[1] = (uint8_t) w;
	return 2;
}

/* the title frame: "SC", one icon frame, the block's number, the title
   (QUAKE and the autosave's map), the icon's CLUT */
static void title_frame(uint8_t *f)
{
	memset(f, 0, FRAME);
	f[0] = 'S';
	f[1] = 'C';
	f[2] = 0x11;                                 /* one icon frame: still */
	f[3] = 1;                                    /* (its blocks: one) */
	uint8_t *t = f + 4;
	for (const char *c = "QUAKE"; *c; c++)
		t += sjis(t, *c);
	if (card_save.slot[0].map[0]) {
		t += sjis(t, ' ');
		for (int i = 0; i < 8 && card_save.slot[0].map[i]; i++)
			t += sjis(t, card_save.slot[0].map[i]);
	}
	if (gdata.icon)
		memcpy(f + 0x60, gdata.icon->clut, 32);
}

int card_load(void)
{
	uint8_t f[FRAME];
	memset(&card_save, 0, sizeof card_save);
	pad_suspend();
	int ours, free_block, r = scan(f, &ours, &free_block);
	if (r == CARD_OK && !ours)
		r = free_block ? CARD_NO_FILE : CARD_FULL;
	if (r == CARD_OK) {
		card_save_t s;
		uint8_t *d = (uint8_t *) &s;
		for (int i = 0; i < DATA_FRAMES && r == CARD_OK; i++) {
			r = frame_read(ours * BLOCK_FRAMES + 2 + i, f);
			uint32_t n = sizeof s - (uint32_t) i * FRAME;
			memcpy(d + i * FRAME, f, n < FRAME ? n : FRAME);
		}
		if (r == CARD_OK && !memcmp(s.magic, "Q1SV", 4) && s.version == CARD_VERSION && s.sum == data_sum(&s))
			card_save = s;
		else if (r == CARD_OK)
			printf("card: the save file is not this game's version, or damaged: left as it is\n");
	}
	pad_resume();
	printf("card: load %d\n", r);
	return r;
}

int card_store(void)
{
	uint8_t f[FRAME];
	memcpy(card_save.magic, "Q1SV", 4);
	card_save.version = CARD_VERSION;
	settings_pack(card_save.settings);
	card_save.sum = data_sum(&card_save);
	pad_suspend();
	int ours, free_block, r = scan(f, &ours, &free_block);
	if (r == CARD_OK && !ours) {
		if (!free_block) {
			r = CARD_FULL;
		} else {                                 /* the file's directory entry */
			memset(f, 0, FRAME);
			f[0] = 0x51;                         /* in use: its first (and only) block */
			f[5] = 0x20;                         /* 2000h bytes */
			f[8] = f[9] = 0xff;                  /* no next block */
			memcpy(f + 10, file_name, sizeof file_name);
			f[FRAME - 1] = xor_sum(f);
			r = frame_write(free_block, f);
			ours = free_block;
		}
	}
	if (r == CARD_OK) {
		title_frame(f);
		r = frame_write(ours * BLOCK_FRAMES, f);
	}
	if (r == CARD_OK && gdata.icon)
		r = frame_write(ours * BLOCK_FRAMES + 1, gdata.icon->px);
	const uint8_t *d = (const uint8_t *) &card_save;
	for (int i = 0; i < DATA_FRAMES && r == CARD_OK; i++) {
		uint32_t n = sizeof card_save - (uint32_t) i * FRAME;
		memset(f, 0, FRAME);
		memcpy(f, d + i * FRAME, n < FRAME ? n : FRAME);
		r = frame_write(ours * BLOCK_FRAMES + 2 + i, f);
	}
	pad_resume();
	printf("card: store %d\n", r);
	return r;
}

int card_format(void)
{
	uint8_t f[FRAME];
	pad_suspend();
	int r = CARD_OK;
	for (int i = 0; i < BLOCK_FRAMES && r == CARD_OK; i++) {
		memset(f, 0, FRAME);
		if (i == 0 || i == BLOCK_FRAMES - 1) {   /* the header, the write test frame */
			f[0] = 'M';
			f[1] = 'C';
		} else if (i < 16) {                     /* the directory: every block free */
			f[0] = 0xa0;
			f[8] = f[9] = 0xff;
		} else if (i < 36) {                     /* the broken sectors: none */
			f[0] = f[1] = f[2] = f[3] = 0xff;
			f[8] = f[9] = 0xff;
		} else {
			memset(f, 0xff, FRAME);              /* (their replacements, unused) */
		}
		if (i < 36 || i == BLOCK_FRAMES - 1)
			f[FRAME - 1] = xor_sum(f);
		r = frame_write(i, f);
	}
	pad_resume();
	printf("card: format %d\n", r);
	return r;
}
