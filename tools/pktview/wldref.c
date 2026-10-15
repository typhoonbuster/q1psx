/*
 * wldref — ground-truth render of a cooked QWV2 world (host tool).
 *
 *   wldref <world.wld> <textures.tex> <out-prefix> [view-index]
 *
 * Draws EVERY face record of the world with a z-buffer and perspective-
 * correct texturing from each bench viewpoint stored in the world file (no
 * BSP, no PVS, no culling, no painter's order), using the same view
 * conventions as engine/render.c (x right, y down, z forward, H = 160 on
 * 320x240). Compare <out-prefix><n>.png with the asm renderer's
 * build-bench/views/view<n>.png: anything missing or extra there is a
 * renderer bug (culling, ordering, PVS); near polygons (< 43 u) are expected
 * to differ until the slow path exists.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../formats/qtex.h"
#include "../../formats/qworld.h"

#define W 320
#define H 240

static uint16_t vram[512][1024];
static uint8_t  fb[H][W][3];
static float    zb[H][W];

/* --- png (same minimal writer as pktview) -------------------------------- */

static uint32_t crc_table[256];
static void crc_init(void)
{
	for (uint32_t n = 0; n < 256; n++) {
		uint32_t c = n;
		for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
		crc_table[n] = c;
	}
}
static uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
	c = ~c;
	while (n--) c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
	return ~c;
}
static void put32(FILE *f, uint32_t v)
{
	uint8_t b[4] = { (uint8_t) (v >> 24), (uint8_t) (v >> 16), (uint8_t) (v >> 8), (uint8_t) v };
	fwrite(b, 1, 4, f);
}
static void chunk(FILE *f, const char *t, const uint8_t *d, uint32_t n)
{
	put32(f, n); fwrite(t, 1, 4, f); fwrite(d, 1, n, f);
	put32(f, crc(crc(0, (const uint8_t *) t, 4), d, n));
}
static void write_png(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f) { fprintf(stderr, "wldref: cannot write %s\n", path); exit(1); }
	static const uint8_t sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
	fwrite(sig, 1, 8, f);
	uint8_t ihdr[13] = { 0, 0, W >> 8, W & 255, 0, 0, H >> 8, H & 255, 8, 2, 0, 0, 0 };
	chunk(f, "IHDR", ihdr, 13);
	size_t rl = (size_t) H * (1 + W * 3);
	uint8_t *raw = malloc(rl);
	for (int y = 0; y < H; y++) { raw[y * (1 + W * 3)] = 0; memcpy(raw + y * (1 + W * 3) + 1, fb[y], W * 3); }
	size_t nb = (rl + 65534) / 65535;
	uint8_t *z = malloc(2 + nb * 5 + rl + 4), *p = z;
	*p++ = 0x78; *p++ = 0x01;
	uint32_t a = 1, b = 0;
	for (size_t i = 0; i < rl; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
	for (size_t off = 0; off < rl; off += 65535) {
		size_t n = rl - off < 65535 ? rl - off : 65535;
		*p++ = off + n == rl; *p++ = (uint8_t) n; *p++ = (uint8_t) (n >> 8);
		*p++ = (uint8_t) ~n; *p++ = (uint8_t) (~n >> 8);
		memcpy(p, raw + off, n); p += n;
	}
	uint32_t ad = (b << 16) | a;
	*p++ = (uint8_t) (ad >> 24); *p++ = (uint8_t) (ad >> 16); *p++ = (uint8_t) (ad >> 8); *p++ = (uint8_t) ad;
	chunk(f, "IDAT", z, (uint32_t) (p - z));
	chunk(f, "IEND", NULL, 0);
	fclose(f); free(raw); free(z);
}

static uint8_t *slurp(const char *path, long *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) { fprintf(stderr, "wldref: cannot open %s\n", path); exit(1); }
	fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t *d = malloc((size_t) *len);
	if (fread(d, 1, (size_t) *len, f) != (size_t) *len) { fprintf(stderr, "wldref: short read\n"); exit(1); }
	fclose(f);
	return d;
}

static void load_tex(const char *path)
{
	long len;
	uint8_t *d = slurp(path, &len);
	qtex_header h;
	memcpy(&h, d, sizeof h);
	memcpy(&vram[QTEX_CLUT_Y][QTEX_CLUT_X], h.clut, sizeof h.clut);
	for (uint32_t i = 0; i < h.num_rects; i++) {
		qtex_rect r;
		memcpy(&r, d + sizeof h + i * sizeof r, sizeof r);
		for (int y = 0; y < r.h; y++)
			memcpy(&vram[r.y + y][r.x], d + r.data_ofs + (size_t) y * r.w * 2, r.w * 2u);
	}
	free(d);
}

