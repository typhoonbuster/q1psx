#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxgpu.h>

#include "cd.h"
#include "gdata.h"
#include "mem.h"
#include "qtex.h"

gdata_t gdata;

int gdata_load(void)
{
	uint32_t mark = mem_level_mark(), size;
	memset(&gdata, 0, sizeof gdata);
	const uint8_t *f = cd_load("\\GAME.DAT;1", &size, NULL);
	if (!f || size < sizeof(qg_header) || memcmp(f, QGAME_MAGIC, 4)) {
		printf("gdata: no usable GAME.DAT on the disc\n");
		mem_level_release(mark);
		return 0;
	}
	const qg_header *h = (const qg_header *) f;
	if (h->num_models != QGM_COUNT || h->num_pics != QGP_COUNT ||
	    h->ofs_geo + h->len_geo > size) {
		printf("gdata: GAME.DAT does not match this engine\n");
		mem_level_release(mark);
		return 0;
	}

	/* images to VRAM: the CLUT row, then the pages' used rows */
	const qtex_rect *r = (const qtex_rect *) (f + h->ofs_rects);
	for (uint32_t i = 0; i < h->num_rects; i++) {
		RECT rc = { (int16_t) r[i].x, (int16_t) r[i].y, (int16_t) r[i].w, (int16_t) r[i].h };
		LoadImage(&rc, (const uint32_t *) (f + r[i].data_ofs));
	}
	DrawSync(0);

	/* tables and geometry to the game arena */
	uint32_t tables = (uint32_t) (QGM_COUNT * sizeof(qg_model) + QGP_COUNT * sizeof(qg_pic) +
	                              QG_LIGHT_YAWS * QG_NORMALS + 256 * 4);
	uint8_t *t = mem_game_alloc(tables);
	uint8_t *g = mem_game_alloc(h->len_geo);
	if (!t || !g) {
		mem_level_release(mark);
		return 0;
	}
	uint8_t *p = t;
	memcpy(p, f + h->ofs_models, QGM_COUNT * sizeof(qg_model)); gdata.models = (const qg_model *) p; p += QGM_COUNT * sizeof(qg_model);
	memcpy(p, f + h->ofs_pics, QGP_COUNT * sizeof(qg_pic));     gdata.pics = (const qg_pic *) p;     p += QGP_COUNT * sizeof(qg_pic);
	memcpy(p, f + h->ofs_dots, QG_LIGHT_YAWS * QG_NORMALS);      gdata.dots = p;                      p += QG_LIGHT_YAWS * QG_NORMALS;
	memcpy(p, f + h->ofs_rgb, 256 * 4);                          gdata.rgb = (const uint32_t *) p;
	memcpy(g, f + h->ofs_geo, h->len_geo);
	gdata.geo  = g;
	gdata.clut = h->clut;
	gdata.ok   = 1;
	mem_level_release(mark);
	printf("gdata: %u models, %u pictures, %u VRAM rects, %u B of geometry -> game arena %u KB\n",
	       h->num_models, h->num_pics, h->num_rects, h->len_geo, mem_game_used() / 1024);
	return 1;
}
