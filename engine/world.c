#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psxgpu.h>

#include "cd.h"
#include "mem.h"
#include "qtex.h"
#include "world.h"

world_t world;

const char *const fidelity_name[FIDELITY_LEVELS] = { "low", "medium", "high", "ultra" };

/* Every Quake map, in play order; the disc carries some of them
   (scripts/cook.sh: episode 1 by default, all with COOK_MAPS=all). */
static const char *const quake_maps[] = {
	"START", "E1M1", "E1M2", "E1M3", "E1M4", "E1M5", "E1M6", "E1M7", "E1M8",
	"E2M1", "E2M2", "E2M3", "E2M4", "E2M5", "E2M6", "E2M7",
	"E3M1", "E3M2", "E3M3", "E3M4", "E3M5", "E3M6", "E3M7",
	"E4M1", "E4M2", "E4M3", "E4M4", "E4M5", "E4M6", "E4M7", "E4M8", "END",
};
#define NUM_QUAKE_MAPS (int) (sizeof quake_maps / sizeof quake_maps[0])

static const char *disc_maps[NUM_QUAKE_MAPS];
static int         num_disc_maps;

static uint8_t map_on_disc[NUM_QUAKE_MAPS];
static int     fid_mask;          /* the fidelity levels the disc's maps come in */

/* cd_list_dir's call: a "<MAP>_<N>.WLD" marks its map present at level N
   (the release disc carries low to high, the ultra one ultra only) */
static void note_map_file(const char *name, uint32_t size)
{
	(void) size;
	const char *s = name;
	while (*s && *s != '_') s++;
	if (s[0] != '_' || s[1] < '0' || s[1] >= '0' + FIDELITY_LEVELS || strncmp(s + 2, ".WLD", 4))
		return;
	for (int i = 0; i < NUM_QUAKE_MAPS; i++)
		if (!strncmp(name, quake_maps[i], (size_t) (s - name)) && !quake_maps[i][s - name]) {
			map_on_disc[i] = 1;
			fid_mask |= 1 << (s[1] - '0');
		}
}

int world_fid_mask(void)
{
	return fid_mask ? fid_mask : (1 << FIDELITY_LEVELS) - 1;
}

int world_scan_maps(void)
{
	num_disc_maps = 0;
	fid_mask = 0;
	memset(map_on_disc, 0, sizeof map_on_disc);
	cd_list_dir("\\MAPS", note_map_file);         /* one directory read */
	for (int i = 0; i < NUM_QUAKE_MAPS; i++)
		if (map_on_disc[i])
			disc_maps[num_disc_maps++] = quake_maps[i];
	printf("world: %d maps on the disc\n", num_disc_maps);
	return num_disc_maps;
}

int world_num_maps(void)
{
	return num_disc_maps;
}

const char *world_map_name(int i)
{
	return i >= 0 && i < num_disc_maps ? disc_maps[i] : "?";
}

int world_find_map(const char *name)
{
	for (int i = 0; i < num_disc_maps; i++) {
		const char *a = disc_maps[i], *b = name;
		while (*a && *b && (*a | 0x20) == (*b | 0x20))
			a++, b++;
		if (!*a && !*b)
			return i;
	}
	return -1;
}

int world_registered(void)
{
	return world_find_map("E2M1") >= 0;
}

/* load progress: bytes of the files before the current one, and in all */
static world_progress_fn load_progress;
static uint32_t          load_base, load_total;

static void load_poll(uint32_t bytes)
{
	uint32_t done = load_base + bytes;
	load_progress(done < load_total ? done : load_total, load_total);
}

static void load_step(uint32_t base)
{
	load_base = base;
	if (load_progress)
		load_poll(0);
}

/* A QTX4 set's CLUT / tpage words, kept until the world file is loaded and
   its texture table can take them. */
#define TEX4_MAX 512
static qtex4_tex tex4_tab[TEX4_MAX];
static uint32_t  tex4_n;

/* LoadImage sends the rectangle's w x h halfwords by DMA in units of 16
   words, rounding a shorter last unit up: up to 60 bytes past the data go
   to the GPU too, as commands. A stray word made DuckStation's GPU (and
   would make the machine's) lose the command stream: the load hung on the
   next DrawSync; PCSX-Redux ignores them. A rectangle whose size is not a
   multiple of 64 bytes (a 4-bit CLUT row of an odd count of CLUTs: 32
   bytes each) goes up from a zero-padded copy (GP0 NOPs after the data). */
