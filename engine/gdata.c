#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxgpu.h>

#include "cd.h"
#include "gdata.h"
#include "world.h"
#include "mem.h"
#include "qtex.h"
#include "settings.h"

gdata_t gdata;
qw_vert *gd_view_pool;


/* GAME_<level>.DAT into the level arena (a staging area), checked */
static const uint8_t *gdata_file(int level, uint32_t *size)
{
	char name[20];
	snprintf(name, sizeof name, "\\GAME_%d.DAT;1", level);
	const uint8_t *f = cd_load(name, size, NULL);
	const qg_header *h = (const qg_header *) f;
	if (!f || *size < sizeof(qg_header) || memcmp(f, QGAME_MAGIC, 4)) {
		printf("gdata: no usable %s on the disc\n", name);
		return 0;
	}
	if (h->num_models != QGM_COUNT || h->num_pics != QGP_COUNT || h->num_aux != QGA_COUNT ||
	    h->ofs_geo + h->len_geo > *size) {
		printf("gdata: %s does not match this engine\n", name);
		return 0;
	}
	return f;
}

int gdata_load(int level)
{
	uint32_t mark = mem_level_mark(), size;
	memset(&gdata, 0, sizeof gdata);
	const uint8_t *f = gdata_file(level, &size);
	if (!f) {
		mem_level_release(mark);
		return 0;
	}
	const qg_header *h = (const qg_header *) f;

	/* images to VRAM: the CLUT row, then the pages' used rows */
	const qtex_rect *r = (const qtex_rect *) (f + h->ofs_rects);
	for (uint32_t i = 0; i < h->num_rects; i++)
		vram_upload(f + r[i].data_ofs, &r[i]);   /* (world.c: LoadImage's DMA rounding) */
	DrawSync(0);

	/* tables and geometry to the game arena */
	uint32_t tables = (uint32_t) (QGM_COUNT * sizeof(qg_model) + QGP_COUNT * sizeof(qg_pic) +
	                              QG_LIGHT_YAWS * QG_NORMALS + 256 * 4 + QGA_COUNT * sizeof(qg_aux));
	uint32_t wtab = Q1PSX_WEAPON_2D ? QG_WEAPONS * sizeof(qg_wsprite) + h->num_wframes * sizeof(qg_wframe) : 0;
	uint32_t view = 0;                            /* the largest view model's pool */
	for (int i = 0; i < QGM_COUNT; i++) {
		const qg_model *m = (const qg_model *) (f + h->ofs_models) + i;
		if ((m->flags & QGF_VIEW) && m->num_verts > view)
			view = m->num_verts;
	}
	uint8_t *t = mem_game_alloc(tables);
	uint8_t *g = mem_game_alloc(h->len_geo);
	uint8_t *fin = h->len_finales ? mem_game_alloc(h->len_finales) : 0;
	uint8_t *wt = wtab ? mem_game_alloc(wtab) : 0;
	uint8_t *fr = h->num_free ? mem_game_alloc(h->num_free * sizeof(qg_free)) : 0;
	gd_view_pool = view ? mem_game_alloc(view * sizeof(qw_vert)) : 0;
	if (!t || !g || (h->len_finales && !fin) || (wtab && !wt) || (h->num_free && !fr) || (view && !gd_view_pool)) {
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
	/* the 2D builds' gun: its tables; its packets stay on the disc until a
	   map's start (game_ws_sync): where they are */
	if (wt) {
		memcpy(wt, f + h->ofs_wsprites, QG_WEAPONS * sizeof(qg_wsprite));
		memcpy(wt + QG_WEAPONS * sizeof(qg_wsprite), f + h->ofs_wframes, h->num_wframes * sizeof(qg_wframe));
		gdata.wsprites    = (const qg_wsprite *) wt;
		gdata.wframes     = (const qg_wframe *) (wt + QG_WEAPONS * sizeof(qg_wsprite));
		gdata.num_wframes = h->num_wframes;
		gdata.wslot       = h->wslot;
	}
	if (wt && h->len_wpix) {
		gdata.ws_lba     = (int32_t) (cd_last_lba() + h->ofs_wpix / CD_SECTOR);
		gdata.ws_skew    = (uint16_t) (h->ofs_wpix % CD_SECTOR);
		gdata.ws_sectors = (uint16_t) ((gdata.ws_skew + h->len_wpix + CD_SECTOR - 1) / CD_SECTOR);
	}
	if (fr) {
		memcpy(fr, f + h->ofs_free, h->num_free * sizeof(qg_free));
		gdata.free     = (const qg_free *) fr;
		gdata.num_free = h->num_free;
	}
	gdata.geo   = g;
	gdata.clut  = h->clut;
	gdata.level = level;
	gdata.ok    = 1;
	mem_level_release(mark);
	printf("gdata: %u models, %u pictures, %u VRAM rects, %u B of geometry -> game arena %u KB\n",
	       h->num_models, h->num_pics, h->num_rects, h->len_geo, mem_game_used() / 1024);
	return 1;
}

/* their links made absolute (KUSEG, as the ordering table's), a chain
   two frames share once; a chain's last stays 0xffffff (game.c hangs it
   in the ordering table) */
uint8_t *gdata_ws_link(uint8_t *block)
{
	uint8_t *wp = block + gdata.ws_skew;
	for (uint32_t i = 0; i < gdata.num_wframes; i++) {
		uint32_t at = gdata.wframes[i].ofs, j = 0;
		if (!gdata.wframes[i].w)
			continue;                             /* (drawn 3D: no chain) */
		while (j < i && gdata.wframes[j].ofs != at)
			j++;
		if (j < i)
			continue;                             /* (shared: linked already) */
		for (;;) {
			uint32_t *tag = (uint32_t *) (wp + at), next = *tag & 0xffffffu;
			if (next == 0xffffffu)
				break;
			*tag = (*tag & 0xff000000u) | (((uint32_t) wp + next) & 0xffffffu);
			at = next;
		}
	}
	return wp;
}