/* the map's texture table, and the record being drawn's window (GP0 E2h) */
static const qw_tex *textures;
static const qw_vert *pool;     /* the map's vertex pool */
static uint32_t      twin;

static void use_tex(const qw_record *r, int *tpage, int *clut)
{
	const qw_tex *t = &textures[r->tex];
	*tpage = t->tpage;
	*clut  = t->clut;
	twin   = t->window;
}

static void texel(int tpage, int clut, int u, int v, uint8_t out[3], int *op)
{
	int px = (tpage & 15) * 64, py = ((tpage >> 4) & 1) * 256;
	int cx = (clut & 63) * 16, cy = (clut >> 6) & 511;
	int mx = twin & 31, my = (twin >> 5) & 31, ox = (twin >> 10) & 31, oy = (twin >> 15) & 31;
	u &= 255;
	v &= 255;
	u = (u & ~(mx * 8)) | ((ox & mx) * 8);
	v = (v & ~(my * 8)) | ((oy & my) * 8);
	uint16_t w = vram[(py + (v & 255)) & 511][(px + (u & 255) / 2) & 1023];
	uint16_t c = vram[cy][(cx + ((w >> ((u & 1) * 8)) & 255)) & 1023];
	*op = c != 0;
	out[0] = (uint8_t) ((c & 31) << 3);
	out[1] = (uint8_t) (((c >> 5) & 31) << 3);
	out[2] = (uint8_t) (((c >> 10) & 31) << 3);
}

/* --- camera + raster ------------------------------------------------------- */

static double rot[3][3], cam[3];

typedef struct { double x, y, z; double u, v; } cv;   /* view space + uv */

static void to_view(const qw_vert *p, cv *o)
{
	double w[3] = { p->x / 4.0 - cam[0], p->y / 4.0 - cam[1], p->z / 4.0 - cam[2] };
	o->x = rot[0][0] * w[0] + rot[0][1] * w[1] + rot[0][2] * w[2];
	o->y = rot[1][0] * w[0] + rot[1][1] * w[1] + rot[1][2] * w[2];
	o->z = rot[2][0] * w[0] + rot[2][1] * w[1] + rot[2][2] * w[2];
}

#define ZNEAR 2.0

static void tri3(cv a, cv b, cv c, int tpage, int clut)
{
	/* clip against z = ZNEAR */
	cv in[3] = { a, b, c }, out[4];
	int n = 0;
	for (int i = 0; i < 3; i++) {
		cv *p = &in[i], *q = &in[(i + 1) % 3];
		if (p->z >= ZNEAR) out[n++] = *p;
		if ((p->z >= ZNEAR) != (q->z >= ZNEAR)) {
			double t = (ZNEAR - p->z) / (q->z - p->z);
			cv r = { p->x + t * (q->x - p->x), p->y + t * (q->y - p->y), ZNEAR,
			         p->u + t * (q->u - p->u), p->v + t * (q->v - p->v) };
			out[n++] = r;
		}
	}
	for (int k = 1; k + 1 < n; k++) {
		cv t[3] = { out[0], out[k], out[k + 1] };
		double sx[3], sy[3], iz[3];
		for (int i = 0; i < 3; i++) {
			iz[i] = 1.0 / t[i].z;
			sx[i] = 160 + t[i].x * 160 * iz[i];
			sy[i] = 120 + t[i].y * 160 * iz[i];
		}
		double area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
		if (fabs(area) < 1e-9) continue;
		int x0 = (int) floor(fmin(sx[0], fmin(sx[1], sx[2]))), x1 = (int) ceil(fmax(sx[0], fmax(sx[1], sx[2])));
		int y0 = (int) floor(fmin(sy[0], fmin(sy[1], sy[2]))), y1 = (int) ceil(fmax(sy[0], fmax(sy[1], sy[2])));
		if (x0 < 0) x0 = 0;
		if (y0 < 0) y0 = 0;
		if (x1 > W - 1) x1 = W - 1;
		if (y1 > H - 1) y1 = H - 1;
		for (int y = y0; y <= y1; y++)
			for (int x = x0; x <= x1; x++) {
				double px = x + 0.5, py = y + 0.5;
				double w0 = ((sx[1] - px) * (sy[2] - py) - (sy[1] - py) * (sx[2] - px)) / area;
				double w1 = ((sx[2] - px) * (sy[0] - py) - (sy[2] - py) * (sx[0] - px)) / area;
				double w2 = 1 - w0 - w1;
				if (w0 < 0 || w1 < 0 || w2 < 0) continue;
				double izp = w0 * iz[0] + w1 * iz[1] + w2 * iz[2];
				double z = 1.0 / izp;
				if (z >= zb[y][x]) continue;
				double u = (w0 * t[0].u * iz[0] + w1 * t[1].u * iz[1] + w2 * t[2].u * iz[2]) * z;
				double v = (w0 * t[0].v * iz[0] + w1 * t[1].v * iz[1] + w2 * t[2].v * iz[2]) * z;
				uint8_t col[3];
				int op;
				texel(tpage, clut, (int) u, (int) v, col, &op);
				if (!op) continue;
				zb[y][x] = (float) z;
				memcpy(fb[y][x], col, 3);
			}
	}
}

