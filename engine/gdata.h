/*
 * q1psx — the game data (formats/qgame.h): weapon and projectile models, the
 * explosion sprite, the HUD pictures. Loaded once at boot from GAME.DAT:
 * its images go to VRAM (the pages above the map's, its own CLUT), its
 * tables and geometry to the game arena.
 */

#ifndef Q1PSX_GDATA_H
#define Q1PSX_GDATA_H

#include <stdint.h>

#include "qgame.h"

typedef struct {
	int             ok;
	const qg_model *models;    /* [QGM_COUNT]                              */
	const qg_pic   *pics;      /* [QGP_COUNT]                              */
	const uint8_t  *dots;      /* [QG_LIGHT_YAWS][QG_NORMALS]              */
	const uint32_t *rgb;       /* [256] palette colour words               */
	uint8_t        *geo;       /* the geometry block (records are patched
	                              per draw: it is writable)                */
	uint16_t        clut;      /* GPU CLUT word of the game palette        */
	const qg_aux   *aux;       /* [QGA_COUNT]: 4-bit pictures (ui.h)       */
	const qg_finale *finales;  /* the episodes' end texts, and their count */
	uint32_t        num_finales;
	const qg_free  *free;      /* what the images leave of the game's pages
	                              (the streamed skins' room), and its count */
	uint32_t        num_free;
} gdata_t;

extern gdata_t gdata;

/* Load GAME.DAT (before any map: it uses the level arena as a staging
   area). 1 on success. */
int gdata_load(void);

/* a model's frame pool and record stream */
static inline const qw_vert *gd_frame(const qg_model *m, int frame)
{
	return (const qw_vert *) (gdata.geo + m->ofs_frames) + (uint32_t) frame * m->num_verts;
}
static inline uint8_t *gd_records(const qg_model *m)
{
	return gdata.geo + m->ofs_records;
}

#endif
