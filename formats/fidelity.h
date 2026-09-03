/*
 * q1psx — asset cooking fidelity levels.
 *
 * ALL four levels are cooked and shipped on the one disc (alongside every game
 * at hand); the player picks a level from the in-game debug control and
 * the scene is reloaded. Lower fidelity = smaller RAM/VRAM footprint and fewer
 * world faces = a map that won't run at "high" still runs, and the frame budget
 * gets some slack back.
 *
 * Shared by the cook tool and the engine.
 *
 * What each field drives (cook side):
 *   atlas_cell     world-texture atlas cell, in texels. World tiles are snapped
 *                  to a power of two <= this, so it is also the max tile size.
 *                  Capped at 64: 20x7 64px cells is all the VRAM left of the
 *                  debug font once the two framebuffers are placed. Bigger tiles
 *                  need a per-fidelity atlas layout — later.
 *   min_face_area  world faces with a 3D area (world units^2) below this are
 *                  dropped at cook time (emitted as an empty QSF slot the
 *                  renderer skips). Fewer faces reach draw_face. 0 = keep all.
 *
 * Not consumed yet (models / lightmaps / BSP decimation are future phases —
 * "ultra" == "high" until then):
 *   max_model, lightmap_shift, geom_decimate
 *
 * Numbers are a starting point; tune in Phase 11 with real measurements.
 */

#ifndef Q1PSX_FIDELITY_H
#define Q1PSX_FIDELITY_H

typedef enum {
	FIDELITY_LOW = 0,
	FIDELITY_MEDIUM,
	FIDELITY_HIGH,     /* default */
	FIDELITY_ULTRA,
	FIDELITY_COUNT
} fidelity_t;

typedef struct {
	const char *name;
	int  atlas_cell;    /* world-texture atlas cell size in texels (power of 2) */
	int  min_face_area; /* cook drops world faces smaller than this (u^2), 0=keep */
	int  max_model;     /* MDL skin clamp (power of two)          — not used yet  */
	int  lightmap_shift;/* lightmap res = full >> shift; >=8 = none — not used yet */
	int  geom_decimate; /* 0 none, 1 light, 2 heavy (BSP merging)  — not used yet */
} fidelity_spec_t;

static const fidelity_spec_t FIDELITY[FIDELITY_COUNT] = {
	/* name      cell  minarea  maxmdl  lm_shift  decim                          */
	{ "low",       16,   1024,     64,      8,      2 }, /* 16px tiles, drop <32u faces */
	{ "medium",    32,    256,    128,      2,      1 }, /* 32px tiles, drop <16u faces */
	{ "high",      64,      0,    128,      1,      0 }, /* 64px tiles, every face       */
	{ "ultra",     64,      0,    256,      0,      0 }, /* == high for now              */
};

#endif
