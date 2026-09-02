#include <stdint.h>
#include <string.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>

#include "vid.h"
#include "mathlib.h"
#include "surf.h"
#include "tex.h"
#include "render.h"

#define R_MAXVERTS 12288
#define R_MAXLINES 2500   /* per-frame line budget (no culling yet) */
#define R_MAXFACES 600    /* per-frame face budget (until PVS culling, 5.4) */
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

int r_surfaces(const surf_t *s)
{
	int nv = s->num_verts < R_MAXVERTS ? s->num_verts : R_MAXVERTS;
	memset(sv_state, 0, nv);

	tex_slot_t slot;
	int drawn = 0;

	for (int f = 0; f < s->num_faces && drawn < R_MAXFACES; f++) {
		const qsrf_face     *face = &s->faces[f];
		const qsrf_facevert *fv   = &s->fv[face->first_fv];
		int n = face->num_fv;
		if (n < 3 || n > R_FAN_MAX)
			continue;

		int bad = 0;
		for (int k = 0; k < n; k++) {
			int vi = fv[k].vert;
			if (vi >= nv) { bad = 1; break; }
			if (!sv_state[vi]) project(s->verts, vi);
			if (sv_state[vi] == 2) { bad = 1; break; }
		}
		if (bad)
			continue;

		/* backface cull on the first triangle's screen winding */
		int i0 = fv[0].vert, i1 = fv[1].vert, i2 = fv[2].vert;
		int cross = (sv_x[i1] - sv_x[i0]) * (sv_y[i2] - sv_y[i0])
		          - (sv_y[i1] - sv_y[i0]) * (sv_x[i2] - sv_x[i0]);
		if (cross >= 0)
			continue;

		if (!qtx_slot(face->tex, &slot))
			continue;

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

		drawn++;
	}
	return drawn;
}
