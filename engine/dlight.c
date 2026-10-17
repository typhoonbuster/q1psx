#include <stdint.h>
#include <string.h>

#include "dlight.h"
#include "mem.h"
#include "prof.h"
#include "render.h"
#include "world.h"

dlight_t dlights[MAX_DLIGHTS];

/* the lights as the renderer's block hook reads them (r_state.lights):
   vertex units */
typedef struct {
	int32_t o[3];
	int32_t radius;
	int32_t minlight;
	int32_t rmm;              /* radius - minlight: the sphere tests' bound */
	int32_t pad[2];
} rlight_t;                   /* 32 bytes (engine/r_defs.inc DL_*) */

static rlight_t rlights[MAX_DLIGHTS];

/* the records lit this frame: their light bytes, to put back after it
   (fixed entries; a record is saved once a frame, the first time a light
   touches it) */
#define SAVE_ENTRY 52             /* the record, its vertex count, 44 bytes (<= 42 light bytes) */
#define SAVE_MAX   470            /* 24 KB */
uint8_t  *dl_save_buf;            /* (the asm hook shares these: r_world.s dl_block_asm) */
uint32_t  dl_save_n;
uint32_t *dl_touched;             /* a bit per record: saved this frame */
#define TOUCHED_WORDS 256         /* 8192 records */

/* sqrt of a squared distance, vertex units: entry i = sqrt(i << 11); the
   top entry is 1447 units (362 world units), past any light's reach */
uint16_t dl_sqrt[1024];

static uint32_t isqrt32(uint32_t x)
{
	uint32_t r = 0, bit = 1u << 30;
	while (bit > x) bit >>= 2;
	while (bit) {
		if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; }
		else r >>= 1;
		bit >>= 2;
	}
	return r;
}

void dl_init(void)
{
	dl_save_buf = mem_game_alloc(SAVE_ENTRY * SAVE_MAX);
	dl_touched  = mem_game_alloc(TOUCHED_WORDS * 4);
	for (int i = 0; i < 1024; i++)
		dl_sqrt[i] = (uint16_t) isqrt32((uint32_t) i << 11);
	r_state.lights  = (uint32_t) rlights;
	r_state.nlights = 0;
	dl_clear();
}

void dl_clear(void)
{
	memset(dlights, 0, sizeof dlights);
	dl_save_n = 0;
	r_state.nlights = 0;
}

dlight_t *dl_alloc(int key, int now)
{
	dlight_t *dl;
	if (key) {                               /* first look for an exact key match */
		dl = dlights;
		for (int i = 0; i < MAX_DLIGHTS; i++, dl++)
			if (dl->key == key) {
				memset(dl, 0, sizeof *dl);
				dl->key = key;
				return dl;
			}
	}
	dl = dlights;                            /* then look for anything else */
	for (int i = 0; i < MAX_DLIGHTS; i++, dl++)
		if (dl->die < now) {
			memset(dl, 0, sizeof *dl);
			dl->key = key;
			return dl;
		}
	dl = &dlights[0];
	memset(dl, 0, sizeof *dl);
	dl->key = key;
	return dl;
}

void dl_decay(int now)
{
	dlight_t *dl = dlights;
	for (int i = 0; i < MAX_DLIGHTS; i++, dl++) {
		if (dl->die < now || !dl->radius)
			continue;
		dl->radius -= dl->decay / 60;        /* per tick */
		if (dl->radius < 0)
			dl->radius = 0;
	}
}

/* ---------------------------------------------------------------- world -- */

/* the nodes whose box the light's sphere reaches, from the root (a box
   outside it ends its subtree) */
static void mark_nodes(qw_node *n, const rlight_t *l, uint16_t stamp)
{
	for (;;) {
		prof_count[PROF_N_DLNODES]++;
		int32_t dx = l->o[0] - n->c[0], dy = l->o[1] - n->c[1], dz = l->o[2] - n->c[2];
		if (dx < 0) dx = -dx;
		if (dy < 0) dy = -dy;
		if (dz < 0) dz = -dz;
		if (dx > n->h[0] + l->radius || dy > n->h[1] + l->radius || dz > n->h[2] + l->radius)
			return;
		n->dlstamp = stamp;
		uint32_t front = n->child[0], back = n->child[1];
		if (!(front & 1))
			mark_nodes((qw_node *) front, l, stamp);
		if (back & 1)
			return;
		n = (qw_node *) back;
	}
}

