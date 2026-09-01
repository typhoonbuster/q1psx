#include <stdint.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <inline_c.h>

#include "vid.h"
#include "mathlib.h"
#include "render.h"

#define R_MAXVERTS 12288
#define R_MAXLINES 2500   /* per-frame line budget (no culling yet) */
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
