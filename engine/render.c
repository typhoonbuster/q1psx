#include <stdint.h>
#include <string.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>

#include "gte_fix.h"   /* must follow <inline_c.h> */
#include "vid.h"
#include "mathlib.h"
#include "surf.h"
#include "tex.h"
#include "qtx.h"
#include "bsp.h"
#include "render.h"

#define R_MAXVERTS 12288
#define R_MAXLINES 2500   /* per-frame line budget (no culling yet)          */
#define R_MAXFACES 600    /* per-frame face budget for the un-culled path    */
#define R_MAXWORLD 900    /* per-frame face budget for the PVS-culled path   */
#define R_MAXFACEIDX 8192 /* > any map's world-face count (MAX_MAP_FACES-ish) */
#define R_FAN_MAX  22     /* largest cooked face polygon */
#define NEAR_Z     16     /* skip vertices closer than this along view Z */
#define NEAR_CLIP  20     /* view-space plane the world polygons are clipped to */
#define CLIPBUF    (R_FAN_MAX + 6)

static int16_t scr_x[R_MAXVERTS];
static int16_t scr_y[R_MAXVERTS];
static uint8_t scr_ok[R_MAXVERTS];

/* view matrix stashed by r_begin_view, for the near-plane clip's view-space Z */
static int32_t vm_row2[3];   /* forward basis, 1.3.12 */
static int32_t vm_t2;        /* forward translation, world units */
static int32_t cam_pos[3];   /* eye, for 3D back-face culling    */

/* --- diagnostics (declared in render.h, shown in the overlay) --- */
int r_dbg_nodes, r_dbg_leaves, r_dbg_faces;
int r_dbg_drop_near, r_dbg_drop_back, r_dbg_drop_sky, r_dbg_drop_slot;
int r_dbg_vz_lo, r_dbg_vz_hi;   /* view_z range this frame */
int r_dbg_vm[4];               /* vm_row2[3], vm_t2       */
int r_dbg_us_walk, r_dbg_us_draw;  /* approx microseconds: traversal, draw_face */
static uint32_t draw_us_acc;

void r_init(void)
{
	InitGeom();
	gte_SetGeomOffset(VID_W / 2, VID_H / 2);
	gte_SetGeomScreen(VID_W / 2);   /* ~90 deg FOV */
	gte_SetBackColor(63, 63, 63);
}

void r_begin_view(const int32_t pos[3], const vec3_t ang)
{
	vec3_t fwd, right, up;
	AngleVectors(ang, fwd, right, up);

	MATRIX m;
	/* rows = camera basis; screen Y points down so row 1 is -up */
	m.m[0][0] = (int16_t) right[0];
	m.m[0][1] = (int16_t) right[1];
	m.m[0][2] = (int16_t) right[2];
	m.m[1][0] = (int16_t) -up[0];
	m.m[1][1] = (int16_t) -up[1];
	m.m[1][2] = (int16_t) -up[2];
	m.m[2][0] = (int16_t) fwd[0];
	m.m[2][1] = (int16_t) fwd[1];
	m.m[2][2] = (int16_t) fwd[2];

	/* t = -M * campos */
	for (int i = 0; i < 3; i++) {
		int64_t d = (int64_t) m.m[i][0] * pos[0]
		          + (int64_t) m.m[i][1] * pos[1]
		          + (int64_t) m.m[i][2] * pos[2];
		m.t[i] = (int32_t) (-(d >> Q_FIX_SHIFT));
	}

	/* Stash the forward row for view_z BEFORE the GTE macros run: PSn00bSDK's
	   gte_SetRotMatrix / gte_SetTransMatrix have undeclared $t0/$t1 clobbers,
	   so a value the compiler keeps live across them (like &m) can be trashed,
	   giving a garbage read afterwards. */
	vm_row2[0] = m.m[2][0];
	vm_row2[1] = m.m[2][1];
	vm_row2[2] = m.m[2][2];
	vm_t2      = m.t[2];
	cam_pos[0] = pos[0];
	cam_pos[1] = pos[1];
	cam_pos[2] = pos[2];
	r_dbg_vm[0] = vm_row2[0];
	r_dbg_vm[1] = vm_row2[1];
	r_dbg_vm[2] = vm_row2[2];
	r_dbg_vm[3] = vm_t2;

	gte_SetRotMatrix(&m);
	gte_SetTransMatrix(&m);
}

/* view-space Z (distance along the forward axis) of a world-space vertex.
   int32 only: row ~x4096, coords <= ~4096 -> <= 5e7 per term, fits. */