static void render(const uint8_t *d, const qw_header *h, const qw_view *vw)
{
	double yaw = vw->yaw * M_PI / 180, pitch = vw->pitch * M_PI / 180;
	double sy = sin(yaw), cy = cos(yaw), sp = sin(pitch), cp = cos(pitch);
	double m[3][3] = { { sy, -cy, 0 }, { -sp * cy, -sp * sy, -cp }, { cp * cy, cp * sy, -sp } };
	memcpy(rot, m, sizeof m);
	for (int k = 0; k < 3; k++) cam[k] = vw->pos[k];

	memset(fb, 0, sizeof fb);
	for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) zb[y][x] = 1e30f;

	const uint8_t *at = d + h->ofs_faces, *end = at + h->len_faces;
	while (at < end) {
		qw_record r;
		memcpy(&r, at, sizeof r);
		int tpage, clut;
		use_tex(&r, &tpage, &clut);
		const uint16_t *v = (const uint16_t *) (at + sizeof r);   /* pool indices */
		const uint8_t *q = at + r.polys;
		cv tv[QW_MAX_VERTS];
		for (int i = 0; i < r.num_verts; i++) to_view(&pool[v[i]], &tv[i]);
		for (int k = 0; k < r.num_quads; k++, q += sizeof(qw_quad)) {
			qw_quad qd;
			memcpy(&qd, q, sizeof qd);
			cv c[4];
			for (int i = 0; i < 4; i++) {
				c[i] = tv[qd.idx[i] / 4];
				c[i].u = qd.uv[i] & 255;
				c[i].v = qd.uv[i] >> 8;
			}
			tri3(c[0], c[1], c[2], tpage, clut);
			tri3(c[1], c[3], c[2], tpage, clut);
		}
		for (int k = 0; k < r.num_tris; k++, q += sizeof(qw_tri)) {
			qw_tri tr;
			memcpy(&tr, q, sizeof tr);
			cv c[3];
			for (int i = 0; i < 3; i++) {
				c[i] = tv[tr.idx[i] / 4];
				c[i].u = tr.uv[i] & 255;
				c[i].v = tr.uv[i] >> 8;
			}
			tri3(c[0], c[1], c[2], tpage, clut);
		}
		at += r.size;
	}
}

/* --- painter: the asm renderer's algorithm in C (debug reference) ---------- */
/* Same BSP walk as engine/r_world.s (PVS marks, face stamps from visible
   leaves, near child -> node block on the camera's side -> far child), no
   frustum culling; records are drawn back to front (reverse emission order)
   with NO depth buffer. Matches the z-buffer image iff the walk order is a
   valid painter's order. */

static const qw_node *P_nodes;
static const qw_leaf *P_leaves;
static uint8_t *P_leafvis, *P_nodevis, *P_face;
static const uint8_t **P_emit;
static int P_nemit;
static double P_cam4[3];

static double p_dist(const qw_node *n)
{
	if (n->type < 3)
		return P_cam4[n->type] * 4096.0 - n->dist;
	return n->n[0] * P_cam4[0] + n->n[1] * P_cam4[1] + n->n[2] * P_cam4[2] - n->dist;
}

