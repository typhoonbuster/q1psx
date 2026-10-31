#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dlight.h"
#include "mem.h"
#include "prof.h"
#include "q_fmt.h"
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

/* The lights' terms on the plane of the block being drawn, computed the
   first time a light lights one of its records and kept while the block
   lasts (engine/r_world.s dr_light_rec; DLB_* in engine/r_defs.inc): word
   0 the node whose terms these are, the entries from DLB_FIRST. */
#define DLB_FIRST 16
#define DLB_SIZE  48
uint32_t dl_blk[(DLB_FIRST + DLB_SIZE * MAX_DLIGHTS) / 4];

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

Q_COLD void dl_init(void)
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

void dl_decay(int now, int dt)
{
	for (int t = now - dt + 1; t <= now; t++) {   /* each 60th of the tick, as at 60 Hz */
		dlight_t *dl = dlights;
		for (int i = 0; i < MAX_DLIGHTS; i++, dl++) {
			if (dl->die < t || !dl->radius)
				continue;
			dl->radius -= dl->decay / 60;
			if (dl->radius < 0)
				dl->radius = 0;
		}
	}
}

/* ---------------------------------------------------------------- world -- */

#if Q1PSX_DLIGHT_C == 1
/* the C reference of r_render's descent (engine/r_world.s, after
   mark_done): the nodes whose box the light's reach (radius - minlight)
   touches, from the root (a box outside it ends its subtree, so does one
   out of the PVS: r_render calls this after mark_pvs), stamped when the
   reach straddles their plane too (R_MarkLights' rule: a node's faces lie
   on its plane, so a plane farther than the radius has none to light;
   farther than radius - minlight, R_AddDynamicLights lights none of
   them: a lit vertex lies within radius - minlight of the light). A plane
   farther than the reach + DL_PRUNE puts it whole on one side: only that
   child is descended (R_MarkLights again; the margin covers dl_sqrt's
   and the roundings' error, so no vertex the far side holds would have
   taken any light) */
static void mark_nodes(qw_node *n, const rlight_t *l, uint16_t stamp, uint16_t visframe)
{
	for (;;) {
		if (n->visframe != visframe)
			return;                          /* nothing visible below */
		prof_count[PROF_N_DLNODES]++;
		int32_t dx = l->o[0] - n->c[0], dy = l->o[1] - n->c[1], dz = l->o[2] - n->c[2];
		if (dx < 0) dx = -dx;
		if (dy < 0) dy = -dy;
		if (dz < 0) dz = -dz;
		if (dx > n->h[0] + l->rmm || dy > n->h[1] + l->rmm || dz > n->h[2] + l->rmm)
			return;
		const qw_plane *pl = (const qw_plane *) ((const int32_t *) world.planes + n->plane3);
		int32_t d = pl->type < 3
		          ? l->o[pl->type] * 4096 - (pl->dist >> 1)
		          : pl->n[0] * l->o[0] + pl->n[1] * l->o[1] + pl->n[2] * l->o[2] - (pl->dist >> 1);
		d >>= 12;                            /* vertex units */
		uint32_t front = n->child[0], back = n->child[1];
		if (d > l->rmm + DL_PRUNE || d < -(l->rmm + DL_PRUNE)) {
			uint32_t c = d > 0 ? front : back;   /* R_MarkLights: the light's side only */
			if (c & 1)
				return;
			n = (qw_node *) c;
			continue;
		}
		if (d < 0) d = -d;
		if (d <= l->rmm)
			n->dlstamp = stamp;
		if (!(front & 1))
			mark_nodes((qw_node *) front, l, stamp, visframe);
		if (back & 1)
			return;
		n = (qw_node *) back;
	}
}

/* r_render, after the PVS marking and before the walk (engine/r_world.s
   dl_stamp): the frame's count is already advanced */
void dl_stamp_nodes(void)
{
	if (!r_state.nlights || !world.root)
		return;
	for (uint32_t i = 0; i < r_state.nlights; i++)
		mark_nodes(world.root, &rlights[i], (uint16_t) r_state.framecount, (uint16_t) r_state.visframe);
}
#endif

/* R_PushDlights: the live lights for the renderer, which lights each face
   block it draws (engine/r_world.s dr_lights -> dl_block) */
int dl_enabled = 1;

void dl_push(int now)
{
	dl_save_n = 0;
	int n = 0;
	for (int i = 0; i < MAX_DLIGHTS && dl_enabled; i++) {
		const dlight_t *l = &dlights[i];
		int minlight = l->minlight > Q1PSX_DLIGHT_MINLIGHT ? l->minlight : Q1PSX_DLIGHT_MINLIGHT;
		if (l->die < now || l->radius <= minlight)
			continue;                        /* (none of the world reaches its minlight) */
		rlight_t *r = &rlights[n++];
		for (int k = 0; k < 3; k++)
			r->o[k] = l->origin[k] * QW_VSCALE / R_POS_SUB;
		r->radius   = l->radius * QW_VSCALE;
		r->minlight = minlight * QW_VSCALE;
		r->rmm      = r->radius - r->minlight;
	}
	r_state.nlights = (uint32_t) n;
	dl_blk[0] = 0;                           /* (the block cache: no block yet) */
	if (n)
		memset(dl_touched, 0, TOUCHED_WORDS * 4);
}