static int32_t view_z(const SVECTOR *v)
{
	int32_t d = vm_row2[0] * (int32_t) v->vx
	          + vm_row2[1] * (int32_t) v->vy
	          + vm_row2[2] * (int32_t) v->vz;
	return (d >> Q_FIX_SHIFT) + vm_t2;
}

int r_wireframe(const SVECTOR *verts, int nverts,
                const uint16_t *edges, int nedges,
                int r, int g, int b)
{
	if (nverts > R_MAXVERTS)
		nverts = R_MAXVERTS;

	for (int i = 0; i < nverts; i++) {
		gte_ldv0(&verts[i]);
		gte_rtps();

		int32_t sz;
		gte_stsz(&sz);

		uint32_t xy;
		gte_stsxy(&xy);

		scr_ok[i] = (sz > NEAR_Z);
		scr_x[i]  = (int16_t) (xy & 0xffff);
		scr_y[i]  = (int16_t) (xy >> 16);
	}

	int drawn = 0;
	for (int e = 0; e < nedges && drawn < R_MAXLINES; e++) {
		int a  = edges[e * 2 + 0];
		int b2 = edges[e * 2 + 1];
		if (a >= nverts || b2 >= nverts || !scr_ok[a] || !scr_ok[b2])
			continue;

		LINE_F2 *ln = vid_alloc(4, sizeof(LINE_F2));
		setLineF2(ln);
		setRGB0(ln, r, g, b);
		setXY2(ln, scr_x[a], scr_y[a], scr_x[b2], scr_y[b2]);
		drawn++;
	}
	return drawn;
}

/* ---------------------------------------------------- textured surfaces --- */

/* lazily-filled per-vertex projection cache, indexed by the surf vertex pool */
static int16_t sv_x[R_MAXVERTS], sv_y[R_MAXVERTS];
static uint8_t sv_state[R_MAXVERTS];   /* 0 = not done, 1 = ok, 2 = near/behind */
static int     sv_nv;                  /* valid vertex count for this frame   */

static void project(const SVECTOR *verts, int i)
{
	gte_ldv0(&verts[i]);
	gte_rtps();
	int32_t sz;
	uint32_t xy;
	gte_stsz(&sz);
	gte_stsxy(&xy);
	sv_x[i]     = (int16_t) (xy & 0xffff);
	sv_y[i]     = (int16_t) (xy >> 16);
	sv_state[i] = (sz > NEAR_Z) ? 1 : 2;
	if (sz < r_dbg_vz_lo) r_dbg_vz_lo = sz;
	if (sz > r_dbg_vz_hi) r_dbg_vz_hi = sz;
}

/* ---- runtime adaptive subdivision -------------------------------------- *
 * The PS1 GPU has no perspective-correct texturing — a big triangle near the
 * camera warps ("Doom / Duke3D stretch"). Fix: split each triangle on its
 * longest edge (3D midpoint, so it *is* a perspective correction) until every
 * piece is small on screen. Only large near triangles pay for this. */

#define SUBDIV_PX   40   /* split a triangle whose screen bbox exceeds this  */
#define SUBDIV_MAX  3    /* recursion cap                                    */

typedef struct {
	SVECTOR w;            /* world position                    */
	int16_t sx, sy;       /* screen coords (valid once scr==1) */
	uint8_t u, v;
	uint8_t scr;
} wvert;

static const tex_slot_t *cur_slot;

static void wv_project(wvert *p)
{
	if (p->scr)
		return;
	gte_ldv0(&p->w);
	gte_rtps();
	uint32_t xy;
	gte_stsxy(&xy);
	p->sx  = (int16_t) (xy & 0xffff);
	p->sy  = (int16_t) (xy >> 16);
	p->scr = 1;
}

static void wv_mid(const wvert *a, const wvert *b, wvert *m)
{
	m->w.vx  = (a->w.vx + b->w.vx) >> 1;
	m->w.vy  = (a->w.vy + b->w.vy) >> 1;
	m->w.vz  = (a->w.vz + b->w.vz) >> 1;
	m->w.pad = 0;
	m->u     = (uint8_t) (((int) a->u + b->u) >> 1);
	m->v     = (uint8_t) (((int) a->v + b->v) >> 1);
	m->scr   = 0;
}

