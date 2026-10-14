/*
 * qcook — world: QWV2 file (formats/qworld.h).
 *
 * Faces are attached to their BSP node (front block / back block), cut on
 * texture-repeat boundaries so every piece's UVs stay inside one VRAM tile,
 * triangulated into PS1 quads (+ a triangle when odd) and grouped into face
 * records of <= 63 shared vertices.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qcook.h"
#include "../../formats/qworld.h"

/* -------------------------------------------------------------- buffers -- */

typedef struct { uint8_t *p; size_t len, cap; } buf_t;

static void buf_put(buf_t *b, const void *data, size_t n)
{
	if (b->len + n > b->cap) {
		b->cap = (b->cap + n) * 2;
		b->p = realloc(b->p, b->cap);
		if (!b->p)
			die("out of memory");
	}
	memcpy(b->p + b->len, data, n);
	b->len += n;
}

static void buf_align4(buf_t *b)
{
	static const uint8_t z[4];
	buf_put(b, z, (4 - (b->len & 3)) & 3);
}

static int16_t clamp16(long v)
{
	return (int16_t) (v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

/* ------------------------------------------------------- face splitting -- */

#define MAXPV 64

typedef struct { double p[3]; double s, t; } pvert;
typedef struct { int n; pvert v[MAXPV]; } poly_t;

/* Keep the part of `in` where (sign * attr) >= sign * value. attr: 0 = s, 1 = t */
static void clip_poly(const poly_t *in, poly_t *out, int attr, double value, int sign)
{
	out->n = 0;
	for (int i = 0; i < in->n; i++) {
		const pvert *a = &in->v[i], *b = &in->v[(i + 1) % in->n];
		double da = sign * ((attr ? a->t : a->s) - value);
		double db = sign * ((attr ? b->t : b->s) - value);
		if (da >= 0 && out->n < MAXPV)
			out->v[out->n++] = *a;
		if ((da >= 0) != (db >= 0) && out->n < MAXPV) {
			double f = da / (da - db);
			pvert *o = &out->v[out->n++];
			for (int k = 0; k < 3; k++)
				o->p[k] = a->p[k] + f * (b->p[k] - a->p[k]);
			o->s = a->s + f * (b->s - a->s);
			o->t = a->t + f * (b->t - a->t);
			if (attr) o->t = value; else o->s = value;   /* exact on the line */
		}
	}
}

static double poly_area(const poly_t *pl)
{
	double area = 0;
	for (int i = 1; i + 1 < pl->n; i++) {
		double a[3], b[3];
		for (int k = 0; k < 3; k++) {
			a[k] = pl->v[i].p[k] - pl->v[0].p[k];
			b[k] = pl->v[i + 1].p[k] - pl->v[0].p[k];
		}
		double cx = a[1] * b[2] - a[2] * b[1];
		double cy = a[2] * b[0] - a[0] * b[2];
		double cz = a[0] * b[1] - a[1] * b[0];
		area += 0.5 * sqrt(cx * cx + cy * cy + cz * cz);
	}
	return area;
}

/* Drop consecutive vertices closer than 1/16 unit. */
static void poly_weld(poly_t *pl)
{
	int n = 0;
	for (int i = 0; i < pl->n; i++) {
		const pvert *a = &pl->v[i];
		const pvert *b = n ? &pl->v[n - 1] : NULL;
		if (b && fabs(a->p[0] - b->p[0]) < 1.0 / 16 && fabs(a->p[1] - b->p[1]) < 1.0 / 16 &&
		    fabs(a->p[2] - b->p[2]) < 1.0 / 16)
			continue;
		pl->v[n++] = *a;
	}
	while (n > 1 && fabs(pl->v[0].p[0] - pl->v[n - 1].p[0]) < 1.0 / 16 &&
	       fabs(pl->v[0].p[1] - pl->v[n - 1].p[1]) < 1.0 / 16 &&
	       fabs(pl->v[0].p[2] - pl->v[n - 1].p[2]) < 1.0 / 16)
		n--;
	pl->n = n;
}

/* ------------------------------------------------------- record builder -- */

typedef struct { int16_t x, y, z; } vkey;

typedef struct {
	int     nv;
	vkey    v[QW_MAX_VERTS];
	int     nq, nt;
	qw_quad q[256];
	qw_tri  t[256];
} rec_t;

typedef struct {
	buf_t    stream;
	uint32_t records, quads, tris, verts, pieces, faces_split;
} out_t;

static int rec_find_or_add(rec_t *r, vkey k)
{
	for (int i = 0; i < r->nv; i++)
		if (r->v[i].x == k.x && r->v[i].y == k.y && r->v[i].z == k.z)
			return i;
	if (r->nv >= QW_MAX_VERTS)
		return -1;
	r->v[r->nv] = k;
	return r->nv++;
}

static void rec_flush(out_t *o, rec_t *r, int face_id, const tex_slot *s)
{
	if (!r->nq && !r->nt) {
		r->nv = 0;
		return;
	}

	qw_record h;
	h.face_id   = (uint16_t) face_id;
	h.num_verts = (uint8_t) r->nv;
	h.num_quads = (uint8_t) r->nq;
	h.num_tris  = (uint8_t) r->nt;
	h.flags     = s->flags;
	h.size      = (uint16_t) (sizeof h + r->nv * sizeof(qw_vert) +
	                          r->nq * sizeof(qw_quad) + r->nt * sizeof(qw_tri));
	h.clut      = s->clut;
	h.tpage     = s->tpage;

	/* bounding sphere: bbox centre, radius = farthest vertex (rounded up) */
	long lo[3] = { 32767, 32767, 32767 }, hi[3] = { -32768, -32768, -32768 };
	for (int i = 0; i < r->nv; i++) {
		long p[3] = { r->v[i].x, r->v[i].y, r->v[i].z };
		for (int k = 0; k < 3; k++) {
			if (p[k] < lo[k]) lo[k] = p[k];
			if (p[k] > hi[k]) hi[k] = p[k];
		}
	}
	for (int k = 0; k < 3; k++)
		h.sc[k] = (int16_t) ((lo[k] + hi[k]) / 2);
	double rad = 0;
	for (int i = 0; i < r->nv; i++) {
		double dx = r->v[i].x - h.sc[0], dy = r->v[i].y - h.sc[1], dz = r->v[i].z - h.sc[2];
		double d = sqrt(dx * dx + dy * dy + dz * dz);
		if (d > rad) rad = d;
	}
	if (rad > 65000) die("record radius %.0f too large", rad);
	h.sr = (uint16_t) ceil(rad + 1);
	buf_put(&o->stream, &h, sizeof h);
	for (int i = 0; i < r->nv; i++) {
		qw_vert v = { r->v[i].x, r->v[i].y, r->v[i].z, 0 };
		buf_put(&o->stream, &v, sizeof v);
	}
	buf_put(&o->stream, r->q, (size_t) r->nq * sizeof(qw_quad));
	buf_put(&o->stream, r->t, (size_t) r->nt * sizeof(qw_tri));

	o->records++;
	o->quads += (uint32_t) r->nq;
	o->tris  += (uint32_t) r->nt;
	o->verts += (uint32_t) r->nv;
	r->nv = r->nq = r->nt = 0;
}

static uint16_t uvword(const tex_slot *s, double u, double v)
{
	long uu = s->u0 + lround(u), vv = s->v0 + lround(v);
	if (uu < 0) uu = 0;
	if (uu > 255) uu = 255;
	if (vv < 0) vv = 0;
	if (vv > 255) vv = 255;
	return (uint16_t) (uu | (vv << 8));
}

/* Add one convex piece (UVs already local to its tile) to the record,
   flushing first if its vertices would not fit. */
static void rec_add_piece(out_t *o, rec_t *r, const poly_t *pc, const double (*uv)[2],
                          const tex_slot *s, int face_id)
{
	vkey keys[MAXPV];
	for (int i = 0; i < pc->n; i++) {
		keys[i].x = clamp16(lround(pc->v[i].p[0] * QW_VSCALE));
		keys[i].y = clamp16(lround(pc->v[i].p[1] * QW_VSCALE));
		keys[i].z = clamp16(lround(pc->v[i].p[2] * QW_VSCALE));
	}

	/* will it fit? count new vertices */
	int fresh = 0;
	for (int i = 0; i < pc->n; i++) {
		int seen = 0;
		for (int j = 0; j < r->nv && !seen; j++)
			seen = r->v[j].x == keys[i].x && r->v[j].y == keys[i].y && r->v[j].z == keys[i].z;
		for (int j = 0; j < i && !seen; j++)
			seen = keys[j].x == keys[i].x && keys[j].y == keys[i].y && keys[j].z == keys[i].z;
		fresh += !seen;
	}
	int tris_needed = pc->n - 2;
	if (r->nv + fresh > QW_MAX_VERTS || r->nq + tris_needed > 255 || r->nt + 1 > 255)
		rec_flush(o, r, face_id, s);

	int idx[MAXPV];
	for (int i = 0; i < pc->n; i++) {
		idx[i] = rec_find_or_add(r, keys[i]);
		if (idx[i] < 0)
			die("piece with %d vertices does not fit a record", pc->n);
	}

	/* fan of quads (p0, pi, pi+1, pi+2) -> PS1 order (p0, pi, pi+2, pi+1) */
	int i = 1;
	for (; i + 2 < pc->n; i += 2) {
		qw_quad *q = &r->q[r->nq++];
		int c[4] = { 0, i, i + 2, i + 1 };
		for (int k = 0; k < 4; k++) {
			q->idx[k] = (uint8_t) (idx[c[k]] * 4);
			q->uv[k]  = uvword(s, uv[c[k]][0], uv[c[k]][1]);
		}
	}
	if (i + 1 < pc->n) {                       /* one triangle left */
		qw_tri *t = &r->t[r->nt++];
		int c[3] = { 0, i, i + 1 };
		for (int k = 0; k < 3; k++) {
			t->idx[k] = (uint8_t) (idx[c[k]] * 4);
			t->uv[k]  = uvword(s, uv[c[k]][0], uv[c[k]][1]);
		}
		t->pad = 0;
		t->pad2 = 0;
	}
	o->pieces++;
}

/* Cut one Quake face on its texture-repeat grid and append its records. */
static void emit_face(out_t *o, const bsp_t *b, const tex_set *ts, int f)
{
	const bsp_face *bf = &b->faces[f];
	if (bf->numedges < 3 || bf->texinfo < 0 || bf->texinfo >= b->num_texinfo)
		return;
	const bsp_texinfo *ti = &b->texinfo[bf->texinfo];
	if (ti->miptex < 0 || ti->miptex >= b->num_miptex)
		return;
	const tex_slot   *s = &ts->slots[ti->miptex];
	const bsp_miptex *m = &b->miptex[ti->miptex];
	if (!s->present)                    /* sky or missing: not drawn (yet) */
		return;

	poly_t pl;
	pl.n = 0;
	for (int e = 0; e < bf->numedges && pl.n < MAXPV; e++) {
		int se = b->surfedges[bf->firstedge + e];
		int vi = se >= 0 ? b->edges[se * 2] : b->edges[-se * 2 + 1];
		pvert *v = &pl.v[pl.n++];
		for (int k = 0; k < 3; k++)
			v->p[k] = b->verts[vi * 3 + k];
		v->s = v->p[0] * ti->vecs[0][0] + v->p[1] * ti->vecs[0][1] + v->p[2] * ti->vecs[0][2] + ti->vecs[0][3];
		v->t = v->p[0] * ti->vecs[1][0] + v->p[1] * ti->vecs[1][1] + v->p[2] * ti->vecs[1][2] + ti->vecs[1][3];
	}

	double smin = 1e30, smax = -1e30, tmin = 1e30, tmax = -1e30;
	for (int i = 0; i < pl.n; i++) {
		if (pl.v[i].s < smin) smin = pl.v[i].s;
		if (pl.v[i].s > smax) smax = pl.v[i].s;
		if (pl.v[i].t < tmin) tmin = pl.v[i].t;
		if (pl.v[i].t > tmax) tmax = pl.v[i].t;
	}

	/* one cell = the tile: reps_x x reps_y texture repetitions */
	const double W = (double) m->w * s->reps_x, H = (double) m->h * s->reps_y;
	long i0 = (long) floor(smin / W), i1 = (long) floor((smax - 1e-4) / W);
	long j0 = (long) floor(tmin / H), j1 = (long) floor((tmax - 1e-4) / H);
	if ((i1 - i0 + 1) * (j1 - j0 + 1) > 1)
		o->faces_split++;

	rec_t *r = xcalloc(1, sizeof *r);
	for (long i = i0; i <= i1; i++) {
		poly_t a, c;
		clip_poly(&pl, &a, 0, i * W, +1);
		clip_poly(&a, &c, 0, (i + 1) * W, -1);
		for (long j = j0; j <= j1; j++) {
			poly_t d, pc;
			clip_poly(&c, &d, 1, j * H, +1);
			clip_poly(&d, &pc, 1, (j + 1) * H, -1);
			poly_weld(&pc);
			if (pc.n < 3 || poly_area(&pc) < 1.0 / 64)
				continue;
			double uv[MAXPV][2];
			for (int k = 0; k < pc.n; k++) {
				uv[k][0] = (pc.v[k].s - i * W) * s->rep_w * s->reps_x / W;
				uv[k][1] = (pc.v[k].t - j * H) * s->rep_h * s->reps_y / H;
			}
			rec_add_piece(o, r, &pc, (const double (*)[2]) uv, s, f);
		}
	}
	rec_flush(o, r, f, s);
	free(r);
}

/* ------------------------------------------------------------- entities -- */

static int find_player_start(const bsp_t *b, double origin[3], double *angle)
{
	const char *p = b->entities, *end = p + b->ent_len;
	while (p < end) {
		while (p < end && *p != '{') p++;
		if (p >= end) break;
		p++;
		char cls[64] = "";
		double o[3] = { 0, 0, 0 }, a = 0;
		int have_o = 0;
		while (p < end && *p != '}') {
			char key[64], val[256];
			int i;
			while (p < end && *p != '"' && *p != '}') p++;
			if (p >= end || *p == '}') break;
			for (i = 0, p++; p < end && *p != '"' && i < 63; i++) key[i] = *p++;
			key[i] = 0;
			if (p < end) p++;
			while (p < end && *p != '"' && *p != '}') p++;
			if (p >= end || *p == '}') break;
			for (i = 0, p++; p < end && *p != '"' && i < 255; i++) val[i] = *p++;
			val[i] = 0;
			if (p < end) p++;
			if (!strcmp(key, "classname")) snprintf(cls, sizeof cls, "%s", val);
			else if (!strcmp(key, "origin")) have_o = sscanf(val, "%lf %lf %lf", &o[0], &o[1], &o[2]) == 3;
			else if (!strcmp(key, "angle")) a = atof(val);
		}
		if (!strcmp(cls, "info_player_start") && have_o) {
			memcpy(origin, o, sizeof o);
			*angle = a;
			return 1;
		}
		if (p < end) p++;
	}
	return 0;
}

/* ---------------------------------------------------------------- world -- */

static int depth_of(const bsp_t *b, int node)
{
	if (node < 0)
		return 0;
	int a = depth_of(b, b->nodes[node].child[0]);
	int c = depth_of(b, b->nodes[node].child[1]);
	return 1 + (a > c ? a : c);
}

static void mark_reachable(const bsp_t *b, int node, uint8_t *reach)
{
	if (node < 0)
		return;
	reach[node] = 1;
	mark_reachable(b, b->nodes[node].child[0], reach);
	mark_reachable(b, b->nodes[node].child[1], reach);
}

/* Quake's rendering-hull point test: contents of the leaf holding p. */
static int point_contents(const bsp_t *b, int node, const double p[3])
{
	while (node >= 0) {
		const bsp_node  *n  = &b->nodes[node];
		const bsp_plane *pl = &b->planes[n->planenum];
		double d = pl->n[0] * p[0] + pl->n[1] * p[1] + pl->n[2] * p[2] - pl->dist;
		node = n->child[d < 0];
	}
	return b->leaves[-1 - node].contents;
}

static void bbox(const int16_t mins[3], const int16_t maxs[3], int16_t c[3], int16_t h[3])
{
	for (int k = 0; k < 3; k++) {
		long lo = (long) mins[k] * QW_VSCALE, hi = (long) maxs[k] * QW_VSCALE;
		c[k] = clamp16((lo + hi) / 2);
		h[k] = clamp16((hi - lo) / 2 + QW_VSCALE);   /* +1 world unit margin */
	}
}

void world_build(const bsp_t *b, const tex_set *ts, const fidelity_t *fid,
                 const char *out_path)
{
	(void) fid;
	const bsp_model *wm = &b->models[0];
	int root = wm->headnode[0];

	uint8_t *reach = xcalloc((size_t) b->num_nodes, 1);
	mark_reachable(b, root, reach);

	/* parents */
	uint32_t *node_parent = xmalloc((size_t) b->num_nodes * 4);
	uint32_t *leaf_parent = xmalloc((size_t) b->num_leaves * 4);
	for (int i = 0; i < b->num_nodes; i++) node_parent[i] = QW_NONE;
	for (int i = 0; i < b->num_leaves; i++) leaf_parent[i] = QW_NONE;
	for (int i = 0; i < b->num_nodes; i++) {
		if (!reach[i])
			continue;
		for (int c = 0; c < 2; c++) {
			int ch = b->nodes[i].child[c];
			if (ch >= 0) node_parent[ch] = (uint32_t) i;
			else         leaf_parent[-1 - ch] = (uint32_t) i;
		}
	}

	/* face stream, node by node: front block then back block */
	out_t o;
	memset(&o, 0, sizeof o);
	qw_node *nodes = xcalloc((size_t) b->num_nodes, sizeof *nodes);
	for (int i = 0; i < b->num_nodes; i++) {
		const bsp_node *bn = &b->nodes[i];
		const bsp_plane *pl = &b->planes[bn->planenum];
		qw_node *n = &nodes[i];

		bbox(bn->mins, bn->maxs, n->c, n->h);
		for (int k = 0; k < 3; k++)
			n->n[k] = clamp16(lround(pl->n[k] * 4096.0));
		n->type = (uint8_t) (pl->type < 3 ? pl->type : 3);
		n->dist = (int32_t) lround((double) pl->dist * 4096.0 * QW_VSCALE);
		for (int c = 0; c < 2; c++) {
			int ch = bn->child[c];
			n->child[c] = ch >= 0 ? (uint32_t) ch : (0x80000000u | (uint32_t) (-1 - ch));
		}
		n->parent = node_parent[i];

		for (int side = 0; side < 2; side++) {
			n->faces[side] = (uint32_t) o.stream.len;
			if (!reach[i])
				continue;
			for (int f = bn->firstface; f < bn->firstface + bn->numfaces; f++)
				if (b->faces[f].side == side)
					emit_face(&o, b, ts, f);
		}
		n->faces[2] = (uint32_t) o.stream.len;
	}

	/* leaves */
	qw_leaf *leaves = xcalloc((size_t) b->num_leaves, sizeof *leaves);
	for (int i = 0; i < b->num_leaves; i++) {
		const bsp_leaf *bl = &b->leaves[i];
		qw_leaf *l = &leaves[i];
		bbox(bl->mins, bl->maxs, l->c, l->h);
		l->num_marks = (uint16_t) bl->nummarks;
		l->marks     = (uint32_t) bl->firstmark;
		l->parent    = leaf_parent[i];
		l->vis       = bl->visofs >= 0 && (uint32_t) bl->visofs < b->vis_len
		             ? (uint32_t) bl->visofs : QW_NONE;
		l->contents  = bl->contents;
	}

	/* bench views: spawn x4 yaws, then the 3 leaves that see the most faces */
	qw_view views[16];
	int nviews = 0;
	double so[3] = { 0, 0, 0 }, sa = 0;
	int has_spawn = find_player_start(b, so, &sa);
	if (has_spawn)
		for (int k = 0; k < 4; k++) {
			qw_view *v = &views[nviews++];
			v->pos[0] = (int32_t) lround(so[0]);
			v->pos[1] = (int32_t) lround(so[1]);
			v->pos[2] = (int32_t) lround(so[2]) + 22;    /* eye height */
			v->yaw    = (int16_t) (((long) lround(sa) + 90 * k) % 360);
			v->pitch  = 0;
		}
	{
		int rowbytes = (wm->visleafs + 7) >> 3;
		long best[3] = { -1, -1, -1 };
		int  besti[3] = { 0, 0, 0 };
		uint8_t *row = xmalloc((size_t) rowbytes + 1);
		for (int i = 1; i <= wm->visleafs && i < b->num_leaves; i++) {
			const bsp_leaf *bl = &b->leaves[i];
			if (bl->visofs < 0 || bl->contents != -1)
				continue;
			/* decompress Quake RLE row */
			const uint8_t *in = b->vis + bl->visofs;
			int out = 0;
			while (out < rowbytes && in < b->vis + b->vis_len) {
				if (*in) { row[out++] = *in++; continue; }
				int c = in[1];
				in += 2;
				while (c-- && out < rowbytes) row[out++] = 0;
			}
			long load = 0;
			for (int j = 0; j < wm->visleafs; j++)
				if (row[j >> 3] & (1 << (j & 7)))
					load += b->leaves[j + 1].nummarks;
			for (int k = 0; k < 3; k++)
				if (load > best[k]) {
					for (int m = 2; m > k; m--) { best[m] = best[m - 1]; besti[m] = besti[m - 1]; }
					best[k] = load;
					besti[k] = i;
					break;
				}
		}
		free(row);
		for (int k = 0; k < 3; k++) {
			if (best[k] < 0)
				continue;
			const bsp_leaf *bl = &b->leaves[besti[k]];
			for (int yaw = 0; yaw < 360; yaw += 180) {
				qw_view *v = &views[nviews++];
				for (int c = 0; c < 3; c++)
					v->pos[c] = (bl->mins[c] + bl->maxs[c]) / 2;
				v->yaw = (int16_t) yaw;
				v->pitch = 0;
			}
		}
	}
	if (has_spawn) {
		/* near-geometry views for the slow path: the floor right under the
		   eye, and a wall at contact distance (12 units from it, about the
		   player's radius) */
		qw_view *v = &views[nviews++];
		*v = views[0];
		v->pitch = 60;

		/* the first of the 4 axial directions from the spawn that meets a
		   SOLID wall within 512 units (liquids are not walls) */
		for (int k = 0; k < 4; k++) {
			double yaw = (sa + 90.0 * k) * M_PI / 180.0, p[3] = { so[0], so[1], so[2] + 22 };
			double dx = cos(yaw), dy = sin(yaw);
			int hit = 0;
			for (int step = 0; step < 128 && !hit; step++) {
				double q[3] = { p[0] + dx * 4, p[1] + dy * 4, p[2] };
				int c = point_contents(b, root, q);
				if (c == -2)
					hit = 1;                         /* CONTENTS_SOLID */
				else if (c != -1)
					break;                           /* liquid: try another way */
				else
					memcpy(p, q, sizeof p);
			}
			if (!hit)
				continue;
			v = &views[nviews++];
			v->pos[0] = (int32_t) lround(p[0] - dx * 12);
			v->pos[1] = (int32_t) lround(p[1] - dy * 12);
			v->pos[2] = (int32_t) lround(p[2]);
			v->yaw    = (int16_t) (lround(sa + 90.0 * k) % 360);
			v->pitch  = 0;
			break;
		}
	}

	/* assemble */
	qw_header h;
	memset(&h, 0, sizeof h);
	memcpy(h.magic, QWORLD_MAGIC, 4);
	h.num_nodes   = (uint32_t) b->num_nodes;
	h.num_leaves  = (uint32_t) b->num_leaves;
	h.num_marks   = (uint32_t) b->num_marks;
	h.num_faces   = (uint32_t) b->num_faces;
	h.num_records = o.records;
	h.num_pieces  = o.quads + o.tris;
	h.num_views   = (uint32_t) nviews;
	h.has_spawn   = (uint32_t) has_spawn;
	for (int k = 0; k < 3; k++)
		h.spawn[k] = (int32_t) lround(so[k]);
	h.spawn_yaw = (int32_t) lround(sa);
	h.root      = (uint32_t) root;
	h.max_depth = (uint32_t) depth_of(b, root);
	h.num_visleafs = (uint32_t) wm->visleafs;
	if (h.max_depth > QW_MAX_DEPTH)
		die("BSP depth %u exceeds the renderer stack (%d)", h.max_depth, QW_MAX_DEPTH);

	buf_t f;
	memset(&f, 0, sizeof f);
	buf_put(&f, &h, sizeof h);
	h.ofs_nodes = (uint32_t) f.len;  buf_put(&f, nodes, (size_t) b->num_nodes * sizeof *nodes);
	h.ofs_leaves = (uint32_t) f.len; buf_put(&f, leaves, (size_t) b->num_leaves * sizeof *leaves);
	h.ofs_marks = (uint32_t) f.len;  buf_put(&f, b->marks, (size_t) b->num_marks * 2); buf_align4(&f);
	h.ofs_vis = (uint32_t) f.len;    buf_put(&f, b->vis, b->vis_len); buf_align4(&f);
	h.len_vis = b->vis_len;
	h.ofs_faces = (uint32_t) f.len;  buf_put(&f, o.stream.p, o.stream.len);
	h.len_faces = (uint32_t) o.stream.len;
	{ static const uint8_t tail[QW_STREAM_TAIL]; buf_put(&f, tail, sizeof tail); }
	h.ofs_views = (uint32_t) f.len;  buf_put(&f, views, (size_t) nviews * sizeof *views);
	memcpy(f.p, &h, sizeof h);

	FILE *fp = fopen(out_path, "wb");
	if (!fp)
		die("cannot write %s", out_path);
	fwrite(f.p, 1, f.len, fp);
	fclose(fp);

	printf("qcook: %s — %u B: nodes %zu, leaves %zu, marks %u, vis %u, faces %u\n",
	       out_path, (uint32_t) f.len, b->num_nodes * sizeof *nodes,
	       b->num_leaves * sizeof *leaves, b->num_marks * 2, b->vis_len, h.len_faces);
	printf("qcook:   %u records, %u pieces -> %u quads + %u tris, %u verts "
	       "(%.1f/record); %u faces split; depth %u; %d bench views\n",
	       o.records, o.pieces, o.quads, o.tris, o.verts,
	       o.records ? (double) o.verts / o.records : 0.0, o.faces_split,
	       h.max_depth, nviews);
	if (has_spawn)
		printf("qcook:   spawn %d %d %d yaw %d\n", h.spawn[0], h.spawn[1], h.spawn[2], h.spawn_yaw);

	free(f.p); free(o.stream.p); free(nodes); free(leaves);
	free(node_parent); free(leaf_parent); free(reach);
}

/* ---------------------------------------------------------------- check -- */

#define FAIL(...) do { fprintf(stderr, "check: " __VA_ARGS__); fputc('\n', stderr); return 1; } while (0)

int world_check(const char *path)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		FAIL("cannot open %s", path);
	fseek(fp, 0, SEEK_END);
	long len = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	uint8_t *d = xmalloc((size_t) len);
	if (fread(d, 1, (size_t) len, fp) != (size_t) len)
		FAIL("short read");
	fclose(fp);

	qw_header h;
	if ((size_t) len < sizeof h)
		FAIL("too small");
	memcpy(&h, d, sizeof h);
	if (memcmp(h.magic, QWORLD_MAGIC, 4))
		FAIL("bad magic");

#define SECTION(ofs, n) do { if ((ofs) & 3) FAIL("unaligned section at %u", (ofs)); \
	if ((uint64_t) (ofs) + (n) > (uint64_t) len) FAIL("section at %u (+%u) past EOF", (ofs), (uint32_t) (n)); } while (0)
	SECTION(h.ofs_nodes, h.num_nodes * sizeof(qw_node));
	SECTION(h.ofs_leaves, h.num_leaves * sizeof(qw_leaf));
	SECTION(h.ofs_marks, h.num_marks * 2);
	SECTION(h.ofs_vis, h.len_vis);
	SECTION(h.ofs_faces, h.len_faces);
	SECTION(h.ofs_views, h.num_views * sizeof(qw_view));
	if (h.max_depth > QW_MAX_DEPTH)
		FAIL("depth %u > %d", h.max_depth, QW_MAX_DEPTH);
	if (h.root >= h.num_nodes)
		FAIL("root %u out of range", h.root);

	const qw_node *nodes = (const qw_node *) (d + h.ofs_nodes);
	const qw_leaf *leaves = (const qw_leaf *) (d + h.ofs_leaves);
	const uint8_t *fs = d + h.ofs_faces;
	uint32_t recs = 0, polys = 0;

	for (uint32_t i = 0; i < h.num_nodes; i++) {
		const qw_node *n = &nodes[i];
		for (int c = 0; c < 2; c++) {
			uint32_t ch = n->child[c];
			if (ch & 0x80000000u) { if ((ch & 0x7fffffffu) >= h.num_leaves) FAIL("node %u: leaf child out of range", i); }
			else if (ch >= h.num_nodes) FAIL("node %u: child out of range", i);
		}
		if (n->parent != QW_NONE && n->parent >= h.num_nodes)
			FAIL("node %u: parent out of range", i);
		if (n->type > 3)
			FAIL("node %u: bad plane type", i);
		if (!(n->faces[0] <= n->faces[1] && n->faces[1] <= n->faces[2] && n->faces[2] <= h.len_faces))
			FAIL("node %u: face block offsets out of order", i);
		if (i + 1 < h.num_nodes && nodes[i + 1].faces[0] != n->faces[2])
			FAIL("node %u: face blocks not contiguous", i);

		/* every record of both blocks must decode exactly to the block end */
		for (int side = 0; side < 2; side++) {
			uint32_t at = n->faces[side], end = n->faces[side + 1];
			while (at < end) {
				qw_record r;
				if (at + sizeof r > end) FAIL("node %u: truncated record", i);
				memcpy(&r, fs + at, sizeof r);
				uint32_t sz = (uint32_t) (sizeof r + r.num_verts * sizeof(qw_vert) +
				              r.num_quads * sizeof(qw_quad) + r.num_tris * sizeof(qw_tri));
				if (r.size != sz) FAIL("record at %u: size %u != %u", at, r.size, sz);
				if (!r.num_verts || r.num_verts > QW_MAX_VERTS)
					FAIL("record at %u: %u verts", at, r.num_verts);
				if (r.face_id >= h.num_faces) FAIL("record at %u: face id %u", at, r.face_id);
				const uint8_t *q = fs + at + sizeof r + r.num_verts * sizeof(qw_vert);
				for (int k = 0; k < r.num_quads; k++, q += sizeof(qw_quad))
					for (int c = 0; c < 4; c++)
						if (q[c] & 3 || q[c] / 4 >= r.num_verts) FAIL("record at %u: quad index", at);
				for (int k = 0; k < r.num_tris; k++, q += sizeof(qw_tri))
					for (int c = 0; c < 3; c++)
						if (q[c] & 3 || q[c] / 4 >= r.num_verts) FAIL("record at %u: tri index", at);
				recs++;
				polys += r.num_quads + r.num_tris;
				at += r.size;
			}
			if (at != end) FAIL("node %u: block overrun", i);
		}
	}
	if (recs != h.num_records || polys != h.num_pieces)
		FAIL("record/poly count mismatch (%u/%u vs %u/%u)", recs, polys, h.num_records, h.num_pieces);

	for (uint32_t i = 0; i < h.num_leaves; i++) {
		const qw_leaf *l = &leaves[i];
		if (l->marks + l->num_marks > h.num_marks) FAIL("leaf %u: marks out of range", i);
		if (l->vis != QW_NONE && l->vis >= h.len_vis) FAIL("leaf %u: vis out of range", i);
		if (l->parent != QW_NONE && l->parent >= h.num_nodes) FAIL("leaf %u: parent out of range", i);
	}
	const uint16_t *marks = (const uint16_t *) (d + h.ofs_marks);
	for (uint32_t i = 0; i < h.num_marks; i++)
		if (marks[i] >= h.num_faces) FAIL("mark %u out of range", i);

	printf("check: %s OK — %u nodes, %u leaves, %u records, %u polys, depth %u\n",
	       path, h.num_nodes, h.num_leaves, recs, polys, h.max_depth);
	free(d);
	return 0;
}