static void p_walk_node(uint32_t ni, const uint8_t *d, const qw_header *h)
{
	const qw_node *n = &P_nodes[ni];
	if (!P_nodevis[ni]) return;
	double dd = p_dist(n);
	int side = dd < 0;
	uint32_t near = n->child[side], far = n->child[!side];
	const uint16_t *marks = (const uint16_t *) (d + h->ofs_marks);
	for (int pass = 0; pass < 2; pass++) {
		uint32_t c = pass ? far : near;
		if (pass == 1 && fabs(dd) > 164) {
			const uint8_t *fs = d + h->ofs_faces;
			uint32_t at = n->faces[side], end = n->faces[side + 1];
			while (at < end) {
				qw_record r;
				memcpy(&r, fs + at, sizeof r);
				if (P_face[r.face_id]) P_emit[P_nemit++] = fs + at;
				at += r.size;
			}
		}
		if (c & 0x80000000u) {
			uint32_t li = c & 0x7fffffffu;
			if (P_leafvis[li]) {
				const qw_leaf *l = &P_leaves[li];
				for (int k = 0; k < l->num_marks; k++) P_face[marks[l->marks + k]] = 1;
			}
		} else {
			p_walk_node(c, d, h);
		}
	}
}

static void render_painter(const uint8_t *d, const qw_header *h, const qw_view *vw)
{
	double yaw = vw->yaw * M_PI / 180, pitch = vw->pitch * M_PI / 180;
	double sy = sin(yaw), cy = cos(yaw), sp = sin(pitch), cp = cos(pitch);
	double m[3][3] = { { sy, -cy, 0 }, { -sp * cy, -sp * sy, -cp }, { cp * cy, cp * sy, -sp } };
	memcpy(rot, m, sizeof m);
	for (int k = 0; k < 3; k++) { cam[k] = vw->pos[k]; P_cam4[k] = vw->pos[k] * 4.0; }

	P_nodes = (const qw_node *) (d + h->ofs_nodes);
	P_leaves = (const qw_leaf *) (d + h->ofs_leaves);
	P_leafvis = calloc(h->num_leaves, 1);
	P_nodevis = calloc(h->num_nodes, 1);
	P_face = calloc(h->num_faces, 1);
	P_emit = calloc(h->num_records, sizeof *P_emit);
	P_nemit = 0;

	/* camera leaf */
	uint32_t at = h->root;
	while (!(at & 0x80000000u)) {
		const qw_node *n = &P_nodes[at];
		at = n->child[p_dist(n) <= 0];
	}
	const qw_leaf *vl = &P_leaves[at & 0x7fffffffu];
	/* PVS */
	int rowbytes = (h->num_visleafs + 7) >> 3;
	const uint8_t *in = vl->vis == QW_NONE ? NULL : d + h->ofs_vis + vl->vis;
	int leaf = 1;
	for (int b = 0; b < rowbytes; ) {
		int byte = 0xff, run = 1;
		if (in) {
			if (*in) { byte = *in++; }
			else { byte = 0; run = in[1]; in += 2; }
		}
		for (int r = 0; r < run && b < rowbytes; r++, b++)
			for (int bit = 0; bit < 8; bit++, leaf++)
				if ((byte >> bit) & 1 && leaf < (int) h->num_leaves) {
					P_leafvis[leaf] = 1;
					for (uint32_t p = P_leaves[leaf].parent; p != QW_NONE; p = P_nodes[p].parent) {
						if (P_nodevis[p]) break;
						P_nodevis[p] = 1;
					}
				}
	}
	p_walk_node(h->root, d, h);

	memset(fb, 0, sizeof fb);
	for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) zb[y][x] = 1e30f;
	for (int e = P_nemit - 1; e >= 0; e--) {        /* back to front */
		const uint8_t *rp = P_emit[e];
		qw_record r;
		memcpy(&r, rp, sizeof r);
		int tpage, clut;
		use_tex(&r, &tpage, &clut);
		const uint16_t *v = (const uint16_t *) (rp + sizeof r);
		const uint8_t *q = rp + r.polys;
		cv tv[QW_MAX_VERTS];
		for (int i = 0; i < r.num_verts; i++) to_view(&pool[v[i]], &tv[i]);
		for (int k = 0; k < r.num_quads; k++, q += sizeof(qw_quad)) {
			qw_quad qd; memcpy(&qd, q, sizeof qd);
			cv c[4];
			for (int i = 0; i < 4; i++) { c[i] = tv[qd.idx[i] / 4]; c[i].u = qd.uv[i] & 255; c[i].v = qd.uv[i] >> 8; }
			for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) zb[y][x] = 1e30f;   /* no depth test */
			tri3(c[0], c[1], c[2], tpage, clut);
			for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) zb[y][x] = 1e30f;
			tri3(c[1], c[3], c[2], tpage, clut);
		}
		for (int k = 0; k < r.num_tris; k++, q += sizeof(qw_tri)) {
			qw_tri tr; memcpy(&tr, q, sizeof tr);
			cv c[3];
			for (int i = 0; i < 3; i++) { c[i] = tv[tr.idx[i] / 4]; c[i].u = tr.uv[i] & 255; c[i].v = tr.uv[i] >> 8; }
			for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) zb[y][x] = 1e30f;
			tri3(c[0], c[1], c[2], tpage, clut);
		}
	}
	printf("wldref: painter view leaf %d, %d records emitted\n", (int) (vl - P_leaves), P_nemit);
	free(P_leafvis); free(P_nodevis); free(P_face); free(P_emit);
}