static void emit_ft3(const wvert *a, const wvert *b, const wvert *c)
{
	POLY_FT3 *t = vid_alloc_raw(sizeof(POLY_FT3));
	setPolyFT3(t);
	setRGB0(t, 128, 128, 128);
	setXY3(t, a->sx, a->sy, b->sx, b->sy, c->sx, c->sy);
	setUV3(t, a->u, a->v, b->u, b->v, c->u, c->v);
	t->tpage = cur_slot->tpage;
	t->clut  = cur_slot->clut;
	vid_link(4, t);
}

static void emit_tri(wvert *a, wvert *b, wvert *c, int depth)
{
	wv_project(a);
	wv_project(b);
	wv_project(c);

	int minx = a->sx, maxx = a->sx, miny = a->sy, maxy = a->sy;
	if (b->sx < minx) minx = b->sx;  if (b->sx > maxx) maxx = b->sx;
	if (c->sx < minx) minx = c->sx;  if (c->sx > maxx) maxx = c->sx;
	if (b->sy < miny) miny = b->sy;  if (b->sy > maxy) maxy = b->sy;
	if (c->sy < miny) miny = c->sy;  if (c->sy > maxy) maxy = c->sy;

	if (depth >= SUBDIV_MAX
	 || (maxx - minx < SUBDIV_PX && maxy - miny < SUBDIV_PX)) {
		emit_ft3(a, b, c);
		return;
	}

	int dab = (b->sx - a->sx) * (b->sx - a->sx) + (b->sy - a->sy) * (b->sy - a->sy);
	int dbc = (c->sx - b->sx) * (c->sx - b->sx) + (c->sy - b->sy) * (c->sy - b->sy);
	int dca = (a->sx - c->sx) * (a->sx - c->sx) + (a->sy - c->sy) * (a->sy - c->sy);

	wvert m;
	if (dab >= dbc && dab >= dca) {
		wv_mid(a, b, &m);
		emit_tri(a, &m, c, depth + 1);
		emit_tri(&m, b, c, depth + 1);
	} else if (dbc >= dca) {
		wv_mid(b, c, &m);
		emit_tri(a, b, &m, depth + 1);
		emit_tri(a, &m, c, depth + 1);
	} else {
		wv_mid(c, a, &m);
		emit_tri(a, b, &m, depth + 1);
		emit_tri(&m, b, c, depth + 1);
	}
}

/* ---------------------------------------------------------------------- */

static wvert dpoly[CLIPBUF];

/* Back-face cull, near-plane clip, fan-triangulate with adaptive subdivision,
   one DR_TWIN. Returns 1 if it emitted, else 0. */