void vram_upload(const uint8_t *data, const qtex_rect *r)
{
	RECT rc = { (int16_t) r->x, (int16_t) r->y, (int16_t) r->w, (int16_t) r->h };
	uint32_t len = (uint32_t) r->w * r->h * 2;
	if (len & 63) {
		uint32_t mark = mem_level_mark();
		uint8_t *tmp = mem_level_alloc((len + 63) & ~63u);
		if (tmp) {
			memcpy(tmp, data, len);
			memset(tmp + len, 0, 64 - (len & 63));
			LoadImage(&rc, (const uint32_t *) tmp);
			DrawSync(0);                     /* (the DMA done before the copy goes) */
			mem_level_release(mark);
			return;
		}
	}
	LoadImage(&rc, (const uint32_t *) data);
}

static void upload_rects(const uint8_t *f, const qtex_rect *r, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		vram_upload(f + r[i].data_ofs, &r[i]);
}

/* The map's texture set into VRAM: QTX3 (8bpp, the Quake palette) or QTX4
   (4bpp, 16 colours per texture). */
static int load_textures(const char *name)
{
	uint32_t mark = mem_level_mark(), size;
	const uint8_t *f = cd_load(name, &size, load_progress ? load_poll : NULL);
	tex4_n = 0;
	if (f && size >= sizeof(qtex_header) && !memcmp(f, QTEX_MAGIC, 4)) {
		const qtex_header *h = (const qtex_header *) f;
		RECT clut = { QTEX_CLUT_X, QTEX_CLUT_Y, 256, 1 };
		LoadImage(&clut, (const uint32_t *) h->clut);
		upload_rects(f, (const qtex_rect *) (f + sizeof *h), h->num_rects);
		printf("world: %s — %u texture pages -> VRAM\n", name, h->num_rects);
	} else if (f && size >= sizeof(qtex4_header) && !memcmp(f, QTEX4_MAGIC, 4)) {
		const qtex4_header *h = (const qtex4_header *) f;
		upload_rects(f, (const qtex_rect *) (f + sizeof *h), h->num_rects);
		tex4_n = h->num_tex < TEX4_MAX ? h->num_tex : TEX4_MAX;
		memcpy(tex4_tab, f + h->ofs_tex, tex4_n * sizeof *tex4_tab);
		printf("world: %s — 4bpp, %u rects -> VRAM\n", name, h->num_rects);
	} else {
		printf("world: bad texture file %s\n", name);
		mem_level_release(mark);
		return 0;
	}
	DrawSync(0);
	mem_level_release(mark);   /* pixels live in VRAM now */
	return 1;
}

#define REL(base, ofs) ((uint32_t) (base) + (ofs))

