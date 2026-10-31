/*
 * q1psx — asset cooker (host tool).
 *
 *   qcook map   --pak <pak0.pak> --map maps/start.bsp
 *               [--textures high --tex <out.tex> [--tex4 <out.tx4>]]
 *               [--geometry high --world <out.wld>] [--sounds <out.snd>]
 *               [--models high --mdl <out.mdl>]   (tools/qcook/sound.c, items.c)
 *   qcook check <file.wld>
 *   qcook game  --pak <pak0.pak> [--textures medium|high] --out <prefix>
 *                                                   (tools/qcook/game.c)
 *   qcook stream --pak <pak0.pak> [--pak1 <pak1.pak>] --out <stream.dat>
 *                                                     (tools/qcook/stream.c)
 *   qcook zones --world <map>_g0.wld --tables <map>.zones --skeleton <out.skl>
 *               [--resident n] [--tex4 <map>_t<N>.tx4 --chunks <out.zon>]...
 *                                                     (tools/qcook/zones.c)
 *
 * Reads the map straight from the pak (gfx/palette.lmp for the CLUT) and
 * writes the files the engine loads for that map: a texture set per
 * texture level, a world per geometry level, the map's own models per
 * model level (formats/qlevels.h); any of them in one run.
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

void buf_put(buf_t *b, const void *data, size_t n)
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

void buf_align4(buf_t *b)
{
	static const uint8_t z[4];
	buf_put(b, z, (4 - (b->len & 3)) & 3);
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

/* --------------------------------------------------------------- levels -- */

const char *const level_name[QL_LEVELS] = { "low", "medium", "high", "ultra" };

int level_find(const char *name, int mask, const char *axis)
{
	for (int i = 0; i < QL_LEVELS; i++)
		if (!strcmp(level_name[i], name) && (mask & 1 << i))
			return i;
	die("no %s level '%s'", axis, name);
}

const tex_level_t tex_levels[QL_LEVELS] = {
	/* shift  max rep                                                         */
	{   2,      32 },   /* low:    1/4 res                                     */
	{   1,      64 },   /* medium: 1/2 res                                     */
	{   0,     128 },   /* high:   native (<= 128)                             */
	{   0,     256 },   /* ultra:  native: the layout the world is cut for     */
};

/* ------------------------------------------------------------------- pak -- */