static int draw_face(const surf_t *s, int f)
{
	const qsrf_face     *face = &s->faces[f];
	const qsrf_facevert *fv   = &s->fv[face->first_fv];
	int n = face->num_fv;
	if (n < 3 || n > R_FAN_MAX)
		return 0;

	/* 3D back-face cull: the camera sees the front iff it is on the positive
	   side of the cooked (outward-oriented) face plane. */
	if ((int32_t) face->n[0] * cam_pos[0]
	  + (int32_t) face->n[1] * cam_pos[1]
	  + (int32_t) face->n[2] * cam_pos[2] - face->dist <= 0) {
		r_dbg_drop_back++;
		return 0;
	}

	/* project every vertex once (cached), count how many are behind the near
	   plane (the GTE's SZ says which side) */
	int nbehind = 0;
	for (int k = 0; k < n; k++) {
		int vi = fv[k].vert;
		if (vi >= sv_nv)
			return 0;
		if (!sv_state[vi])
			project(s->verts, vi);
		if (sv_state[vi] == 2)
			nbehind++;
	}
	if (nbehind == n) {
		r_dbg_drop_near++;
		return 0;
	}

	const tex_slot_t *slot = qtx_slot_ptr(face->tex);
	if (!slot) {
		r_dbg_drop_slot++;
		return 0;
	}
	if (slot->flags & QTX_F_SKY) {
		r_dbg_drop_sky++;
		return 0;
	}
	cur_slot = slot;

	/* --- fast path: whole face in front, and small enough on screen that the
	   affine warp is imperceptible — emit the fan as POLY_FT4 quads, no
	   per-vertex world copy, no subdivision. Most faces take this path. */
	if (nbehind == 0) {
		int minx = 32767, maxx = -32768, miny = 32767, maxy = -32768;
		for (int k = 0; k < n; k++) {
			int vi = fv[k].vert, x = sv_x[vi], y = sv_y[vi];
			if (x < minx) minx = x;  if (x > maxx) maxx = x;
			if (y < miny) miny = y;  if (y > maxy) maxy = y;
		}
		if (maxx - minx < SUBDIV_PX && maxy - miny < SUBDIV_PX) {
			uint16_t tp = slot->tpage, cl = slot->clut;
			for (int k = 1; k + 1 < n; k += 2) {
				int a = fv[0].vert;
				if (k + 2 < n) {
					int b = fv[k].vert, c = fv[k + 1].vert, d = fv[k + 2].vert;
					POLY_FT4 *t = vid_alloc_raw(sizeof(POLY_FT4));
					setPolyFT4(t);
					setRGB0(t, 128, 128, 128);
					setXY4(t, sv_x[b], sv_y[b], sv_x[a], sv_y[a],
					          sv_x[c], sv_y[c], sv_x[d], sv_y[d]);
					setUV4(t, fv[k].u, fv[k].v, fv[0].u, fv[0].v,
					          fv[k + 1].u, fv[k + 1].v, fv[k + 2].u, fv[k + 2].v);
					t->tpage = tp;
					t->clut  = cl;
					vid_link(4, t);
				} else {
					int b = fv[k].vert, c = fv[k + 1].vert;
					POLY_FT3 *t = vid_alloc_raw(sizeof(POLY_FT3));
					setPolyFT3(t);
					setRGB0(t, 128, 128, 128);
					setXY3(t, sv_x[a], sv_y[a], sv_x[b], sv_y[b], sv_x[c], sv_y[c]);
					setUV3(t, fv[0].u, fv[0].v, fv[k].u, fv[k].v,
					          fv[k + 1].u, fv[k + 1].v);
					t->tpage = tp;
					t->clut  = cl;
					vid_link(4, t);
				}
			}
			DR_TWIN *tw = vid_alloc_raw(sizeof(DR_TWIN));
			setTexWindow(tw, &slot->twin);
			vid_link(4, tw);
			return 1;
		}
	}

	/* --- slow path: build a working polygon (world + UV), near-clip if any
	   vertex is behind, then fan-triangulate with subdivision. */
	int np = 0;
	if (nbehind == 0) {
		for (int k = 0; k < n; k++) {
			int vi = fv[k].vert;
			dpoly[np].w   = s->verts[vi];
			dpoly[np].sx  = sv_x[vi];
			dpoly[np].sy  = sv_y[vi];
			dpoly[np].scr = 1;
			dpoly[np].u   = fv[k].u;
			dpoly[np].v   = fv[k].v;
			np++;
		}
	} else {
		int32_t vz[R_FAN_MAX];
		for (int k = 0; k < n; k++)
			vz[k] = view_z(&s->verts[fv[k].vert]);
		for (int k = 0; k < n && np < CLIPBUF - 1; k++) {
			int a = k, b = (k + 1) % n;
			int af = vz[a] >= NEAR_CLIP, bf = vz[b] >= NEAR_CLIP;
			const SVECTOR *wa = &s->verts[fv[a].vert];
			const SVECTOR *wb = &s->verts[fv[b].vert];
			if (af) {
				dpoly[np].w   = *wa;
				dpoly[np].u   = fv[a].u;
				dpoly[np].v   = fv[a].v;
				dpoly[np].scr = 0;
				np++;
			}
			if (af != bf) {
				int32_t denom = vz[b] - vz[a];
				if (!denom)
					continue;
				int32_t t = (int32_t) (((int64_t) (NEAR_CLIP - vz[a]) << Q_FIX_SHIFT) / denom);
				if (t < 0) t = 0;
				if (t > Q_FIX_ONE) t = Q_FIX_ONE;
				dpoly[np].w.vx  = (int16_t) (wa->vx + (((wb->vx - wa->vx) * t) >> Q_FIX_SHIFT));
				dpoly[np].w.vy  = (int16_t) (wa->vy + (((wb->vy - wa->vy) * t) >> Q_FIX_SHIFT));
				dpoly[np].w.vz  = (int16_t) (wa->vz + (((wb->vz - wa->vz) * t) >> Q_FIX_SHIFT));
				dpoly[np].w.pad = 0;
				dpoly[np].u     = (uint8_t) (fv[a].u + (((fv[b].u - fv[a].u) * t) >> Q_FIX_SHIFT));
				dpoly[np].v     = (uint8_t) (fv[a].v + (((fv[b].v - fv[a].v) * t) >> Q_FIX_SHIFT));
				dpoly[np].scr   = 0;
				np++;
			}
		}
	}

	if (np < 3)
		return 0;

	for (int k = 1; k + 1 < np; k++)
		emit_tri(&dpoly[0], &dpoly[k], &dpoly[k + 1], 0);

	DR_TWIN *tw = vid_alloc_raw(sizeof(DR_TWIN));
	setTexWindow(tw, &slot->twin);
	vid_link(4, tw);
	return 1;
}