/* R_PushDlights: the live lights for the renderer, which lights each face
   block it draws (engine/r_world.s dr_lights -> dl_block) */
void dl_push(int now)
{
	dl_save_n = 0;
	int n = 0;
	for (int i = 0; i < MAX_DLIGHTS; i++) {
		const dlight_t *l = &dlights[i];
		if (l->die < now || !l->radius)
			continue;
		rlight_t *r = &rlights[n++];
		for (int k = 0; k < 3; k++)
			r->o[k] = l->origin[k] * QW_VSCALE / R_POS_SUB;
		r->radius   = l->radius * QW_VSCALE;
		r->minlight = l->minlight * QW_VSCALE;
		r->rmm      = r->radius - r->minlight;
	}
	r_state.nlights = (uint32_t) n;
	if (!n || !world.root)
		return;
	memset(dl_touched, 0, TOUCHED_WORDS * 4);
	/* the nodes the lights reach get the coming frame's count (r_frame
	   advances it first thing, with the same wrap): the walk tests it at
	   every face block */
	uint32_t stamp = r_state.framecount + 1;
	if (stamp >= 0xffff)
		stamp = 1;
	for (int i = 0; i < n; i++)
		mark_nodes(world.root, &rlights[i], (uint16_t) stamp);
}

/* The per-block table the walk's record loop reads (engine/r_world.s
   dr_light_rec; DLB_* in engine/r_defs.inc): the lights that reach the
   node's box, with their terms on the node's plane. Word 0 holds the
   count, the entries start at DLB_FIRST. */
#define DLB_FIRST 16
#define DLB_SIZE  48
uint32_t dl_blk[(DLB_FIRST + DLB_SIZE * MAX_DLIGHTS) / 4];

typedef struct {
	int32_t ix, iy, iz;       /* the light's impact point on the plane, vertex units */
	int32_t cutoff;           /* rad - minlight: a vertex nearer than this is lit  */
	int32_t minlight;
	int32_t ox, oy, oz;       /* the light's origin (the records' sphere test)    */
	int32_t rmm;              /* radius - minlight                                */
	int32_t pad[3];
} dlblk_t;                    /* 48 bytes */

/* The C reference of the block hook (R_DLIGHT_C in engine/r_defs.inc): the
   lights that reach a node (its box against each sphere) with their terms
   on the node's plane, as R_AddDynamicLights takes them for a surface:
   `rad = radius - |distance to the plane|`, nothing under the light's
   minlight, the impact point. Returns the table for the block's records,
   or 0 when no light reaches the node. */
uint32_t *dl_block_setup(const qw_node *node)
{
	int n = (int) r_state.nlights, m = 0;
	const qw_plane *pl = (const qw_plane *) ((const int32_t *) world.planes + node->plane3);
	dlblk_t *e = (dlblk_t *) ((uint8_t *) dl_blk + DLB_FIRST);
	for (int i = 0; i < n; i++) {
		const rlight_t *l = &rlights[i];
		int32_t dx = l->o[0] - node->c[0], dy = l->o[1] - node->c[1], dz = l->o[2] - node->c[2];
		if (dx < 0) dx = -dx;
		if (dy < 0) dy = -dy;
		if (dz < 0) dz = -dz;
		if (dx > node->h[0] + l->radius || dy > node->h[1] + l->radius || dz > node->h[2] + l->radius)
			continue;
		int32_t d = pl->type < 3
		          ? l->o[pl->type] * 4096 - (pl->dist >> 1)
		          : pl->n[0] * l->o[0] + pl->n[1] * l->o[1] + pl->n[2] * l->o[2] - (pl->dist >> 1);
		d >>= 12;                            /* vertex units */
		int rad = l->radius - (d < 0 ? -d : d);
		if (rad < l->minlight)
			continue;
		e[m].ix = l->o[0] - ((pl->n[0] * d) >> 12);
		e[m].iy = l->o[1] - ((pl->n[1] * d) >> 12);
		e[m].iz = l->o[2] - ((pl->n[2] * d) >> 12);
		e[m].cutoff   = rad - l->minlight;
		e[m].minlight = l->minlight;
		e[m].ox = l->o[0];
		e[m].oy = l->o[1];
		e[m].oz = l->o[2];
		e[m].rmm = l->rmm;
		m++;
	}
	dl_blk[0] = (uint32_t) m;
	return m ? dl_blk : 0;
}

