#include <stdint.h>
#include <string.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>

#include "vid.h"
#include "mathlib.h"
#include "surf.h"
#include "tex.h"
#include "bsp.h"
#include "render.h"

#define R_MAXVERTS 12288
#define R_MAXLINES 2500   /* per-frame line budget (no culling yet)          */
#define R_MAXFACES 600    /* per-frame face budget for the un-culled path    */
#define R_MAXWORLD 800    /* per-frame face budget for the PVS-culled path   */
#define R_MAXFACEIDX 8192 /* > any map's world-face count (MAX_MAP_FACES-ish) */
#define R_FAN_MAX  22     /* largest cooked face polygon */
#define NEAR_Z     16     /* skip vertices closer than this along view Z */

static int16_t scr_x[R_MAXVERTS];
static int16_t scr_y[R_MAXVERTS];
static uint8_t scr_ok[R_MAXVERTS];

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

	gte_SetRotMatrix(&m);
	gte_SetTransMatrix(&m);
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
}

/* Project (cached), back-face cull on screen winding, fan-triangulate one face
   into POLY_FT3s preceded by its DR_TWIN. Returns 1 if it emitted, else 0. */
static int draw_face(const surf_t *s, int f)
{
	const qsrf_face     *face = &s->faces[f];
	const qsrf_facevert *fv   = &s->fv[face->first_fv];
	int n = face->num_fv;
	if (n < 3 || n > R_FAN_MAX)
		return 0;

	for (int k = 0; k < n; k++) {
		int vi = fv[k].vert;
		if (vi >= sv_nv)
			return 0;
		if (!sv_state[vi])
			project(s->verts, vi);
		if (sv_state[vi] == 2)
			return 0;
	}

	int i0 = fv[0].vert, i1 = fv[1].vert, i2 = fv[2].vert;
	int cross = (sv_x[i1] - sv_x[i0]) * (sv_y[i2] - sv_y[i0])
	          - (sv_y[i1] - sv_y[i0]) * (sv_x[i2] - sv_x[i0]);
	if (cross >= 0)
		return 0;

	tex_slot_t slot;
	if (!qtx_slot(face->tex, &slot))
		return 0;

	/* fan: emit tris reversed, then the texture window, so on the OT the
	   window runs immediately before this face's triangles */
	int ntri = n - 2;
	for (int k = ntri; k >= 1; k--) {
		int a = fv[0].vert, b = fv[k].vert, c = fv[k + 1].vert;
		POLY_FT3 *t = vid_alloc_raw(sizeof(POLY_FT3));
		setPolyFT3(t);
		setRGB0(t, 128, 128, 128);
		setXY3(t, sv_x[a], sv_y[a], sv_x[b], sv_y[b], sv_x[c], sv_y[c]);
		setUV3(t, fv[0].u, fv[0].v, fv[k].u, fv[k].v, fv[k + 1].u, fv[k + 1].v);
		t->tpage = slot.tpage;
		t->clut  = slot.clut;
		vid_link(4, t);
	}
	DR_TWIN *tw = vid_alloc_raw(sizeof(DR_TWIN));
	setTexWindow(tw, &slot.twin);
	vid_link(4, tw);
	return 1;
}

/* Draw every cooked face (no visibility culling) — the SELECT fallback view. */
int r_surfaces(const surf_t *s)
{
	sv_nv = s->num_verts < R_MAXVERTS ? s->num_verts : R_MAXVERTS;
	memset(sv_state, 0, sv_nv);

	int drawn = 0;
	for (int f = 0; f < s->num_faces && drawn < R_MAXFACES; f++)
		drawn += draw_face(s, f);
	return drawn;
}

/* ------------------------------------------------------- world (PVS + frustum) */

/* world-space inward frustum normals (~x4096 scale), rebuilt each r_world call.
   0 right, 1 left, 2 bottom, 3 top, 4 near. */
static int32_t fr_n[5][3];
#define CLIP_ALL 0x1f

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
		int64_t vmax = 0, vmin = 0;
		for (int k = 0; k < 3; k++) {
			int32_t hi = (n[k] >= 0 ? mx[k] : mn[k]) - pos[k];
			int32_t lo = (n[k] >= 0 ? mn[k] : mx[k]) - pos[k];
			vmax += (int64_t) n[k] * hi;
			vmin += (int64_t) n[k] * lo;
		}
		int64_t bias = (i == 4) ? ((int64_t) NEAR_Z << Q_FIX_SHIFT) : 0;
		if (vmax < bias)
			return 0;
		if (vmin >= bias)
			c &= ~(1 << i);
	}
	*clip = c;
	return 1;
}

static uint8_t face_seen[R_MAXFACEIDX];
static uint8_t face_key;

static void world_node(const surf_t *s, const bsp_t *b, int nodeidx,
                       const int32_t pos[3], const uint8_t *pvs,
                       int clip, int *drawn)
{
	while (nodeidx >= 0) {
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

	int first = lf->first_marksurf;
	int nm    = lf->num_marksurf;
	for (int m = 0; m < nm && *drawn < R_MAXWORLD; m++) {
		int f = b->marksurf[first + m];
		if (f >= s->num_faces || f >= R_MAXFACEIDX)
			continue;
		if (face_seen[f] == face_key)
			continue;
		face_seen[f] = face_key;
		*drawn += draw_face(s, f);
	}
}

int r_world(const surf_t *s, const bsp_t *b,
            const int32_t pos[3], const vec3_t ang)
{
	sv_nv = s->num_verts < R_MAXVERTS ? s->num_verts : R_MAXVERTS;
	memset(sv_state, 0, sv_nv);

	if (++face_key == 0) {
		memset(face_seen, 0, sizeof face_seen);
		face_key = 1;
	}

	vec3_t fw, rt, up;
	AngleVectors(ang, fw, rt, up);
	frustum_setup(rt, up, fw);

	int leaf = bsp_point_leaf(b, pos);
	const uint8_t *pvs = bsp_pvs(b, leaf);

	int drawn = 0;
	world_node(s, b, b->root, pos, pvs, CLIP_ALL, &drawn);
	return drawn;
}
