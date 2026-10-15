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

int world_scan_maps(void)
{
	num_disc_maps = 0;
	for (int i = 0; i < NUM_QUAKE_MAPS; i++) {
		char name[24];
		snprintf(name, sizeof name, "\\MAPS\\%s_0.WLD;1", quake_maps[i]);
		if (cd_exists(name, NULL))
			disc_maps[num_disc_maps++] = quake_maps[i];
	}
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

static int load_textures(const char *map, int fid)
{
	char name[24];
	snprintf(name, sizeof name, "\\MAPS\\%s_%d.TEX;1", map, fid);

	uint32_t mark = mem_level_mark(), size;
	const uint8_t *f = cd_load(name, &size, load_progress ? load_poll : NULL);
	if (!f || size < sizeof(qtex_header) || memcmp(f, QTEX_MAGIC, 4)) {
		printf("world: bad texture file %s\n", name);
		mem_level_release(mark);
		return 0;
	}
	const qtex_header *h = (const qtex_header *) f;
	const qtex_rect   *r = (const qtex_rect *) (f + sizeof *h);

	RECT clut = { QTEX_CLUT_X, QTEX_CLUT_Y, 256, 1 };
	LoadImage(&clut, (const uint32_t *) h->clut);
	for (uint32_t i = 0; i < h->num_rects; i++) {
		RECT rc = { (int16_t) r[i].x, (int16_t) r[i].y, (int16_t) r[i].w, (int16_t) r[i].h };
		LoadImage(&rc, (const uint32_t *) (f + r[i].data_ofs));
	}
	DrawSync(0);
	printf("world: %s — %u texture pages -> VRAM\n", name, h->num_rects);

	mem_level_release(mark);   /* pixels live in VRAM now */
	return 1;
}

#define REL(base, ofs) ((uint32_t) (base) + (ofs))

int world_load(int map, int fid, world_progress_fn progress)
{
	mem_level_reset();
	memset(&world, 0, sizeof world);
	if (map < 0 || map >= num_disc_maps)
		return 0;

	char name[24], tex_name[24];
	snprintf(tex_name, sizeof tex_name, "\\MAPS\\%s_%d.TEX;1", disc_maps[map], fid);
	snprintf(name, sizeof name, "\\MAPS\\%s_%d.WLD;1", disc_maps[map], fid);
	uint32_t tex_size = 0, wld_size = 0;
	load_progress = progress;
	if (progress) {
		cd_exists(tex_name, &tex_size);
		cd_exists(name, &wld_size);
		load_total = tex_size + wld_size;
		load_step(0);
	}

	if (!load_textures(disc_maps[map], fid))
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
	world.faces       = f + h->ofs_faces;
	world.views       = (const qw_view *) (f + h->ofs_views);
	world.textures    = (const qw_tex *) (f + h->ofs_tex);
	world.num_tex     = h->num_tex;
	world.verts       = (const qw_vert *) (f + h->ofs_verts);
	world.num_verts   = h->num_verts;
	world.num_nodes   = h->num_nodes;
	world.num_leaves  = h->num_leaves;
	world.num_faces   = h->num_faces;
	world.num_views   = h->num_views;
	world.num_records = h->num_records;
	world.num_pieces  = h->num_pieces;
	world.vis_rowbytes = (h->num_visleafs + 7) >> 3;
	world.root        = &world.nodes[h->root];
	world.has_spawn   = (int) h->has_spawn;
	world.spawn_yaw   = h->spawn_yaw;
	memcpy(world.spawn, h->spawn, sizeof world.spawn);

	/* links: indices / offsets -> pointers (leaf pointers tagged with bit 0) */
	const uint8_t *vis = f + h->ofs_vis;
	for (uint32_t i = 0; i < h->num_nodes; i++) {
		qw_node *n = &world.nodes[i];
		for (int c = 0; c < 2; c++) {
			uint32_t ch = n->child[c];
			n->child[c] = (ch & 0x80000000u)
			            ? (uint32_t) &world.leaves[ch & 0x7fffffffu] | 1u
			            : (uint32_t) &world.nodes[ch];
		}
		n->visframe = 0;                 /* parents stay word offsets */
		for (int k = 0; k < 2; k++)
			n->faces[k] = REL(world.faces, n->faces[k]);
	}
	world.nodes[h->num_nodes].faces[0] = REL(world.faces, world.nodes[h->num_nodes].faces[0]);
	for (uint32_t i = 0; i < h->num_leaves; i++) {
		qw_leaf *l = &world.leaves[i];
		l->marks    = (uint32_t) &world.marks[l->marks];
		l->vis      = l->vis == QW_NONE ? 0 : REL(vis, l->vis);
		l->visframe = 0;
	}

	if (h->ofs_clip && h->len_clip >= sizeof(qw_clip_header)) {
		const qw_clip_header *ch = (const qw_clip_header *) (f + h->ofs_clip);
		world.planes     = (const qw_plane *) (ch + 1);
		world.clipnodes  = (const qw_clipnode *) (world.planes + ch->num_planes);
		world.hull1_root = ch->hull1_root;
		world.hull2_root = ch->hull2_root;
	}

	world.face_visframe = mem_level_alloc(h->num_faces * 2);
	if (!world.face_visframe)
		return 0;
	memset(world.face_visframe, 0, h->num_faces * 2);

	printf("world: %s — %u nodes, %u leaves, %u records, %u polys, arena %u KB\n",
	       name, h->num_nodes, h->num_leaves, h->num_records, h->num_pieces,
	       mem_level_used() / 1024);
	load_step(load_total);
	return 1;
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
