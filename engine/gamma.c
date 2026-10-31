/*
 * q1psx — the Options menu's Brightness (engine/gamma.h). Quake brightens
 * the palette (view.c BuildGammaTable: 255 x ((i + 0.5) / 255.5) ^ gamma,
 * v_gamma from 1 down to 0.5); the PlayStation draws every texel through a
 * CLUT, so the CLUTs take the same curve on their 5-bit channels:
 *   - the map's textures' CLUTs (VRAM rows 481-502: the world, its brush
 *     models, the sky, the liquids), the game's palette (row 504: the
 *     monsters, the items, the gun, the status bar's pictures) and the
 *     console's (row 480) go to VRAM through the current level's table
 *     (gamma_upload: engine/world.c vram_upload, ui.c);
 *   - the map's and the game's are kept as the files have them (the map's
 *     with its texture tables, 1.3-3.2 KB; the game's here, 512 bytes): a
 *     change sends them again through the new level's table (a 5-bit
 *     table merges neighbours near white, so a colour read back from VRAM
 *     could not be taken back exactly);
 *   - the menu's pictures (rows 505-506) and the particles' colours keep
 *     theirs.
 * Black with its STP bit clear (0x0000) is the GPU's transparent texel and
 * stays so. Nothing changes in a frame's work.
 */

#include <stdint.h>
#include <string.h>
#include <psxgpu.h>

#include "gamma.h"

#define COLS 320                 /* the CLUT rows' width (halfwords)          */

/* BuildGammaTable at v_gamma 1 - level / 20, on the 5-bit channels (c5 to
   8 bits: c << 3 | c >> 2; back: x 31 / 255, rounded) */
static const uint8_t gamma5[GAMMA_LEVELS][32] = {
	{  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 },
	{  0,  1,  2,  3,  4,  5,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 18, 19, 21, 22, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 },
	{  0,  1,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 27, 28, 29, 30, 31 },
	{  0,  2,  3,  4,  5,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 20, 21, 22, 23, 24, 25, 26, 27, 28, 28, 29, 30, 31 },
	{  0,  2,  3,  5,  6,  7,  8,  9, 11, 12, 13, 13, 15, 16, 16, 17, 18, 19, 20, 21, 22, 23, 24, 24, 25, 26, 27, 28, 29, 29, 30, 31 },
	{  0,  2,  4,  5,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 22, 23, 24, 25, 26, 26, 27, 28, 29, 30, 30, 31 },
	{  0,  3,  4,  6,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 20, 21, 22, 23, 24, 24, 25, 26, 27, 27, 28, 29, 30, 30, 31 },
	{  0,  3,  5,  7,  8,  9, 11, 12, 13, 14, 15, 16, 17, 18, 18, 19, 20, 21, 22, 22, 23, 24, 25, 26, 26, 27, 28, 28, 29, 30, 30, 31 },
	{  1,  4,  6,  8,  9, 10, 12, 13, 14, 15, 16, 17, 18, 18, 19, 20, 21, 22, 22, 23, 24, 25, 25, 26, 27, 27, 28, 29, 29, 30, 30, 31 },
	{  1,  5,  7,  9, 10, 11, 13, 14, 15, 16, 17, 18, 18, 19, 20, 21, 22, 22, 23, 24, 24, 25, 26, 26, 27, 28, 28, 29, 29, 30, 31, 31 },
	{  1,  6,  8, 10, 11, 13, 14, 15, 16, 17, 18, 18, 19, 20, 21, 22, 22, 23, 24, 24, 25, 26, 26, 27, 27, 28, 28, 29, 30, 30, 31, 31 },
};

static int cur;                                   /* the level in VRAM */
static uint16_t row[COLS] __attribute__((aligned(4)));   /* (640 bytes: LoadImage's 64-byte units) */
static uint16_t game_pal[256] __attribute__((aligned(4)));   /* the game's palette as cooked */
static int game_pal_ok;
static const qtex_rect *map_rects;                /* the map's CLUT rectangles as cooked, their */
static const uint8_t   *map_base;                 /* data_ofs from map_base (engine/world.c)    */
static uint32_t         map_n;

int gamma_level(void)
{
	return cur;
}

/* a colour's 5-bit channels through table t; the STP bit kept; 0x0000
   (transparent) kept */
static uint16_t remap(uint16_t c, const uint8_t *t)
{
	if (!c)
		return 0;
	return (uint16_t) ((c & 0x8000) | t[c & 31] | t[(c >> 5) & 31] << 5 | t[(c >> 10) & 31] << 10);
}

void gamma_upload(const uint8_t *data, int x, int y, int w, int h)
{
	const uint16_t *src = (const uint16_t *) data;
	for (int j = 0; j < h; j++, src += w) {
		int n = w < COLS ? w : COLS;
		for (int i = 0; i < n; i++)
			row[i] = remap(src[i], gamma5[cur]);
		int pad = ((n * 2 + 63) & ~63) / 2;           /* (zeros: GP0 NOPs past the row) */
		for (int i = n; i < pad; i++)
			row[i] = 0;
		RECT rc = { (int16_t) x, (int16_t) (y + j), (int16_t) n, 1 };
		LoadImage(&rc, (const uint32_t *) row);
		DrawSync(0);                                  /* (the buffer's next row) */
	}
}

void gamma_keep_game(const uint8_t *pal)
{
	memcpy(game_pal, pal, sizeof game_pal);
	game_pal_ok = 1;
}

void gamma_keep_map(const qtex_rect *rects, const uint8_t *base, uint32_t n)
{
	map_rects = rects;
	map_base  = base;
	map_n     = n;
}

#ifdef Q1PSX_BENCH
/* (the bench: the map's CLUTs kept, FNV-1a over their halfwords, against
   the cooked file's) */
uint32_t gamma_kept_hash(int *rects)
{
	uint32_t h = 2166136261u;
	for (uint32_t i = 0; i < map_n; i++) {
		const uint8_t *d = map_base + map_rects[i].data_ofs;
		for (uint32_t k = 0; k < (uint32_t) map_rects[i].w * map_rects[i].h * 2; k++)
			h = (h ^ d[k]) * 16777619u;
	}
	*rects = (int) map_n;
	return h;
}
#endif

void gamma_set(int level)
{
	if (level < 0)
		level = 0;
	if (level >= GAMMA_LEVELS)
		level = GAMMA_LEVELS - 1;
	if (level == cur)
		return;
	cur = level;
	DrawSync(0);                                  /* (the frame drawn: VRAM still) */
	for (uint32_t i = 0; i < map_n; i++)
		gamma_upload(map_base + map_rects[i].data_ofs, map_rects[i].x, map_rects[i].y, map_rects[i].w,
		             map_rects[i].h);
	if (game_pal_ok)
		gamma_upload((const uint8_t *) game_pal, 0, 504, 256, 1);
}