uint8_t *read_file(const char *path, long *len)
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
const uint8_t *pak_find(const uint8_t *pak, long pak_len, const char *name,
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
		for (int k = 0; k < 4; k++)
			d->ambient[k] = p[24 + k];
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
	const char *tex_name = arg(argc, argv, "--textures", "high");
	const char *geo_name = arg(argc, argv, "--geometry", "low");
	const char *mdl_name = arg(argc, argv, "--models", "low");
	const char *out_tex  = arg(argc, argv, "--tex", NULL);
	const char *out_tex4 = arg(argc, argv, "--tex4", NULL);
	const char *out_wld  = arg(argc, argv, "--world", NULL);
	const char *out_mdl  = arg(argc, argv, "--mdl", NULL);
	const char *pal_pak  = arg(argc, argv, "--palette-pak", NULL);
	const char *out_snd  = arg(argc, argv, "--sounds", NULL);
	const char *out_sset = arg(argc, argv, "--sndset", NULL);   /* (with --sounds: the level's sound set, §86) */
	light_gain           = atof(arg(argc, argv, "--light-gain", "1.66"));
	world_fmap           = arg(argc, argv, "--fmap", NULL);
	if (!pak_path || !map || (!out_tex && !out_tex4 && !out_wld && !out_mdl))
		die("usage: qcook map --pak <pak> --map <name> [--textures l [--tex <out>] [--tex4 <out>]]"
		    " [--geometry l --world <out>] [--models l --mdl <out>] [--palette-pak <pak0>]");

	int tex_level = level_find(tex_name, QL_TEXTURE_MASK, "texture");
	int geo_level = level_find(geo_name, QL_GEOMETRY_MASK, "geometry");
	int mdl_level = level_find(mdl_name, QL_MODEL_MASK, "model");

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
	printf("qcook: %s — %d nodes, %d leaves, %d faces, %d textures\n",
	       map, bsp.num_nodes, bsp.num_leaves, bsp.models[0].numfaces, bsp.num_miptex);

	/* the layout every level shares: the finest texture level's slots, the
	   models' textures as they are, the animations kept. The world's faces
	   are cut and the map's models mapped for it; a texture set maps it
	   onto its own tiles (formats/qtex.h qtex_uvmap). */
	tex_set    canon, ts;
	tex_extra *extra;
	int        nextra;
	items_prepare(&bsp, pak, pak_len, pal_pak ? ppak : NULL, ppak_len, pal, 0, &extra, &nextra);
	tex_build(&canon, NULL, &bsp, extra, nextra, pal, QL_ULTRA, NULL, NULL, 1, 0);

	if (out_tex || out_tex4) {
		printf("qcook: textures at %s\n", level_name[tex_level]);
		/* the pages full: the models' textures squeezed level by level,
		   then the same without the textures' animation (their frames and
		   display tiles: the faces show the chain's first frame) */
		for (int attempt = 0; ; attempt++) {
			int anims = attempt <= ITEMS_SQUEEZE_MAX, squeeze = anims ? attempt : attempt - ITEMS_SQUEEZE_MAX - 1;
			items_prepare(&bsp, pak, pak_len, pal_pak ? ppak : NULL, ppak_len, pal, squeeze, &extra, &nextra);
			if (tex_build(&ts, &canon, &bsp, extra, nextra, pal, tex_level, out_tex, out_tex4, anims,
			              attempt < 2 * ITEMS_SQUEEZE_MAX + 1))
				break;
			if (attempt == ITEMS_SQUEEZE_MAX)
				printf("qcook: the textures' animation left out\n");
			else
				printf("qcook: the models' textures squeezed to level %d\n", squeeze + 1);
		}
	}
	if (out_mdl) {
		printf("qcook: models at %s\n", level_name[mdl_level]);
		items_prepare(&bsp, pak, pak_len, pal_pak ? ppak : NULL, ppak_len, pal, 0, &extra, &nextra);
		items_cook(&canon, bsp.num_miptex, mdl_level);
		items_write(out_mdl);
	}
	if (out_snd)
		{
			char base[32];                            /* "maps/e1m1.bsp" -> "e1m1" */
			const char *sl = strrchr(map, '/');
			snprintf(base, sizeof base, "%s", sl ? sl + 1 : map);
			char *dot = strrchr(base, '.');
			if (dot)
				*dot = 0;
			sound_build(&bsp, pak, pak_len, pal_pak ? ppak : NULL, ppak_len, out_snd, base, out_sset);
		}
	if (!out_wld)
		return 0;
	printf("qcook: geometry at %s\n", level_name[geo_level]);
	world_build(&bsp, &canon, geo_level, out_wld);
	return world_check(out_wld);
}

int main(int argc, char **argv)
{
	if (argc >= 2 && !strcmp(argv[1], "map"))
		return cmd_map(argc, argv);
	if (argc == 3 && !strcmp(argv[1], "check"))
		return world_check(argv[2]);
	if (argc >= 2 && !strcmp(argv[1], "game"))
		return cmd_game(argc, argv);
	if (argc >= 2 && !strcmp(argv[1], "stream"))
		return cmd_stream(argc, argv);
	if (argc >= 3 && !strcmp(argv[1], "bmf"))
		return cmd_bmf(argc, argv);
	if (argc >= 2 && !strcmp(argv[1], "menu"))
		return cmd_menu(argc, argv);
	if (argc >= 2 && !strcmp(argv[1], "spumusic"))
		return cmd_spumusic(argc, argv);
	if (argc >= 2 && !strcmp(argv[1], "zones"))
		return cmd_zones(argc, argv);
	if (argc >= 2 && !strcmp(argv[1], "section"))
		return cmd_section(argc, argv);

	fprintf(stderr,
	        "usage:\n"
	        "  qcook map --pak <pak> --map <maps/x.bsp> [--palette-pak <pak0>] [--light-gain g]\n"
	        "            [--textures low|medium|high|ultra --tex <out.tex> [--tex4 <out.tx4>]]\n"
	        "            [--geometry low|high|ultra --world <out.wld>] [--sounds <out.snd>]\n"
	        "            [--models low|medium|high --mdl <out.mdl>]\n"
	        "  qcook check <file.wld>\n"
	        "  qcook game --pak <pak0.pak> [--textures medium|high] --out <prefix>   (<prefix>_<n>.dat per model level)\n"
	        "  qcook stream --pak <pak0.pak> [--pak1 <pak1.pak>] --out <stream.dat>\n"
	        "  qcook menu --pak <pak0.pak> --sounds <menu.snd> --conback <conback.dat>\n"
	        "  qcook spumusic --music <dir> --track <n> [--rate <hz>] --out <spu_nn.dat>\n"
	        "  qcook zones --world <wld> --tables <map.zones> --skeleton <out.skl> [--resident n]\n"
	        "              [--tex4 <tx4> --chunks <out.zon>]...\n");
	return 2;
}
