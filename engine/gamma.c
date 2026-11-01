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
 * stays so.
 *
 * Quake's palette is the colour after the light (the colormap's row), so
 * its gamma brightens the light too: a power curve splits over a product,
 * g(t x l) = g(t) x g(l), and the light bytes (128 = the texel as it is)
 * take 128 x (L / 128) ^ (1 - level / 20). Without that share a dark
 * place stays dark at any level: a texel brightened, times a light near
 * nothing (docs/porting-notes.md §121). The bytes in RAM are rewritten
 * (gamma_light_level: the level they are at): the world's records, the
 * zones' chunks as they arrive, the brush models' pieces, the models'
 * normal factors; a model's light goes through gamma_light as it is
 * taken. A new level rewrites them between frames with no menu up
 * (engine/main.c light_commit: once a menu, whatever the slider's steps):
 * the levels' curves are nested (light_curve), so a byte stays within the
 * run of values the brightest level it went through merged (near white, 5
 * at most), however many times the level changes; a load reads the
 * originals again. Nothing changes in a frame's work.
 */

#include <stdint.h>
#include <string.h>
#include <psxgpu.h>

#include "gamma.h"
#include "mem.h"
#include "qworld.h"

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
static int light_level;                           /* the level the light bytes in RAM are at */
static int light_want;                            /* the slider's, for them */
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
	light_want = level;                           /* (the light's: between frames, no menu up) */
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

/* ------------------------------------------------------------ the light -- */

uint8_t gamma_light[256] = {                      /* the curve at light_level */
#define ROW(x) x, x + 1, x + 2, x + 3, x + 4, x + 5, x + 6, x + 7
#define ROWS(x) ROW(x), ROW(x + 8), ROW(x + 16), ROW(x + 24)
	ROWS(0), ROWS(32), ROWS(64), ROWS(96), ROWS(128), ROWS(160), ROWS(192), ROWS(224),
#undef ROWS
#undef ROW
};

int gamma_light_level(void)
{
	return light_level;
}

/* log2(x) x 65536, x in 1 .. 65535: the top bit, then the fraction's bits
   by squaring (x / 2^15 in [1, 2): over 2, a bit and halved) */
Q_COLD static int32_t log2_16(uint32_t x)
{
	int32_t r = 15;
	while (x < 32768) {
		x <<= 1;
		r--;
	}
	r <<= 16;
	for (int32_t b = 32768; b; b >>= 1) {
		x = x * x >> 15;
		if (x >= 65536) {
			x >>= 1;
			r += b;
		}
	}
	return r;
}

/* The curve at a level: 128 x (x / 128) ^ (1 - level / 20), rounded (255
   at most), the levels nested: each one is built on the one before and
   gives the same byte to the x the one before merged (the curve taken at
   the run's first x; 3 at most from the exact rounding). So a byte at a
   level stands for its run of x, the first of which (gamma_light_change)
   is the same at every level as fine or finer: a byte goes through any
   number of changes within the run of the brightest level it was at (5
   at most near white, at the top level), where steps of the exact curve
   from the runs' middles drifted 20 in 60 changes. The logs (x 65536,
   less 7: 128's) are taken once: x's, and y + 1/2's, the bounds y is
   counted up to. */
Q_COLD static void light_curve(uint8_t g[256], int level)
{
	int32_t lx[256], ly[256];
	for (uint32_t i = 1; i < 256; i++) {
		lx[i] = log2_16(i) - (7 << 16);
		ly[i - 1] = log2_16(2 * i - 1) - (8 << 16);
	}
	for (uint32_t x = 0; x < 256; x++)
		g[x] = (uint8_t) x;
	for (int k = 1; k <= level; k++) {
		uint32_t y = 0, old = 0, v = 0;
		for (uint32_t x = 1; x < 256; x++) {
			uint32_t was = g[x];
			if (was != old) {                         /* a run's first x: the curve there */
				int32_t e = lx[x] * (20 - k) / 20;
				while (y < 255 && e >= ly[y])
					y++;
				v = y;
			}
			old = was;
			g[x] = (uint8_t) v;
		}
	}
}

Q_COLD int gamma_light_change(uint8_t t[256])
{
	if (light_want == light_level)
		return 0;
	uint8_t g[256];
	light_curve(g, light_want);
	for (uint32_t y = 0; y < 256; y++)            /* (a byte the old curve never gives) */
		t[y] = g[y];
	for (uint32_t x = 256; x-- > 0;)              /* each y: the first x the old curve gives it */
		t[gamma_light[x]] = g[x];
	memcpy(gamma_light, g, sizeof g);
	light_level = light_want;
	return 1;
}

/* 2^32 / 4n rounded up, n = 0 .. QW_MAX_VERTS: a record's average light,
   (sum + 2n) / 4n, by a product's high word (engine/zone.s
   gamma_light_records; exact for every sum of n bytes) */
const uint32_t light_recip[QW_MAX_VERTS + 1] = {
	0x00000000, 0x40000000, 0x20000000, 0x15555556, 0x10000000, 0x0ccccccd, 0x0aaaaaab, 0x0924924a,
	0x08000000, 0x071c71c8, 0x06666667, 0x05d1745e, 0x05555556, 0x04ec4ec5, 0x04924925, 0x04444445,
	0x04000000, 0x03c3c3c4, 0x038e38e4, 0x035e50d8, 0x03333334, 0x030c30c4, 0x02e8ba2f, 0x02c8590c,
	0x02aaaaab, 0x028f5c29, 0x02762763, 0x025ed098, 0x02492493, 0x0234f72d, 0x02222223, 0x02108422,
	0x02000000, 0x01f07c20, 0x01e1e1e2, 0x01d41d42, 0x01c71c72, 0x01bacf92, 0x01af286c, 0x01a41a42,
	0x0199999a, 0x018f9c19, 0x01861862,
};

#if Q1PSX_ENGINE_C
/* the C engine's (engine/zone.s's asm, followed) */
uint8_t *gamma_light_records(uint8_t *p, const uint8_t *end, uint32_t n, const uint8_t *t)
{
	for (; n; n--) {
		qw_record *r = (qw_record *) p;
		if (p + sizeof *r > end || !r->size || p + r->size > end)
			break;
		uint32_t words = ((uint32_t) r->num_verts + 3) >> 2, sum = 0;
		uint8_t *light = p + r->polys - words * 4;
		for (uint32_t k = 0; k < words * 4; k++) {   /* (the padding's zeros: t[0] = 0) */
			light[k] = t[light[k]];
			sum += light[k];
		}
		uint32_t avg = (uint32_t) (((uint64_t) (sum + 2u * r->num_verts) * light_recip[r->num_verts]) >> 32);
		r->flags = (uint8_t) ((r->flags & (QWF_SKY | QWF_LIQUID)) | (avg > 63 ? 63 : avg) << 2);
		p += r->size;
	}
	return p;
}
#endif
