/*
 * q1psx — asset cooker (host tool).
 *
 *   qcook map   --pak <pak0.pak> --map maps/start.bsp [--fidelity high]
 *               --tex <out.tex> --world <out.wld>
 *   qcook check <file.wld>
 *
 * Reads the map straight from the pak (gfx/palette.lmp for the CLUT) and
 * writes the two files the engine loads for that map + fidelity level.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qcook.h"

/* ------------------------------------------------------------------ util -- */

void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "qcook: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	exit(1);
}

void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p)
		die("out of memory (%zu bytes)", n);
	return p;
}

void *xcalloc(size_t n, size_t size)
{
	void *p = calloc(n ? n : 1, size ? size : 1);
	if (!p)
		die("out of memory (%zu x %zu bytes)", n, size);
	return p;
}

static uint32_t rd_u32(const uint8_t *p)
{
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
	       ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}
static int32_t  rd_i32(const uint8_t *p) { return (int32_t) rd_u32(p); }
static uint16_t rd_u16(const uint8_t *p) { return (uint16_t) (p[0] | (p[1] << 8)); }
static int16_t  rd_i16(const uint8_t *p) { return (int16_t) rd_u16(p); }
static float    rd_f32(const uint8_t *p) { uint32_t u = rd_u32(p); float f; memcpy(&f, &u, 4); return f; }

/* ------------------------------------------------------------- fidelity -- */

static const fidelity_t FIDELITY[] = {
	/* name      shift  max rep  tile min                                     */
	{ "low",       2,     32,      64 },   /* 1/4 res, small textures tiled  */
	{ "medium",    1,     64,      64 },   /* 1/2 res, small textures tiled  */
	{ "high",      0,    128,       0 },   /* native (<= 128), one rep/tile  */
	{ "ultra",     0,    256,       0 },   /* native                          */
};

const fidelity_t *fidelity_find(const char *name)
{
	for (size_t i = 0; i < sizeof FIDELITY / sizeof FIDELITY[0]; i++)
		if (!strcmp(FIDELITY[i].name, name))
			return &FIDELITY[i];
	die("unknown fidelity '%s' (low|medium|high|ultra)", name);
}

/* ------------------------------------------------------------------- pak -- */

static uint8_t *read_file(const char *path, long *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		die("cannot open %s", path);
	fseek(f, 0, SEEK_END);
	*len = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *buf = xmalloc((size_t) *len);
	if (fread(buf, 1, (size_t) *len, f) != (size_t) *len)
		die("short read on %s", path);
	fclose(f);
	return buf;
}

/* Find `name` in a PACK file (case-insensitive: Steam paks are upper case). */
static const uint8_t *pak_find(const uint8_t *pak, long pak_len, const char *name,
                               uint32_t *len)
{
	if (pak_len < 12 || memcmp(pak, "PACK", 4))
		die("not a PACK file");
	uint32_t dirofs = rd_u32(pak + 4), dirlen = rd_u32(pak + 8);
	if ((uint64_t) dirofs + dirlen > (uint64_t) pak_len)
		die("pak directory out of range");
	for (uint32_t i = 0; i < dirlen / 64; i++) {
		const uint8_t *e = pak + dirofs + i * 64;
		char n[57];
		memcpy(n, e, 56);
		n[56] = 0;
		if (!strcasecmp(n, name)) {
			uint32_t ofs = rd_u32(e + 56), l = rd_u32(e + 60);
			if ((uint64_t) ofs + l > (uint64_t) pak_len)
				die("pak entry %s out of range", name);
			*len = l;
			return pak + ofs;
		}
	}
	return NULL;
}

/* ------------------------------------------------------------------- bsp -- */

#define BSPVERSION 29
enum { L_ENTITIES, L_PLANES, L_TEXTURES, L_VERTEXES, L_VISIBILITY, L_NODES,
       L_TEXINFO, L_FACES, L_LIGHTING, L_CLIPNODES, L_LEAFS, L_MARKSURFACES,
       L_EDGES, L_SURFEDGES, L_MODELS, L_COUNT };

static const uint8_t *lump(const bsp_t *b, int idx, uint32_t *len, uint32_t elem)
{
	const uint8_t *l = b->raw + 4 + idx * 8;
	uint32_t ofs = rd_u32(l), n = rd_u32(l + 4);
	if ((uint64_t) ofs + n > b->len)
		die("bsp lump %d out of range", idx);
	if (elem && n % elem)
		die("bsp lump %d: size %u not a multiple of %u", idx, n, elem);
	*len = n;
	return b->raw + ofs;
}