/* Every per-face DR_TWIN leaves the GPU texture window set; whatever runs after
   the surfaces on the OT (the debug font, in a lower bucket) would inherit it
   and tile its texture wrongly. Emit a full-page window that, added before the
   faces, is the *last* thing executed in the bucket — restoring the default. */
static void reset_texwindow(void)
{
	RECT     full = { 0, 0, 0, 0 };
	DR_TWIN *tw   = vid_alloc_raw(sizeof(DR_TWIN));
	setTexWindow(tw, &full);
	vid_link(4, tw);
}

/* Draw every cooked face (no visibility culling) — the SELECT fallback view. */
int r_surfaces(const surf_t *s)
{
	sv_nv = s->num_verts < R_MAXVERTS ? s->num_verts : R_MAXVERTS;
	memset(sv_state, 0, sv_nv);
	reset_texwindow();

	int drawn = 0;
	for (int f = 0; f < s->num_faces && drawn < R_MAXFACES; f++)
		drawn += draw_face(s, f);
	return drawn;
}

/* ------------------------------------------------------- world (PVS + frustum) */

#define R_MAXNODEIDX 4096   /* > any cooked map's node/leaf count */
#define CLIP_ALL     0x1f
#define BOX_CLAMP    24000  /* clamp box-corner offsets so box_visible fits int32 */

/* world-space inward frustum normals (side planes ~x28672, near ~x4096),
   rebuilt each r_world call. 0 right, 1 left, 2 bottom, 3 top, 4 near. */
static int32_t fr_n[5][3];

static void frustum_setup(const vec3_t rt, const vec3_t up, const vec3_t fw)
{
	for (int k = 0; k < 3; k++) {
		fr_n[0][k] = -rt[k] + fw[k];             /* right edge  */
		fr_n[1][k] =  rt[k] + fw[k];             /* left edge   */
		fr_n[2][k] =  4 * up[k] + 3 * fw[k];     /* bottom edge */
		fr_n[3][k] = -4 * up[k] + 3 * fw[k];     /* top edge    */
		fr_n[4][k] =  fw[k];                     /* near        */
	}
}

static int32_t clampb(int32_t v)
{
	return v > BOX_CLAMP ? BOX_CLAMP : v < -BOX_CLAMP ? -BOX_CLAMP : v;
}

/* 1 = box may be visible (*clip updated: a cleared bit = trivially inside that
   plane, children can skip it). 0 = box is wholly outside some plane. */
static int box_visible(const int16_t mn[3], const int16_t mx[3],
                       const int32_t pos[3], int *clip)
{
	int c = *clip;
	for (int i = 0; i < 5; i++) {
		if (!(c & (1 << i)))
			continue;
		const int32_t *n = fr_n[i];
		int32_t vmax = 0, vmin = 0;
		for (int k = 0; k < 3; k++) {
			int32_t hi = clampb((n[k] >= 0 ? mx[k] : mn[k]) - pos[k]);
			int32_t lo = clampb((n[k] >= 0 ? mn[k] : mx[k]) - pos[k]);
			vmax += n[k] * hi;
			vmin += n[k] * lo;
		}
		int32_t bias = (i == 4) ? (NEAR_Z << Q_FIX_SHIFT) : 0;
		if (vmax < bias)
			return 0;
		if (vmin >= bias)
			c &= ~(1 << i);
	}
	*clip = c;
	return 1;
}

/* --- BSP node marking (Quake R_MarkLeaves): mark every node on a path from a
   PVS-visible leaf up to the root, so world_node can prune whole invisible
   subtrees instead of frustum-testing the entire tree. */
static int16_t       np_parent[R_MAXNODEIDX];   /* node -> parent node, -1 = root */
static int16_t       lp_parent[R_MAXNODEIDX];   /* leaf -> parent node            */
static uint16_t      nmark[R_MAXNODEIDX];
static uint16_t      nmark_key;
static const bsp_t  *parent_bsp;                /* which bsp np/lp were built for */
static int           marking_on;

static void set_parent(const bsp_t *b, int idx, int parent)
{
	if (idx < 0) {
		int leaf = -idx - 1;
		if (leaf < R_MAXNODEIDX)
			lp_parent[leaf] = (int16_t) parent;
		return;
	}
	if (idx >= R_MAXNODEIDX)
		return;
	np_parent[idx] = (int16_t) parent;
	const qbsp_node *nd = &b->nodes[idx];
	set_parent(b, nd->child[0], idx);
	set_parent(b, nd->child[1], idx);
}