int world_load(int map, int fid, int tex4, world_progress_fn progress)
{
	mem_level_reset();
	memset(&world, 0, sizeof world);
	if (map < 0 || map >= num_disc_maps)
		return 0;

	char name[24], tex_name[24];
	uint32_t tex_size = 0, wld_size = 0;
	snprintf(tex_name, sizeof tex_name, "\\MAPS\\%s_%d.TX4;1", disc_maps[map], fid);
	if (!tex4 || !cd_exists(tex_name, &tex_size))     /* (an older disc: 8 bits) */
		snprintf(tex_name, sizeof tex_name, "\\MAPS\\%s_%d.TEX;1", disc_maps[map], fid);
	snprintf(name, sizeof name, "\\MAPS\\%s_%d.WLD;1", disc_maps[map], fid);
	load_progress = progress;
	if (progress) {
		cd_exists(tex_name, &tex_size);
		cd_exists(name, &wld_size);
		load_total = tex_size + wld_size;
		load_step(0);
	}

	if (!load_textures(tex_name))
		return 0;

	load_step(tex_size);                 /* the pages are in VRAM */
	uint32_t size;
	uint8_t *f = cd_load(name, &size, progress ? load_poll : NULL);
	if (!f || size < sizeof(qw_header) || memcmp(f, QWORLD_MAGIC, 4)) {
		printf("world: bad world file %s\n", name);
		return 0;
	}
	const qw_header *h = (const qw_header *) f;

	world.nodes       = (qw_node *) (f + h->ofs_nodes);
	world.leaves      = (qw_leaf *) (f + h->ofs_leaves);
	world.marks       = (uint16_t *) (f + h->ofs_marks);
	for (uint32_t i = 0; i < h->num_marks; i++)
		world.marks[i] <<= 1;            /* face index -> byte offset in
		                                    face_visframe (faces < 32768) */
	world.faces       = f + h->ofs_faces;
	world.views       = (const qw_view *) (f + h->ofs_views);
	world.textures    = (const qw_tex *) (f + h->ofs_tex);
	world.num_tex     = h->num_tex;
	world.tex4        = tex4_n != 0;
	for (uint32_t i = 0; i < tex4_n && i < h->num_tex; i++) {   /* 4bpp: its CLUTs, pages */
		qw_tex *t = (qw_tex *) &world.textures[i];
		t->clut  = tex4_tab[i].clut;
		t->tpage = tex4_tab[i].tpage;
	}
	world.verts       = (const qw_vert *) (f + h->ofs_verts);
	world.num_verts   = h->num_verts;
	world.ambient     = f + h->ofs_ambient;
	world.num_nodes   = h->num_nodes;
	world.num_leaves  = h->num_leaves;
	world.num_faces   = h->num_faces;
	world.num_views   = h->num_views;
	world.num_records = h->num_records;
	world.num_pieces  = h->num_pieces;
	/* PVS rows: leaves 1..num_visleafs, which a malformed map could put
	   past the leaf array (mark_pvs stamps every leaf a row names) */
	world.vis_leaves  = h->num_visleafs < h->num_leaves ? h->num_visleafs
	                  : h->num_leaves ? h->num_leaves - 1 : 0;
	world.vis_rowbytes = (world.vis_leaves + 7) >> 3;
	world.root        = &world.nodes[h->root];
	world.has_spawn   = (int) h->has_spawn;
	world.map_name    = disc_maps[map];
	world.spawn_yaw   = h->spawn_yaw;
	memcpy(world.spawn, h->spawn, sizeof world.spawn);

	/* links: indices / offsets -> pointers (leaf pointers tagged with bit 0).
	   Child pointers go through KUSEG (0x00xxxxxx, the same RAM): the asm
	   walk ORs them into its stack words as they are, flags in the top byte. */
	const uint8_t *vis = f + h->ofs_vis;
	for (uint32_t i = 0; i < h->num_nodes; i++) {
		qw_node *n = &world.nodes[i];
		for (int c = 0; c < 2; c++) {
			uint32_t ch = n->child[c];
			n->child[c] = ((ch & 0x80000000u)
			            ? (uint32_t) &world.leaves[ch & 0x7fffffffu] | 1u
			            : (uint32_t) &world.nodes[ch]) & 0x00ffffffu;
		}
		n->visframe = 0;                 /* parents stay word offsets */
		for (int k = 0; k < 2; k++)
			n->faces[k] = REL(world.faces, n->faces[k]);
	}
	world.nodes[h->num_nodes].faces[0] = REL(world.faces, world.nodes[h->num_nodes].faces[0]);
	for (uint32_t i = 0; i < h->num_leaves; i++) {
		qw_leaf *l = &world.leaves[i];
		l->marks    = (uint32_t) &world.marks[l->marks];
		l->vis      = l->vis == QW_NONE ? 0 : REL(vis, l->vis) & 0x00ffffffu;   /* KUSEG: the top
		                                                                        byte holds the leaf's
		                                                                        render list (rlist.h) */
		l->visframe = 0;
	}

	if (h->ofs_clip && h->len_clip >= sizeof(qw_clip_header)) {
		const qw_clip_header *ch = (const qw_clip_header *) (f + h->ofs_clip);
		world.planes     = (const qw_plane *) (ch + 1);
		world.clipnodes  = (const qw_clipnode *) (world.planes + ch->num_planes);
		world.hull1_root = ch->hull1_root;
		world.hull2_root = ch->hull2_root;
	}

	/* the brush models (their roots like the children: KUSEG, bit 0 for a
	   leaf), the entities and their strings */
	world.bmodels      = (qw_bmodel *) (f + h->ofs_bmodels);
	world.num_bmodels  = h->num_bmodels;
	for (uint32_t i = 1; i < h->num_bmodels; i++) {
		uint32_t r = world.bmodels[i].root;
		world.bmodels[i].root = ((r & 0x80000000u)
		                      ? (uint32_t) &world.leaves[r & 0x7fffffffu] | 1u
		                      : (uint32_t) &world.nodes[r]) & 0x00ffffffu;
	}
	/* the map's own models (items, flames): their textures' words from the
	   table (the 4-bit set's when loaded), a table each for its records */
	world.num_models = h->num_models;
	world.mgeo       = f + h->ofs_mgeo;
	for (int i = 0; i < QIM_COUNT; i++)
		world.model_of[i] = -1;
	if (h->num_models) {
		const qw_model *qm = (const qw_model *) (f + h->ofs_models);
		world.models    = mem_level_alloc(h->num_models * sizeof *world.models);
		world.model_tex = mem_level_alloc(h->num_models * sizeof *world.model_tex);
		if (!world.models || !world.model_tex)
			return 0;
		for (uint32_t i = 0; i < h->num_models; i++) {
			world.models[i] = qm[i].m;
			qw_tex *t = mem_level_alloc(QW_MODEL_TEX * sizeof *t);
			if (!t)
				return 0;
			for (int k = 0; k < qm[i].num_tex && k < QW_MODEL_TEX; k++)
				t[k] = qm[i].tex[k] < world.num_tex ? world.textures[qm[i].tex[k]] : world.textures[0];
			if (!(world.models[i].flags & QGF_FLAT))           /* (an alias model: no wrap) */
				t[0].window = 0xE2000000u;
			world.models[i].tex = t[0];
			world.model_tex[i]  = t;
			if (qm[i].qim < QIM_COUNT)
				world.model_of[qm[i].qim] = (int16_t) i;
		}
	}

	world.anims        = (const qw_anim *) (f + h->ofs_anims);
	world.num_anims    = h->num_anims;
	world.ents         = (const qw_ent *) (f + h->ofs_ents);
	world.num_ents     = h->num_ents;
	world.strings      = (const char *) (f + h->ofs_strings);
	world.worldtype    = h->worldtype;
	world.title        = world.strings + h->title;
	world.max_bm_depth = h->max_bm_depth;

	world.face_visframe = mem_level_alloc(h->num_faces * 2);
	if (!world.face_visframe)
		return 0;
	memset(world.face_visframe, 0, h->num_faces * 2);

	/* each record's face index -> its byte offset in face_visframe (the
	   asm adds it to the array as it is; faces < 32768) */
	{
		uint8_t *r = (uint8_t *) world.faces;
		for (uint32_t i = 0; i < h->num_records; i++) {
			qw_record *rec = (qw_record *) r;
			if (!rec->size)
				break;
			rec->face_id = (uint16_t) (rec->face_id << 1);
			r += rec->size;
		}
	}

	printf("world: %s — %u nodes, %u leaves, %u records, %u polys, %u brush models, "
	       "%u entities, arena %u KB\n",
	       name, h->num_nodes, h->num_leaves, h->num_records, h->num_pieces,
	       h->num_bmodels ? h->num_bmodels - 1 : 0, h->num_ents, mem_level_used() / 1024);
	load_step(load_total);
	return 1;
}