/* --- overlay: edges of the asm renderer's polygons (bench dump) ------------ */

static void plot(int x, int y)
{
	if (x >= 0 && x < W && y >= 0 && y < H) {
		fb[y][x][0] = 0; fb[y][x][1] = 255; fb[y][x][2] = 0;
	}
}

static void line(int x0, int y0, int x1, int y1)
{
	int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;
	for (int n = 0; n < 4096; n++) {
		plot(x0, y0);
		if (x0 == x1 && y0 == y1) break;
		int e2 = 2 * err;
		if (e2 >= dy) { err += dy; x0 += sx; }
		if (e2 <= dx) { err += dx; y0 += sy; }
	}
}

static void overlay(const char *log_path, uint32_t view)
{
	FILE *f = fopen(log_path, "r");
	if (!f) { fprintf(stderr, "wldref: cannot open %s\n", log_path); exit(1); }
	static char ln[1 << 16];
	int on = 0;
	while (fgets(ln, sizeof ln, f)) {
		if (!strncmp(ln, "D view ", 7)) { on = (uint32_t) atoi(ln + 7) == view; continue; }
		if (!on) continue;
		if (ln[0] == 'E') break;
		if (ln[0] != 'P') continue;
		uint32_t w[16];
		int n = 0;
		char *s = ln + 1;
		while (n < 16) { char *e; unsigned long x = strtoul(s, &e, 16); if (e == s) break; w[n++] = (uint32_t) x; s = e; }
		uint32_t code = w[0] >> 24;
		if ((code & 0xe4) != 0x24) continue;          /* textured flat polys */
		int nv = (code & 8) ? 4 : 3;
		int xs[4], ys[4];
		for (int i = 0; i < nv; i++) { xs[i] = (int16_t) (w[1 + 2 * i] & 0xffff); ys[i] = (int16_t) (w[1 + 2 * i] >> 16); }
		/* PS1 order: quad edges 0-1, 1-3, 3-2, 2-0 */
		static const int q[4][2] = { { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 } };
		static const int t[3][2] = { { 0, 1 }, { 1, 2 }, { 2, 0 } };
		for (int e = 0; e < nv; e++) {
			const int *p = nv == 4 ? q[e] : t[e];
			line(xs[p[0]], ys[p[0]], xs[p[1]], ys[p[1]]);
		}
	}
	fclose(f);
}

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: wldref <world.wld> <textures.tex> <out-prefix> [view [bench.log]]\n");
		return 2;
	}
	crc_init();
	load_tex(argv[2]);
	long len;
	uint8_t *d = slurp(argv[1], &len);
	qw_header h;
	memcpy(&h, d, sizeof h);
	if (memcmp(h.magic, QWORLD_MAGIC, 4)) { fprintf(stderr, "wldref: not a %s file\n", QWORLD_MAGIC); return 1; }
	textures = (const qw_tex *) (d + h.ofs_tex);
	pool     = (const qw_vert *) (d + h.ofs_verts);
	const qw_view *views = (const qw_view *) (d + h.ofs_views);
	for (uint32_t i = 0; i < h.num_views; i++) {
		if (argc > 4 && (uint32_t) atoi(argv[4]) != i) continue;
		if (getenv("WLDREF_PAINTER"))
			render_painter(d, &h, &views[i]);
		else
			render(d, &h, &views[i]);
		if (argc > 5)
			overlay(argv[5], i);
		char path[512];
		snprintf(path, sizeof path, "%s%u.png", argv[3], i);
		write_png(path);
		printf("wldref: %s\n", path);
	}
	free(d);
	return 0;
}