void bsp_parse(bsp_t *b, const uint8_t *raw, uint32_t len)
{
	memset(b, 0, sizeof *b);
	b->raw = raw;
	b->len = len;
	if (len < 4 + L_COUNT * 8 || rd_u32(raw) != BSPVERSION)
		die("not a BSP29 file");

	uint32_t n;
	const uint8_t *p;

	p = lump(b, L_PLANES, &n, 20);
	b->num_planes = (int) (n / 20);
	b->planes = xcalloc((size_t) b->num_planes, sizeof *b->planes);
	for (int i = 0; i < b->num_planes; i++, p += 20) {
		for (int k = 0; k < 3; k++)
			b->planes[i].n[k] = rd_f32(p + k * 4);
		b->planes[i].dist = rd_f32(p + 12);
		b->planes[i].type = rd_i32(p + 16);
	}

	p = lump(b, L_NODES, &n, 24);
	b->num_nodes = (int) (n / 24);
	b->nodes = xcalloc((size_t) b->num_nodes, sizeof *b->nodes);
	for (int i = 0; i < b->num_nodes; i++, p += 24) {
		bsp_node *d = &b->nodes[i];
		d->planenum = rd_i32(p);
		d->child[0] = rd_i16(p + 4);
		d->child[1] = rd_i16(p + 6);
		for (int k = 0; k < 3; k++) {
			d->mins[k] = rd_i16(p + 8 + k * 2);
			d->maxs[k] = rd_i16(p + 14 + k * 2);
		}
		d->firstface = rd_u16(p + 20);
		d->numfaces  = rd_u16(p + 22);
	}

	p = lump(b, L_LEAFS, &n, 28);
	b->num_leaves = (int) (n / 28);
	b->leaves = xcalloc((size_t) b->num_leaves, sizeof *b->leaves);
	for (int i = 0; i < b->num_leaves; i++, p += 28) {
		bsp_leaf *d = &b->leaves[i];
		d->contents = rd_i32(p);
		d->visofs   = rd_i32(p + 4);
		for (int k = 0; k < 3; k++) {
			d->mins[k] = rd_i16(p + 8 + k * 2);
			d->maxs[k] = rd_i16(p + 14 + k * 2);
		}
		d->firstmark = rd_u16(p + 20);
		d->nummarks  = rd_u16(p + 22);
	}

	p = lump(b, L_TEXINFO, &n, 40);
	b->num_texinfo = (int) (n / 40);
	b->texinfo = xcalloc((size_t) b->num_texinfo, sizeof *b->texinfo);
	for (int i = 0; i < b->num_texinfo; i++, p += 40) {
		for (int k = 0; k < 8; k++)
			b->texinfo[i].vecs[k / 4][k % 4] = rd_f32(p + k * 4);
		b->texinfo[i].miptex = rd_i32(p + 32);
		b->texinfo[i].flags  = rd_i32(p + 36);
	}

	p = lump(b, L_FACES, &n, 20);
	b->num_faces = (int) (n / 20);
	b->faces = xcalloc((size_t) b->num_faces, sizeof *b->faces);
	for (int i = 0; i < b->num_faces; i++, p += 20) {
		bsp_face *d = &b->faces[i];
		d->planenum  = rd_i16(p);
		d->side      = rd_i16(p + 2);
		d->firstedge = rd_i32(p + 4);
		d->numedges  = rd_i16(p + 8);
		d->texinfo   = rd_i16(p + 10);
		memcpy(d->styles, p + 12, 4);
		d->lightofs  = rd_i32(p + 16);
	}

	p = lump(b, L_CLIPNODES, &n, 8);
	b->num_clipnodes = (int) (n / 8);
	b->clipnodes = xcalloc((size_t) b->num_clipnodes, sizeof *b->clipnodes);
	for (int i = 0; i < b->num_clipnodes; i++, p += 8) {
		b->clipnodes[i].planenum = rd_i32(p);
		b->clipnodes[i].child[0] = rd_i16(p + 4);
		b->clipnodes[i].child[1] = rd_i16(p + 6);
	}

	p = lump(b, L_MODELS, &n, 64);
	b->num_models = (int) (n / 64);
	b->models = xcalloc((size_t) b->num_models, sizeof *b->models);
	for (int i = 0; i < b->num_models; i++, p += 64) {
		bsp_model *d = &b->models[i];
		for (int k = 0; k < 3; k++) {
			d->mins[k] = rd_f32(p + k * 4);
			d->maxs[k] = rd_f32(p + 12 + k * 4);
		}
		for (int k = 0; k < 4; k++)
			d->headnode[k] = rd_i32(p + 36 + k * 4);
		d->visleafs  = rd_i32(p + 52);
		d->firstface = rd_i32(p + 56);
		d->numfaces  = rd_i32(p + 60);
	}
	if (b->num_models < 1)
		die("bsp has no world model");

	p = lump(b, L_VERTEXES, &n, 12);
	b->num_verts = (int) (n / 12);
	b->verts = xcalloc((size_t) b->num_verts * 3, sizeof(float));
	for (int i = 0; i < b->num_verts * 3; i++)
		b->verts[i] = rd_f32(p + i * 4);

	p = lump(b, L_SURFEDGES, &n, 4);
	b->num_surfedges = (int) (n / 4);
	b->surfedges = xcalloc((size_t) b->num_surfedges, sizeof(int));
	for (int i = 0; i < b->num_surfedges; i++)
		b->surfedges[i] = rd_i32(p + i * 4);

	p = lump(b, L_EDGES, &n, 4);
	b->num_edges = (int) (n / 4);
	b->edges = xcalloc((size_t) b->num_edges * 2, sizeof(uint16_t));
	for (int i = 0; i < b->num_edges * 2; i++)
		b->edges[i] = rd_u16(p + i * 2);

	p = lump(b, L_MARKSURFACES, &n, 2);
	b->num_marks = (int) (n / 2);
	b->marks = xcalloc((size_t) b->num_marks, sizeof(uint16_t));
	for (int i = 0; i < b->num_marks; i++)
		b->marks[i] = rd_u16(p + i * 2);

	b->vis = lump(b, L_VISIBILITY, &b->vis_len, 0);
	b->lighting = lump(b, L_LIGHTING, &b->light_len, 0);
	b->entities = (const char *) lump(b, L_ENTITIES, &b->ent_len, 0);

	/* miptex: all four box-filtered mip levels */
	uint32_t tl;
	const uint8_t *t = lump(b, L_TEXTURES, &tl, 0);
	b->num_miptex = tl >= 4 ? (int) rd_u32(t) : 0;
	b->miptex = xcalloc((size_t) b->num_miptex, sizeof *b->miptex);
	for (int i = 0; i < b->num_miptex; i++) {
		bsp_miptex *m = &b->miptex[i];
		int32_t ofs = rd_i32(t + 4 + i * 4);
		if (ofs < 0 || (uint32_t) ofs + 40 > tl) {
			snprintf(m->name, sizeof m->name, "missing%d", i & 0xffff);
			continue;
		}
		const uint8_t *mt = t + ofs;
		memcpy(m->name, mt, 16);
		m->name[15] = 0;
		int w = (int) rd_u32(mt + 16), h = (int) rd_u32(mt + 20);
		uint32_t pofs[4];
		for (int k = 0; k < 4; k++)
			pofs[k] = rd_u32(mt + 24 + k * 4);
		if (w <= 0 || h <= 0 || w > 1024 || h > 1024 ||
		    (uint64_t) ofs + pofs[0] + (uint64_t) w * h > tl)
			continue;
		m->w = w;
		m->h = h;
		for (int k = 0; k < 4; k++)
			m->mip[k] = ((uint64_t) ofs + pofs[k] + (uint64_t) (w >> k) * (h >> k) <= tl)
			          ? mt + pofs[k] : NULL;
	}
}