static void build_parents(const bsp_t *b)
{
	if (parent_bsp == b)
		return;
	parent_bsp = b;
	marking_on = (b->num_nodes <= R_MAXNODEIDX && b->num_leaves <= R_MAXNODEIDX);
	if (!marking_on)
		return;
	set_parent(b, b->root, -1);
	memset(nmark, 0, sizeof nmark);
	nmark_key = 0;
}

static void mark_pvs_leaves(const bsp_t *b, const uint8_t *pvs)
{
	if (!marking_on)
		return;
	if (++nmark_key == 0) {
		memset(nmark, 0, sizeof nmark);
		nmark_key = 1;
	}
	for (int i = 0; i < b->num_visleaves; i++) {
		if (!(pvs[i >> 3] & (1 << (i & 7))))
			continue;
		int n = lp_parent[i + 1];
		while (n >= 0 && nmark[n] != nmark_key) {
			nmark[n] = nmark_key;
			n = np_parent[n];
		}
	}
}

static uint8_t face_seen[R_MAXFACEIDX];
static uint8_t face_key;

static void world_node(const surf_t *s, const bsp_t *b, int nodeidx,
                       const int32_t pos[3], const uint8_t *pvs,
                       int clip, int *drawn)
{
	while (nodeidx >= 0) {
		if (marking_on && nmark[nodeidx] != nmark_key)
			return;
		r_dbg_nodes++;
		const qbsp_node *nd = &b->nodes[nodeidx];
		int c = clip;
		if (!box_visible(nd->mins, nd->maxs, pos, &c))
			return;
		int side = bsp_plane_side(&b->planes[nd->plane], pos);
		world_node(s, b, nd->child[side], pos, pvs, c, drawn);
		clip    = c;
		nodeidx = nd->child[side ^ 1];
	}

	int leaf = -nodeidx - 1;
	if (leaf <= 0)
		return;
	if (!(pvs[(leaf - 1) >> 3] & (1 << ((leaf - 1) & 7))))
		return;

	const qbsp_leaf *lf = &b->leaves[leaf];
	int c = clip;
	if (!box_visible(lf->mins, lf->maxs, pos, &c))
		return;
	r_dbg_leaves++;

	int first = lf->first_marksurf;
	int nm    = lf->num_marksurf;
	for (int m = 0; m < nm && *drawn < R_MAXWORLD; m++) {
		int f = b->marksurf[first + m];
		if (f >= s->num_faces || f >= R_MAXFACEIDX)
			continue;
		if (face_seen[f] == face_key)
			continue;
		face_seen[f] = face_key;
		r_dbg_faces++;
		uint32_t da = vid_ticks();
		*drawn += draw_face(s, f);
		draw_us_acc += (vid_ticks() - da) & 0xffff;
	}
}

int r_world(const surf_t *s, const bsp_t *b,
            const int32_t pos[3], const vec3_t ang)
{
	sv_nv = s->num_verts < R_MAXVERTS ? s->num_verts : R_MAXVERTS;
	memset(sv_state, 0, sv_nv);
	reset_texwindow();
	build_parents(b);

	if (++face_key == 0) {
		memset(face_seen, 0, sizeof face_seen);
		face_key = 1;
	}
	r_dbg_nodes = r_dbg_leaves = r_dbg_faces = 0;
	r_dbg_drop_near = r_dbg_drop_back = r_dbg_drop_sky = r_dbg_drop_slot = 0;
	r_dbg_vz_lo = 999999;
	r_dbg_vz_hi = -999999;

	vec3_t fw, rt, up;
	AngleVectors(ang, fw, rt, up);
	frustum_setup(rt, up, fw);

	int leaf = bsp_point_leaf(b, pos);
	const uint8_t *pvs = bsp_pvs(b, leaf);
	mark_pvs_leaves(b, pvs);

	draw_us_acc = 0;
	uint32_t tw = vid_ticks();
	int drawn = 0;
	world_node(s, b, b->root, pos, pvs, CLIP_ALL, &drawn);
	uint32_t te = vid_ticks();

	r_dbg_us_draw = (int) (draw_us_acc * 64);
	r_dbg_us_walk = (int) (((te - tw) & 0xffff) * 64) - r_dbg_us_draw;
	return drawn;
}
