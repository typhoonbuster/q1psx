/*
 * q1psx — asset cooking fidelity levels.
 *
 * ALL four levels are cooked and shipped on the one disc (alongside every game
 * at hand); the player picks a level from the in-game menu. Lower fidelity
 * = smaller RAM/VRAM footprint = a map that won't load at "high" still runs.
 * Numbers are a starting point; tune in Phase 11 with real measurements.
 *
 * Shared by the cook tool and (for the manifest) the engine.
 *
 * NOTE: world textures tile across BSP surfaces, so max_texture is always a
 * power of two — the PS1 GPU only wraps cleanly on power-of-two sizes within a
 * 256x256 page. That is why "medium" keeps 64px textures and instead spends its
 * budget on lightmaps + geometry vs "low".
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
	int  max_texture;   /* power of two; textures downscaled so w,h <= this   */
	int  max_model;     /* MDL skin clamp (power of two)                      */
	int  lightmap_shift;/* lightmap res = full >> shift; >=8 means "none"     */
	int  geom_decimate; /* 0 none, 1 light, 2 heavy (BSP surface merging)     */
} fidelity_spec_t;

static const fidelity_spec_t FIDELITY[FIDELITY_COUNT] = {
	/* name       maxtex  maxmdl  lm_shift  decim  target                        */
	{ "low",        64,     64,      8,       2 }, /* ep1-4 + packs fit, roughest */
	{ "medium",     64,    128,      2,       1 }, /* ep1-4 fit, lit             */
	{ "high",      128,    128,      1,       0 }, /* ep1 default                */
	{ "ultra",     256,    256,      0,       0 }, /* ep1 showcase               */
};

#endif