typedef struct {
	int32_t ix, iy, iz;       /* the light's impact point on the plane, vertex units */
	int32_t cutoff;           /* rad - minlight: a vertex nearer than this is lit;
	                             < 0: the light does not reach the plane        */
	int32_t minlight;
	int32_t pad[4];
	int32_t valid;            /* +36: computed for this block                      */
} dlblk_t;                    /* 48 bytes */

/* The C reference of the record hook (R_DLIGHT_C in engine/r_defs.inc):
   a stamped record (about to be drawn; sky and liquid excepted) of a
   node a light reaches. For each live light whose sphere reaches the
   record's (the Chebyshev distance of the centre, less the radius, under
   radius - minlight), the light's terms on the node's plane, as
   R_AddDynamicLights takes them for a surface: `rad = radius - |distance
   to the plane|`, nothing under the light's minlight, the impact point
   (cached per block); then the light bytes saved once a frame
   (dl_touched, a dl_save_buf entry) and each vertex within `rad -
   minlight` of the impact point gets `rad - dist` x 53/128 (a quarter world
   unit per vertex unit, x 1.656 for the cook's light gain: DL_GAIN), saturating at
   255; `dist` from dl_sqrt on the squared distance >> 11 (Quake's
   octagonal "sd + td / 2" approximated the same length). */
#if Q1PSX_DLIGHT_C != 0
void dl_light_record(qw_record *q, const qw_node *node)
{
	if (q->flags & (QWF_SKY | QWF_LIQUID))
		return;                              /* unlit in Quake (no lightmap) */
	uint32_t id = q->face_id >> 1;           /* (the loader doubled it) */
	if (id >= TOUCHED_WORDS * 32)
		return;                              /* (more records than the bitmap: unlit) */
	int n = (int) r_state.nlights;
	dlblk_t *tab = (dlblk_t *) ((uint8_t *) dl_blk + DLB_FIRST);
	if (dl_blk[0] != (uint32_t) node) {      /* another block: its terms are stale */
		dl_blk[0] = (uint32_t) node;
		for (int i = 0; i < n; i++)
			tab[i].valid = 0;
	}
	int nv = q->num_verts;
	const uint16_t *idx = (const uint16_t *) ((uint8_t *) q + sizeof *q);
	const qw_vert *pool = (const qw_vert *) r_state.vertpool;   /* (the world's records': a
	                                         zoned map's, the current chunk's, engine/zone.c) */
	uint8_t *light = (uint8_t *) q + q->polys - ((nv + 3) & ~3);
	for (int i = 0; i < n; i++) {
		const rlight_t *l = &rlights[i];
		/* the record's sphere against the light (Chebyshev: under the
		   real distance, never a wrong rejection) */
		int cx = q->sc[0] - l->o[0], cy = q->sc[1] - l->o[1], cz = q->sc[2] - l->o[2];
		if (cx < 0) cx = -cx;
		if (cy < 0) cy = -cy;
		if (cz < 0) cz = -cz;
		int far = cx > cy ? cx : cy;
		if (cz > far) far = cz;
		if (far - q->sr >= l->rmm)
			continue;
		dlblk_t *e = &tab[i];
		if (!e->valid) {                     /* this light on this block's plane */
			const qw_plane *pl = (const qw_plane *) ((const int32_t *) world.planes + node->plane3);
			int32_t d = pl->type < 3
			          ? l->o[pl->type] * 4096 - (pl->dist >> 1)
			          : pl->n[0] * l->o[0] + pl->n[1] * l->o[1] + pl->n[2] * l->o[2] - (pl->dist >> 1);
			d >>= 12;                        /* vertex units */
			int rad = l->radius - (d < 0 ? -d : d);
			e->cutoff   = rad - l->minlight; /* (< 0 under minlight) */
			e->minlight = l->minlight;
			e->ix = l->o[0] - ((pl->n[0] * d) >> 12);
			e->iy = l->o[1] - ((pl->n[1] * d) >> 12);
			e->iz = l->o[2] - ((pl->n[2] * d) >> 12);
			e->valid = 1;
		}
		if (e->cutoff < 0)
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
			const qw_vert *p = &pool[idx[v]];
			int32_t a = p->x - e->ix, b = p->y - e->iy, c = p->z - e->iz;
			uint32_t d2 = (uint32_t) (a * a + b * b + c * c) >> 11;
			if (d2 >= 1024)
				continue;
			int dist = dl_sqrt[d2];
			int add = e->cutoff - dist;      /* > 0: within rad - minlight */
			if (add <= 0)
				continue;
			add = ((add + e->minlight) * 53) >> 7;   /* (rad - dist) x 53/128 */
			int lv = light[v] + add;
			light[v] = (uint8_t) (lv > 255 ? 255 : lv);
		}
	}
}

