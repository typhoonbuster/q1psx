#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxgpu.h>

#include "cd.h"
#include "gamma.h"
#include "gdata.h"
#include "world.h"
#include "mem.h"
#include "qtex.h"

gdata_t gdata;
qw_vert *gd_view_pool;


/* GAME.DAT into the level arena (a staging area), checked */
static const uint8_t *gdata_file(uint32_t *size)
{
	static const char name[] = "\\GAME.DAT;1";
	const uint8_t *f = cd_load(name, size, NULL);
	const qg_header *h = (const qg_header *) f;
	if (!f || *size < sizeof(qg_header) || memcmp(f, QGAME_MAGIC, 4)) {
		printf("gdata: no usable %s on the disc\n", name);
		return 0;
	}
	if (h->num_models != QGM_COUNT || h->num_pics != QGP_COUNT || h->num_aux != QGA_COUNT ||
	    h->ofs_geo + h->len_geo > *size || h->ofs_icon + sizeof(qg_icon) > *size) {
		printf("gdata: %s does not match this engine\n", name);
		return 0;
	}
	return f;
}

/* the models' normal factors (x128) through t: the Brightness's light,
   a power curve, is a product's (light x factor) on each of them
   (engine/gamma.h; engine/main.c light_commit) */
Q_COLD void gdata_relight(const uint8_t *t)
{
	uint8_t *d = (uint8_t *) gdata.dots;
	if (!d)
		return;
	for (uint32_t i = 0; i < QG_LIGHT_YAWS * QG_NORMALS; i++)
		d[i] = t[d[i]];
}

Q_COLD int gdata_load(void)
{
	uint32_t mark = mem_level_mark(), size;
	memset(&gdata, 0, sizeof gdata);
	const uint8_t *f = gdata_file(&size);
	if (!f) {
		mem_level_release(mark);
		return 0;
	}
	const qg_header *h = (const qg_header *) f;

	/* images to VRAM: the CLUT row, then the pages' used rows */
	const qtex_rect *r = (const qtex_rect *) (f + h->ofs_rects);
	for (uint32_t i = 0; i < h->num_rects; i++) {
		if (r[i].x == QG_CLUT_X && r[i].y == QG_CLUT_Y && r[i].w == 256 && r[i].h == 1)
			gamma_keep_game(f + r[i].data_ofs);  /* (the palette as cooked: the Brightness) */
		vram_upload(f + r[i].data_ofs, &r[i]);   /* (world.c: LoadImage's DMA rounding) */
	}
	DrawSync(0);

	/* tables and geometry to the game arena */
	uint32_t tables = (uint32_t) (QGM_COUNT * sizeof(qg_model) + QGP_COUNT * sizeof(qg_pic) +
	                              QG_LIGHT_YAWS * QG_NORMALS + 256 * 4 + QGA_COUNT * sizeof(qg_aux) +
	                              sizeof(qg_icon));
	uint32_t view = 0;                            /* the largest view model's pool */
	for (int i = 0; i < QGM_COUNT; i++) {
		const qg_model *m = (const qg_model *) (f + h->ofs_models) + i;
		if ((m->flags & QGF_VIEW) && m->num_verts > view)
			view = m->num_verts;
	}
	uint8_t *t = mem_game_alloc(tables);
	uint8_t *g = mem_game_alloc(h->len_geo);
	uint8_t *fin = h->len_finales ? mem_game_alloc(h->len_finales) : 0;
	uint8_t *fr = h->num_free ? mem_game_alloc(h->num_free * sizeof(qg_free)) : 0;
	gd_view_pool = view ? mem_game_alloc(view * sizeof(qw_vert)) : 0;
	if (!t || !g || (h->len_finales && !fin) || (h->num_free && !fr) || (view && !gd_view_pool)) {
		mem_level_release(mark);
		return 0;
	}
	uint8_t *p = t;
	memcpy(p, f + h->ofs_models, QGM_COUNT * sizeof(qg_model)); gdata.models = (const qg_model *) p; p += QGM_COUNT * sizeof(qg_model);
	memcpy(p, f + h->ofs_pics, QGP_COUNT * sizeof(qg_pic));     gdata.pics = (const qg_pic *) p;     p += QGP_COUNT * sizeof(qg_pic);
	memcpy(p, f + h->ofs_dots, QG_LIGHT_YAWS * QG_NORMALS);      gdata.dots = p;
	if (gamma_light_level())                                     /* (the Brightness's light: */
		gdata_relight(gamma_light);                              /*  the factors' curve too) */
	p += QG_LIGHT_YAWS * QG_NORMALS;
	memcpy(p, f + h->ofs_rgb, 256 * 4);                          gdata.rgb = (const uint32_t *) p;    p += 256 * 4;
	memcpy(p, f + h->ofs_aux, QGA_COUNT * sizeof(qg_aux));       gdata.aux = (const qg_aux *) p;      p += QGA_COUNT * sizeof(qg_aux);
	memcpy(p, f + h->ofs_icon, sizeof(qg_icon));                 gdata.icon = (const qg_icon *) p;
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
	gdata.geo   = g;
	gdata.clut  = h->clut;
	gdata.ok    = 1;
	mem_level_release(mark);
	printf("gdata: %u models, %u pictures, %u VRAM rects, %u B of geometry -> game arena %u KB\n",
	       h->num_models, h->num_pics, h->num_rects, h->len_geo, mem_game_used() / 1024);
	return 1;
}
