#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxgpu.h>

#include "cd.h"
#include "gdata.h"
#include "world.h"
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
	if (h->num_models != QGM_COUNT || h->num_pics != QGP_COUNT || h->num_aux != QGA_COUNT ||
	    h->ofs_geo + h->len_geo > size) {
		printf("gdata: GAME.DAT does not match this engine\n");
		mem_level_release(mark);
		return 0;
	}

	/* images to VRAM: the CLUT row, then the pages' used rows */
	const qtex_rect *r = (const qtex_rect *) (f + h->ofs_rects);
	for (uint32_t i = 0; i < h->num_rects; i++)
		vram_upload(f + r[i].data_ofs, &r[i]);   /* (world.c: LoadImage's DMA rounding) */
	DrawSync(0);

	/* tables and geometry to the game arena */
	uint32_t tables = (uint32_t) (QGM_COUNT * sizeof(qg_model) + QGP_COUNT * sizeof(qg_pic) +
	                              QG_LIGHT_YAWS * QG_NORMALS + 256 * 4 + QGA_COUNT * sizeof(qg_aux));
	uint8_t *t = mem_game_alloc(tables);
	uint8_t *g = mem_game_alloc(h->len_geo);
	uint8_t *fin = h->len_finales ? mem_game_alloc(h->len_finales) : 0;
	uint8_t *fr  = h->num_free ? mem_game_alloc(h->num_free * sizeof(qg_free)) : 0;
	if (!t || !g || (h->len_finales && !fin) || (h->num_free && !fr)) {
		mem_level_release(mark);
		return 0;
	}
	uint8_t *p = t;
	memcpy(p, f + h->ofs_models, QGM_COUNT * sizeof(qg_model)); gdata.models = (const qg_model *) p; p += QGM_COUNT * sizeof(qg_model);
	memcpy(p, f + h->ofs_pics, QGP_COUNT * sizeof(qg_pic));     gdata.pics = (const qg_pic *) p;     p += QGP_COUNT * sizeof(qg_pic);
	memcpy(p, f + h->ofs_dots, QG_LIGHT_YAWS * QG_NORMALS);      gdata.dots = p;                      p += QG_LIGHT_YAWS * QG_NORMALS;
	memcpy(p, f + h->ofs_rgb, 256 * 4);                          gdata.rgb = (const uint32_t *) p;    p += 256 * 4;
	memcpy(p, f + h->ofs_aux, QGA_COUNT * sizeof(qg_aux));       gdata.aux = (const qg_aux *) p;
	memcpy(g, f + h->ofs_geo, h->len_geo);
	if (fin) {
		memcpy(fin, f + h->ofs_finales, h->len_finales);
		gdata.finales     = (const qg_finale *) fin;
		gdata.num_finales = h->num_finales;
	}
	if (fr) {
		memcpy(fr, f + h->ofs_free, h->num_free * sizeof(qg_free));
		gdata.free     = (const qg_free *) fr;
		gdata.num_free = h->num_free;
	}
	gdata.geo  = g;
	gdata.clut = h->clut;
	gdata.ok   = 1;
	mem_level_release(mark);
	printf("gdata: %u models, %u pictures, %u VRAM rects, %u B of geometry -> game arena %u KB\n",
	       h->num_models, h->num_pics, h->num_rects, h->len_geo, mem_game_used() / 1024);
	return 1;
}