/* The asm hook's checker (R_DLIGHT_C = 2 builds, engine/r_world.s
   dr_light_full): after dl_light_record_asm lit a record, recompute what
   the C reference would have written from the originals in the record's
   save entry, and print the first vertices that differ. */
int dl_check_count;
void dl_check_record(qw_record *q, const qw_node *node)
{
	uint32_t id = q->face_id >> 1;
	if ((q->flags & (QWF_SKY | QWF_LIQUID)) || id >= TOUCHED_WORDS * 32)
		return;
	int nv = q->num_verts;
	const uint16_t *idx = (const uint16_t *) ((uint8_t *) q + sizeof *q);
	uint8_t *light = (uint8_t *) q + q->polys - ((nv + 3) & ~3);
	/* the originals: the save entry of this record, else the bytes as they
	   are (the asm did not light it) */
	const uint8_t *orig = 0;
	for (uint32_t k = 0; k < dl_save_n; k++) {
		const uint32_t *h = (const uint32_t *) (dl_save_buf + k * SAVE_ENTRY);
		if (h[0] == (uint32_t) q) { orig = (const uint8_t *) (h + 2); break; }
	}
	int expect[42];
	for (int v = 0; v < nv; v++) expect[v] = orig ? orig[v] : light[v];
	int n = (int) r_state.nlights, lit = 0;
	const qw_plane *pl = (const qw_plane *) ((const int32_t *) world.planes + node->plane3);
	char why[160] = "";
	for (int i = 0; i < n; i++) {
		const rlight_t *l = &rlights[i];
		int cx = q->sc[0] - l->o[0], cy = q->sc[1] - l->o[1], cz = q->sc[2] - l->o[2];
		if (cx < 0) cx = -cx;
		if (cy < 0) cy = -cy;
		if (cz < 0) cz = -cz;
		int far = cx > cy ? cx : cy;
		if (cz > far) far = cz;
		if (far - q->sr >= l->rmm) {
			q_snprintf(why + strlen(why), sizeof why - strlen(why), " L%d far-sr %d >= rmm %d;", i, far - q->sr, l->rmm);
			continue;
		}
		int32_t d = pl->type < 3
		          ? l->o[pl->type] * 4096 - (pl->dist >> 1)
		          : pl->n[0] * l->o[0] + pl->n[1] * l->o[1] + pl->n[2] * l->o[2] - (pl->dist >> 1);
		d >>= 12;
		int rad = l->radius - (d < 0 ? -d : d);
		int cutoff = rad - l->minlight;
		q_snprintf(why + strlen(why), sizeof why - strlen(why), " L%d type %d d %d cutoff %d;", i, pl->type, (int) d, cutoff);
		if (cutoff < 0)
			continue;
		lit = 1;
		int32_t ix = l->o[0] - ((pl->n[0] * d) >> 12), iy = l->o[1] - ((pl->n[1] * d) >> 12), iz = l->o[2] - ((pl->n[2] * d) >> 12);
		for (int v = 0; v < nv; v++) {
			const qw_vert *p = &((const qw_vert *) r_state.vertpool)[idx[v]];
			int32_t a = p->x - ix, b = p->y - iy, c = p->z - iz;
			uint32_t d2 = (uint32_t) (a * a + b * b + c * c) >> 11;
			if (d2 >= 1024) continue;
			int add = cutoff - dl_sqrt[d2];
			if (add <= 0) continue;
			add = ((add + l->minlight) * 53) >> 7;
			expect[v] += add;
			if (expect[v] > 255) expect[v] = 255;
		}
	}
	for (int v = 0; v < nv; v++)
		if (light[v] != expect[v]) {
			if (dl_check_count < 12) {
				dl_check_count++;
				printf("dlchk rec %u v%d: asm %u, C %d (orig %s%u) nv %d node %08x lit %d:%s\n", (unsigned) id, v,
				       light[v], expect[v], orig ? "" : "unsaved ", orig ? orig[v] : light[v], nv, (unsigned) node, lit, why);
			}
			break;
		}
}

#endif

#if Q1PSX_DLIGHT_C == 1
/* the C reference of r_world.s dl_restore_asm (the asm puts the padded
   words back: the same bytes where the renderer reads) */
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
#endif

/* ------------------------------------------------------------- entities -- */

int dl_add_at(const int32_t org[3], int now)
{
	int add = 0;
	for (int i = 0; i < MAX_DLIGHTS && dl_enabled; i++) {
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
		uint32_t d2 = (uint32_t) (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		int dist;
		if (d2 < 1024u << 7) {               /* the world's table (vertex units, x 4 these),
			                                    then exact: isqrt32's */
			dist = dl_sqrt[d2 >> 7] >> 2;
			while ((uint32_t) (dist + 1) * (uint32_t) (dist + 1) <= d2)
				dist++;
		} else {
			dist = (int) isqrt32(d2);
		}
		if (l->radius > dist)
			add += l->radius - dist;
	}
	return add;
}