/* the box (vertex units: centre c, half extents h) down the tree from a
   KUSEG node or leaf pointer */
static int box_leaves(uint32_t at, const int32_t c[3], const int32_t h[3], uint16_t *out,
                      int n, int max)
{
	for (;;) {
		if (at & 1) {
			const qw_leaf *l = (const qw_leaf *) ((at & ~1u) | 0x80000000u);
			if (l->contents != QW_CONTENTS_SOLID && n < max)
				out[n++] = (uint16_t) (l - world.leaves);
			return n;
		}
		const qw_node  *nd = (const qw_node *) (at | 0x80000000u);
		const qw_plane *pl = (const qw_plane *) ((const int32_t *) world.planes + nd->plane3);
		int32_t dist = pl->dist >> 1;    /* 1/8 units -> vertex units, x4096 */
		int32_t d, ext;
		if (pl->type < 3) {
			d   = c[pl->type] * 4096 - dist;
			ext = h[pl->type] * 4096;
		} else {
			d   = pl->n[0] * c[0] + pl->n[1] * c[1] + pl->n[2] * c[2] - dist;
			ext = (pl->n[0] < 0 ? -pl->n[0] : pl->n[0]) * h[0] +
			      (pl->n[1] < 0 ? -pl->n[1] : pl->n[1]) * h[1] +
			      (pl->n[2] < 0 ? -pl->n[2] : pl->n[2]) * h[2];
		}
		if (d - ext > 0) {
			at = nd->child[0];
		} else if (d + ext < 0) {
			at = nd->child[1];
		} else {
			n  = box_leaves(nd->child[0], c, h, out, n, max);
			at = nd->child[1];
		}
	}
}

int world_box_leaves(const int32_t mins[3], const int32_t maxs[3], uint16_t *out, int max)
{
	int32_t c[3], h[3];
	for (int k = 0; k < 3; k++) {
		/* 1/8 units -> vertex units (x4): the box rounded outwards */
		int32_t lo = mins[k] >> 1, hi = (maxs[k] + 1) >> 1;
		c[k] = (lo + hi) >> 1;
		h[k] = ((hi - lo) >> 1) + 1;
	}
	return box_leaves((uint32_t) world.root & 0x00ffffffu, c, h, out, 0, max);
}

const qw_leaf *world_point_leaf(const int32_t pos[3])
{
	int32_t p[3] = { pos[0] * QW_VSCALE, pos[1] * QW_VSCALE, pos[2] * QW_VSCALE };
	uint32_t at = (uint32_t) world.root;
	while (!(at & 1)) {
		const qw_node *n = (const qw_node *) at;
		const qw_plane *pl = (const qw_plane *) ((const int32_t *) world.planes + n->plane3);
		int32_t dist = pl->dist >> 1;    /* 1/8 units -> vertex units, x4096 */
		int32_t d = pl->type < 3
		          ? p[pl->type] * 4096 - dist
		          : pl->n[0] * p[0] + pl->n[1] * p[1] + pl->n[2] * p[2] - dist;
		at = n->child[d <= 0];   /* Quake: d > 0 is the front child */
	}
	return (const qw_leaf *) (at & ~1u);
}