/* The C reference of the record hook: a stamped record (about to be drawn;
   sky and liquid excepted) whose sphere a light of the block's table
   reaches has its light bytes saved once a frame (dl_touched, the
   dl_save_buf entries) and each vertex within `rad - minlight` of the
   impact point gets `rad - dist` x 3/8 (a quarter world unit per vertex
   unit, x 1.5 for the cook's light gain), saturating at 255; `dist` from
   dl_sqrt on the squared distance >> 11 (Quake's octagonal "sd + td / 2"
   approximated the same length). */
void dl_light_record(qw_record *q, const uint32_t *tab)
{
	if (q->flags & (QWF_SKY | QWF_LIQUID))
		return;                              /* unlit in Quake (no lightmap) */
	uint32_t id = q->face_id >> 1;           /* (the loader doubled it) */
	if (id >= TOUCHED_WORDS * 32)
		return;                              /* (more records than the bitmap: unlit) */
	int n = (int) tab[0];
	const dlblk_t *e = (const dlblk_t *) ((const uint8_t *) tab + DLB_FIRST);
	int nv = q->num_verts;
	const uint16_t *idx = (const uint16_t *) ((uint8_t *) q + sizeof *q);
	uint8_t *light = (uint8_t *) q + q->polys - ((nv + 3) & ~3);
	for (int i = 0; i < n; i++, e++) {
		/* the record's sphere against the light (Chebyshev: under the
		   real distance, never a wrong rejection) */
		int cx = q->sc[0] - e->ox, cy = q->sc[1] - e->oy, cz = q->sc[2] - e->oz;
		if (cx < 0) cx = -cx;
		if (cy < 0) cy = -cy;
		if (cz < 0) cz = -cz;
		int far = cx > cy ? cx : cy;
		if (cz > far) far = cz;
		if (far - q->sr >= e->rmm)
			continue;
		if (!(dl_touched[id >> 5] & (1u << (id & 31)))) {   /* saved once a frame */
			if (dl_save_n >= SAVE_MAX)
				return;                      /* the frame's budget: the rest stays unlit */
			dl_touched[id >> 5] |= 1u << (id & 31);
			uint32_t *h = (uint32_t *) (dl_save_buf + dl_save_n * SAVE_ENTRY);
			h[0] = (uint32_t) q;
			h[1] = (uint32_t) nv;
			memcpy(h + 2, light, (size_t) nv);
			dl_save_n++;
			prof_count[PROF_N_DLRECS]++;
			prof_count[PROF_N_DLVERTS] += (uint32_t) nv;
		}
		for (int v = 0; v < nv; v++) {
			const qw_vert *p = &world.verts[idx[v]];
			int32_t a = p->x - e->ix, b = p->y - e->iy, c = p->z - e->iz;
			uint32_t d2 = (uint32_t) (a * a + b * b + c * c) >> 11;
			if (d2 >= 1024)
				continue;
			int dist = dl_sqrt[d2];
			int add = e->cutoff - dist;      /* > 0: within rad - minlight */
			if (add <= 0)
				continue;
			add = ((add + e->minlight) * 3) >> 3;   /* (rad - dist) x 3/8 */
			int lv = light[v] + add;
			light[v] = (uint8_t) (lv > 255 ? 255 : lv);
		}
	}
}

void dl_restore(void)
{
	while (dl_save_n > 0) {
		dl_save_n--;
		uint32_t *h = (uint32_t *) (dl_save_buf + dl_save_n * SAVE_ENTRY);
		qw_record *rec = (qw_record *) h[0];
		uint32_t nv = h[1];
		uint8_t *light = (uint8_t *) rec + rec->polys - ((nv + 3) & ~3u);
		memcpy(light, h + 2, nv);
	}
}

/* ------------------------------------------------------------- entities -- */

int dl_add_at(const int32_t org[3], int now)
{
	int add = 0;
	for (int i = 0; i < MAX_DLIGHTS; i++) {
		const dlight_t *l = &dlights[i];
		if (l->die < now || !l->radius)
			continue;
		int32_t d[3];
		int32_t m = 0;
		for (int k = 0; k < 3; k++) {
			d[k] = (org[k] - l->origin[k]) / R_POS_SUB;   /* world units */
			m += d[k] < 0 ? -d[k] : d[k];
		}
		if (m >= l->radius * 2)
			continue;                        /* (the Manhattan distance bounds it) */
		int dist = (int) isqrt32((uint32_t) (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
		if (l->radius > dist)
			add += l->radius - dist;
	}
	return add;
}