/* ------------------------------------------------------------------ main -- */

static const char *arg(int argc, char **argv, const char *key, const char *def)
{
	for (int i = 2; i < argc - 1; i++)
		if (!strcmp(argv[i], key))
			return argv[i + 1];
	return def;
}

static int cmd_map(int argc, char **argv)
{
	const char *pak_path = arg(argc, argv, "--pak", NULL);
	const char *map      = arg(argc, argv, "--map", NULL);
	const char *fid_name = arg(argc, argv, "--fidelity", "high");
	const char *out_tex  = arg(argc, argv, "--tex", NULL);
	const char *out_wld  = arg(argc, argv, "--world", NULL);
	const char *pal_pak  = arg(argc, argv, "--palette-pak", NULL);
	light_gain           = atof(arg(argc, argv, "--light-gain", "1"));
	if (!pak_path || !map || !out_tex || !out_wld)
		die("usage: qcook map --pak <pak> --map <name> [--fidelity f] --tex <out> --world <out>"
		    " [--palette-pak <pak0>]");

	const fidelity_t *fid = fidelity_find(fid_name);

	long pak_len;
	uint8_t *pak = read_file(pak_path, &pak_len);

	/* the retail pak1.pak has maps but no palette: take it from pak0 */
	uint32_t pal_len, bsp_len;
	long     ppak_len = pak_len;
	uint8_t *ppak     = pal_pak ? read_file(pal_pak, &ppak_len) : pak;
	const uint8_t *pal = pak_find(ppak, ppak_len, "gfx/palette.lmp", &pal_len);
	if (!pal || pal_len < 768)
		die("gfx/palette.lmp missing in %s", pal_pak ? pal_pak : pak_path);
	const uint8_t *raw = pak_find(pak, pak_len, map, &bsp_len);
	if (!raw)
		die("%s not found in %s", map, pak_path);

	bsp_t bsp;
	bsp_parse(&bsp, raw, bsp_len);
	printf("qcook: %s @ %s — %d nodes, %d leaves, %d faces, %d textures\n",
	       map, fid->name, bsp.num_nodes, bsp.num_leaves,
	       bsp.models[0].numfaces, bsp.num_miptex);

	tex_set ts;
	tex_build(&ts, &bsp, pal, fid, out_tex);
	world_build(&bsp, &ts, fid, out_wld);
	return world_check(out_wld);
}

int main(int argc, char **argv)
{
	if (argc >= 2 && !strcmp(argv[1], "map"))
		return cmd_map(argc, argv);
	if (argc == 3 && !strcmp(argv[1], "check"))
		return world_check(argv[2]);

	fprintf(stderr,
	        "usage:\n"
	        "  qcook map --pak <pak> --map <maps/x.bsp> [--fidelity low|medium|high|ultra]\n"
	        "            --tex <out.tex> --world <out.wld>\n"
	        "  qcook check <file.wld>\n");
	return 2;
}
