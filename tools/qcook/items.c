/*
 * q1psx — `qcook map`: the map's own models (formats/qgame.h qw_model,
 * QIM_*): the items its entities spawn with (items.qc's setmodel), the
 * torches' flames, the fireballs, the explosive boxes. Alias models go
 * through game.c (their skins reduced, placed among the map's textures:
 * tex_build's extras), the BSP item boxes (maps/b_*.bsp) are cut into
 * records over their own textures (extras too, tiling like the world's).
 * Only what the entities use is cooked.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qcook.h"
#include "../../formats/qgame.h"

typedef struct {
	const char *file;
	int         skin, div, flat;
} qim_spec;

/* skins at a quarter of their resolution (the projectiles' in GAME.DAT:
   small and seldom near); the flames at a half (fullbright: flat) */
static const qim_spec QIMS[QIM_COUNT] = {
	[QIM_G_SHOT]   = { "progs/g_shot.mdl",   0, 4, 0 },
	[QIM_G_NAIL]   = { "progs/g_nail.mdl",   0, 4, 0 },
	[QIM_G_NAIL2]  = { "progs/g_nail2.mdl",  0, 4, 0 },
	[QIM_G_ROCK]   = { "progs/g_rock.mdl",   0, 4, 0 },
	[QIM_G_ROCK2]  = { "progs/g_rock2.mdl",  0, 4, 0 },
	[QIM_G_LIGHT]  = { "progs/g_light.mdl",  0, 4, 0 },
	[QIM_ARMOR1]   = { "progs/armor.mdl",    0, 4, 0 },
	[QIM_ARMOR2]   = { "progs/armor.mdl",    1, 4, 0 },
	[QIM_ARMOR3]   = { "progs/armor.mdl",    2, 4, 0 },
	[QIM_W_S_KEY]  = { "progs/w_s_key.mdl",  0, 4, 0 },
	[QIM_W_G_KEY]  = { "progs/w_g_key.mdl",  0, 4, 0 },
	[QIM_M_S_KEY]  = { "progs/m_s_key.mdl",  0, 4, 0 },
	[QIM_M_G_KEY]  = { "progs/m_g_key.mdl",  0, 4, 0 },
	[QIM_B_S_KEY]  = { "progs/b_s_key.mdl",  0, 4, 0 },
	[QIM_B_G_KEY]  = { "progs/b_g_key.mdl",  0, 4, 0 },
	[QIM_INVULNER] = { "progs/invulner.mdl", 0, 4, 0 },
	[QIM_SUIT]     = { "progs/suit.mdl",     0, 4, 0 },
	[QIM_INVISIBL] = { "progs/invisibl.mdl", 0, 4, 0 },
	[QIM_QUADDAMA] = { "progs/quaddama.mdl", 0, 4, 0 },
	[QIM_END1]     = { "progs/end1.mdl",     0, 4, 0 },
	[QIM_END2]     = { "progs/end2.mdl",     0, 4, 0 },
	[QIM_END3]     = { "progs/end3.mdl",     0, 4, 0 },
	[QIM_END4]     = { "progs/end4.mdl",     0, 4, 0 },
	[QIM_FLAME]    = { "progs/flame.mdl",    0, 2, 1 },
	[QIM_FLAME2]   = { "progs/flame2.mdl",   0, 2, 1 },
	[QIM_LAVABALL] = { "progs/lavaball.mdl", 0, 4, 1 },
	[QIM_TELEPORT] = { "progs/teleport.mdl", 0, 4, 1 },
	[QIM_B_BH10]   = { "maps/b_bh10.bsp",   0, 0, 1 },
	[QIM_B_BH25]   = { "maps/b_bh25.bsp",   0, 0, 1 },
	[QIM_B_BH100]  = { "maps/b_bh100.bsp",  0, 0, 1 },
	[QIM_B_SHELL0] = { "maps/b_shell0.bsp", 0, 0, 1 },
	[QIM_B_SHELL1] = { "maps/b_shell1.bsp", 0, 0, 1 },
	[QIM_B_NAIL0]  = { "maps/b_nail0.bsp",  0, 0, 1 },
	[QIM_B_NAIL1]  = { "maps/b_nail1.bsp",  0, 0, 1 },
	[QIM_B_ROCK0]  = { "maps/b_rock0.bsp",  0, 0, 1 },
	[QIM_B_ROCK1]  = { "maps/b_rock1.bsp",  0, 0, 1 },
	[QIM_B_BATT0]  = { "maps/b_batt0.bsp",  0, 0, 1 },
	[QIM_B_BATT1]  = { "maps/b_batt1.bsp",  0, 0, 1 },
	[QIM_B_EXPLOB] = { "maps/b_explob.bsp", 0, 0, 1 },
	[QIM_B_EXBOX2] = { "maps/b_exbox2.bsp", 0, 0, 1 },
};

static int     need[QIM_COUNT];
static qc_mdl *mdls[QIM_COUNT];
static bsp_t   bsps[QIM_COUNT];
static int     extra_of[QIM_COUNT];               /* a model's skin: its extra */
static int     bsp_extra[QIM_COUNT][QW_MODEL_TEX];   /* a box's textures: its extras */
static int     box_miptex[QIM_COUNT][QW_MODEL_TEX];  /*   ... and their miptex */
static int     bsp_ntex[QIM_COUNT];
static tex_extra *extras;
static int     nextras;

static buf_t   geo, table;
static int     nmodels;

static int is_bsp(int q)
{
	return strstr(QIMS[q].file, ".bsp") != 0;
}

/* items.qc / misc.qc: the models the entity's spawn function sets */
static void need_for(const ent_list *l, const ent_rec *e, int worldtype)
{
	const char *c = e->classname;
	int sf = (int) ent_float(l, e, "spawnflags", 0);
	if (!strcmp(c, "item_health"))
		need[sf & 1 ? QIM_B_BH10 : sf & 2 ? QIM_B_BH100 : QIM_B_BH25] = 1;
	else if (!strcmp(c, "item_armor1"))   need[QIM_ARMOR1] = 1;
	else if (!strcmp(c, "item_armor2"))   need[QIM_ARMOR2] = 1;
	else if (!strcmp(c, "item_armorInv")) need[QIM_ARMOR3] = 1;
	else if (!strcmp(c, "item_shells"))   need[sf & 1 ? QIM_B_SHELL1 : QIM_B_SHELL0] = 1;
	else if (!strcmp(c, "item_spikes"))   need[sf & 1 ? QIM_B_NAIL1 : QIM_B_NAIL0] = 1;
	else if (!strcmp(c, "item_rockets"))  need[sf & 1 ? QIM_B_ROCK1 : QIM_B_ROCK0] = 1;
	else if (!strcmp(c, "item_cells"))    need[sf & 1 ? QIM_B_BATT1 : QIM_B_BATT0] = 1;
	else if (!strcmp(c, "item_weapon")) {           /* the old ammo item */
		int big = sf & 8;
		if (sf & 1) need[big ? QIM_B_SHELL1 : QIM_B_SHELL0] = 1;
		if (sf & 4) need[big ? QIM_B_NAIL1 : QIM_B_NAIL0] = 1;
		if (sf & 2) need[big ? QIM_B_ROCK1 : QIM_B_ROCK0] = 1;
	}
	else if (!strcmp(c, "weapon_supershotgun"))    need[QIM_G_SHOT] = 1;
	else if (!strcmp(c, "weapon_nailgun"))         need[QIM_G_NAIL] = 1;
	else if (!strcmp(c, "weapon_supernailgun"))    need[QIM_G_NAIL2] = 1;
	else if (!strcmp(c, "weapon_grenadelauncher")) need[QIM_G_ROCK] = 1;
	else if (!strcmp(c, "weapon_rocketlauncher"))  need[QIM_G_ROCK2] = 1;
	else if (!strcmp(c, "weapon_lightning"))       need[QIM_G_LIGHT] = 1;
	else if (!strcmp(c, "item_key1"))
		need[worldtype == 2 ? QIM_B_S_KEY : worldtype == 1 ? QIM_M_S_KEY : QIM_W_S_KEY] = 1;
	else if (!strcmp(c, "item_key2"))
		need[worldtype == 2 ? QIM_B_G_KEY : worldtype == 1 ? QIM_M_G_KEY : QIM_W_G_KEY] = 1;
	else if (!strcmp(c, "item_sigil"))
		need[sf & 2 ? QIM_END2 : sf & 4 ? QIM_END3 : sf & 8 ? QIM_END4 : QIM_END1] = 1;
	else if (!strcmp(c, "item_artifact_invulnerability")) need[QIM_INVULNER] = 1;
	else if (!strcmp(c, "item_artifact_envirosuit"))      need[QIM_SUIT] = 1;
	else if (!strcmp(c, "item_artifact_invisibility"))    need[QIM_INVISIBL] = 1;
	else if (!strcmp(c, "item_artifact_super_damage"))    need[QIM_QUADDAMA] = 1;
	else if (!strcmp(c, "light_torch_small_walltorch"))   need[QIM_FLAME] = 1;
	else if (!strncmp(c, "light_flame_", 12))             need[QIM_FLAME2] = 1;
	else if (!strcmp(c, "misc_fireball"))      need[QIM_LAVABALL] = 1;
	else if (!strcmp(c, "misc_explobox"))      need[QIM_B_EXPLOB] = 1;
	else if (!strcmp(c, "misc_explobox2"))     need[QIM_B_EXBOX2] = 1;
	else if (!strcmp(c, "misc_teleporttrain")) need[QIM_TELEPORT] = 1;
}

/* an extra texture; a box's identical to one already there (the same
   name, size and texels: the ammo boxes' rockettop) shared; its index */
static int add_extra(const bsp_miptex *mt, int skin, int shift)
{
	if (!skin)
		for (int i = 0; i < nextras; i++)
			if (!extras[i].skin && !strcmp(extras[i].mt.name, mt->name) && extras[i].mt.w == mt->w &&
			    extras[i].mt.h == mt->h && !memcmp(extras[i].mt.mip[0], mt->mip[0], (size_t) mt->w * mt->h))
				return i;
	extras = realloc(extras, (size_t) (nextras + 1) * sizeof *extras);
	if (!extras) die("out of memory");
	extras[nextras].mt    = *mt;
	extras[nextras].skin  = skin;
	extras[nextras].shift = shift;
	return nextras++;
}

void items_prepare(const bsp_t *b, const uint8_t *pak, long pak_len, const uint8_t *pak0,
                   long pak0_len, const uint8_t *palette, int squeeze, tex_extra **extra, int *nextra)
{
	memset(need, 0, sizeof need);
	nextras = 0;
	qc_set_palette(palette);
	ent_list l;
	ents_parse(b, &l);
	int worldtype = 0;
	for (int i = 0; i < l.num; i++)
		if (!strcmp(l.ents[i].classname, "worldspawn"))
			worldtype = (int) ent_float(&l, &l.ents[i], "worldtype", 0);
	for (int i = 0; i < l.num; i++)
		need_for(&l, &l.ents[i], worldtype);
	ents_free(&l);

	for (int q = 0; q < QIM_COUNT; q++) {
		if (!need[q])
			continue;
		uint32_t len;
		const uint8_t *d = pak_find(pak, pak_len, QIMS[q].file, &len);
		if (!d && pak0)
			d = pak_find(pak0, pak0_len, QIMS[q].file, &len);
		if (!d) {
			printf("qcook: %s missing: not cooked\n", QIMS[q].file);
			need[q] = 0;
			continue;
		}
		if (!is_bsp(q)) {
			int flame = q == QIM_FLAME || q == QIM_FLAME2;
			int div = QIMS[q].div << (squeeze >= (flame ? 3 : 2));
			mdls[q] = qc_mdl_load(QIMS[q].file, d, len, QIMS[q].skin, div, QIMS[q].flat);
			bsp_miptex mt;
			memset(&mt, 0, sizeof mt);
			snprintf(mt.name, sizeof mt.name, "~skin%d", q);
			const uint8_t *px;
			qc_mdl_skin(mdls[q], &mt.w, &mt.h, &px);
			for (int k = 0; k < 4; k++)
				mt.mip[k] = px;
			extra_of[q] = add_extra(&mt, 1, 0);
			continue;
		}
		/* a box: its model's textures */
		bsp_parse(&bsps[q], d, len);
		const bsp_t *ib = &bsps[q];
		const bsp_model *m0 = &ib->models[0];
		bsp_ntex[q] = 0;
		for (int f = m0->firstface; f < m0->firstface + m0->numfaces; f++) {
			int ti = ib->faces[f].texinfo, mt = ib->texinfo[ti].miptex, k;
			for (k = 0; k < bsp_ntex[q] && box_miptex[q][k] != mt; k++)
				;
			if (k == bsp_ntex[q]) {
				if (k >= QW_MODEL_TEX)
					die("%s: more than %d textures", QIMS[q].file, QW_MODEL_TEX);
				box_miptex[q][k] = mt;
				bsp_extra[q][k]  = add_extra(&ib->miptex[mt], 0, (squeeze >= 1) + (squeeze >= 4));
				bsp_ntex[q]++;
			}
		}
	}
	*extra  = extras;
	*nextra = nextras;
}

/* ------------------------------------------------------------ the boxes -- */

typedef struct { int16_t p[3]; } pvtx;
static pvtx *pool;
static int   npool, pool_cap;

static int pool_add(const int16_t p[3])
{
	for (int i = 0; i < npool; i++)
		if (!memcmp(pool[i].p, p, sizeof pool[i].p))
			return i;
	if (npool == pool_cap) {
		pool_cap = pool_cap ? 2 * pool_cap : 64;
		pool = realloc(pool, (size_t) pool_cap * sizeof *pool);
		if (!pool) die("out of memory");
	}
	memcpy(pool[npool].p, p, sizeof pool[npool].p);
	return npool++;
}

#define BOX_MAX_TRIS 48
typedef struct {
	int    nv, nt;
	int    pv[QW_MAX_VERTS];        /* slot -> pool vertex */
	qw_tri t[BOX_MAX_TRIS];
} brec;

static int brec_slot(brec *r, int pv)
{
	for (int i = 0; i < r->nv; i++)
		if (r->pv[i] == pv)
			return i;
	r->pv[r->nv] = pv;
	return r->nv++;
}

static void brec_flush(brec *r, int tex, int *nrec)
{
	if (!r->nt)
		return;
	qw_record h;
	memset(&h, 0, sizeof h);
	h.num_verts = (uint8_t) r->nv;
	h.num_tris  = (uint8_t) r->nt;
	h.tex       = (uint16_t) tex;
	int nidx    = (r->nv + 2) / 3 * 3;
	int ibytes  = (nidx * 2 + 3) & ~3;
	int cbytes  = (r->nv + 3) & ~3;
	h.polys     = (uint16_t) (sizeof h + ibytes + cbytes);
	h.size      = (uint16_t) (h.polys + r->nt * sizeof(qw_tri));
	long lo[3] = { 32767, 32767, 32767 }, hi[3] = { -32768, -32768, -32768 };
	for (int i = 0; i < r->nv; i++)
		for (int k = 0; k < 3; k++) {
			if (pool[r->pv[i]].p[k] < lo[k]) lo[k] = pool[r->pv[i]].p[k];
			if (pool[r->pv[i]].p[k] > hi[k]) hi[k] = pool[r->pv[i]].p[k];
		}
	for (int k = 0; k < 3; k++)
		h.sc[k] = (int16_t) ((lo[k] + hi[k]) / 2);
	double rad = 0;
	for (int i = 0; i < r->nv; i++) {
		double dx = pool[r->pv[i]].p[0] - h.sc[0], dy = pool[r->pv[i]].p[1] - h.sc[1],
		       dz = pool[r->pv[i]].p[2] - h.sc[2];
		double d = sqrt(dx * dx + dy * dy + dz * dz);
		if (d > rad) rad = d;
	}
	h.sr = (uint16_t) ceil(rad + 1);
	buf_put(&geo, &h, sizeof h);
	uint16_t idx[QW_MAX_VERTS + 3] = { 0 };
	for (int i = 0; i < r->nv; i++)
		idx[i] = (uint16_t) r->pv[i];
	for (int i = r->nv; i < nidx; i++)
		idx[i] = idx[0];
	buf_put(&geo, idx, (size_t) ibytes);
	uint8_t col[QW_MAX_VERTS + 4];
	memset(col, 128, sizeof col);
	buf_put(&geo, col, (size_t) cbytes);
	buf_put(&geo, r->t, (size_t) r->nt * sizeof(qw_tri));
	(*nrec)++;
	r->nv = r->nt = 0;
}

/* a box: per texture, its faces as triangle fans; the texture coordinates
   of a face shifted by whole repetitions so they start in the first (the
   texture window folds the rest), at the slot's scale */
static void cook_box(int q, const tex_set *ts, int first_extra, qw_model *out)
{
	const bsp_t *ib = &bsps[q];
	const bsp_model *m0 = &ib->models[0];
	npool = 0;
	size_t rec_start = geo.len;
	int nrec = 0;
	for (int k = 0; k < bsp_ntex[q]; k++) {
		const tex_slot *s = &ts->slots[first_extra + bsp_extra[q][k]];
		const bsp_miptex *mt = &ib->miptex[box_miptex[q][k]];
		if (!s->present)
			continue;
		brec r;
		memset(&r, 0, sizeof r);
		for (int f = m0->firstface; f < m0->firstface + m0->numfaces; f++) {
			const bsp_face *bf = &ib->faces[f];
			const bsp_texinfo *ti = &ib->texinfo[bf->texinfo];
			if (ti->miptex != box_miptex[q][k] || bf->numedges < 3 || bf->numedges > 16)
				continue;
			int    pv[16];
			double st[16][2], smin = 1e30, tmin = 1e30;
			for (int e = 0; e < bf->numedges; e++) {
				int se = ib->surfedges[bf->firstedge + e];
				int vi = se >= 0 ? ib->edges[se * 2] : ib->edges[-se * 2 + 1];
				const float *v = &ib->verts[vi * 3];
				int16_t p[3];
				for (int c = 0; c < 3; c++)
					p[c] = (int16_t) lround(v[c] * QW_VSCALE);
				pv[e] = pool_add(p);
				st[e][0] = v[0] * ti->vecs[0][0] + v[1] * ti->vecs[0][1] + v[2] * ti->vecs[0][2] + ti->vecs[0][3];
				st[e][1] = v[0] * ti->vecs[1][0] + v[1] * ti->vecs[1][1] + v[2] * ti->vecs[1][2] + ti->vecs[1][3];
				if (st[e][0] < smin) smin = st[e][0];
				if (st[e][1] < tmin) tmin = st[e][1];
			}
			double s0 = floor(smin / mt->w) * mt->w, t0 = floor(tmin / mt->h) * mt->h;
			uint16_t uv[16];
			for (int e = 0; e < bf->numedges; e++) {
				long u = lround((st[e][0] - s0) * s->rep_w / mt->w) + (s->win_x ? 0 : s->u0);
				long v = lround((st[e][1] - t0) * s->rep_h / mt->h) + (s->win_y ? 0 : s->v0);
				if (u < 0) u = 0;
				if (u > 255) u = 255;
				if (v < 0) v = 0;
				if (v > 255) v = 255;
				uv[e] = (uint16_t) (u | v << 8);
			}
			for (int e = 1; e + 1 < bf->numedges; e++) {   /* the fan */
				int c[3] = { 0, e, e + 1 };
				int newv = 0;
				for (int j = 0; j < 3; j++) {
					int seen = 0;
					for (int i = 0; i < r.nv && !seen; i++)
						seen = r.pv[i] == pv[c[j]];
					newv += !seen;
				}
				if (r.nv + newv > QW_MAX_VERTS || r.nt >= BOX_MAX_TRIS)
					brec_flush(&r, k, &nrec);
				qw_tri *t = &r.t[r.nt++];
				memset(t, 0, sizeof *t);
				for (int j = 0; j < 3; j++) {
					t->idx[j] = (uint8_t) (brec_slot(&r, pv[c[j]]) * 4);
					t->uv[j]  = uv[c[j]];
				}
			}
		}
		brec_flush(&r, k, &nrec);
		out->tex[k] = (uint16_t) (first_extra + bsp_extra[q][k]);
	}
	out->num_tex = (uint16_t) bsp_ntex[q];
	qg_model *m = &out->m;
	m->ofs_records = (uint32_t) rec_start;
	m->len_records = (uint32_t) (geo.len - rec_start);
	m->num_records = (uint16_t) nrec;
	buf_align4(&geo);
	m->ofs_frames = (uint32_t) geo.len;
	m->num_frames = 1;
	m->num_verts  = (uint16_t) npool;
	double rad = 0;
	for (int i = 0; i < npool; i++) {
		qw_vert v = { pool[i].p[0], pool[i].p[1], pool[i].p[2], 0 };
		buf_put(&geo, &v, sizeof v);
		double d = sqrt((double) v.x * v.x + (double) v.y * v.y + (double) v.z * v.z);
		if (d > rad) rad = d;
	}
	m->radius = (uint16_t) ceil(rad);
	m->flags  = QGF_MAP | QGF_FLAT;
	if (ts->num > 0 && out->num_tex) {
		const tex_slot *s = &ts->slots[out->tex[0]];
		m->tex.clut = s->clut; m->tex.tpage = s->tpage; m->tex.window = s->window;
	}
}

void items_cook(const tex_set *ts, int first_extra)
{
	geo.len = table.len = 0;
	nmodels = 0;
	for (int q = 0; q < QIM_COUNT; q++) {
		if (!need[q])
			continue;
		qw_model out;
		memset(&out, 0, sizeof out);
		out.qim  = (uint16_t) q;
		out.m.id = (uint16_t) q;
		if (is_bsp(q)) {
			cook_box(q, ts, first_extra, &out);
		} else {
			const tex_slot *s = &ts->slots[first_extra + extra_of[q]];
			if (!s->present)
				continue;
			buf_align4(&geo);
			qc_mdl_cook(mdls[q], s->u0, s->v0, &geo, &out.m);
			out.num_tex = 1;
			out.tex[0]  = (uint16_t) (first_extra + extra_of[q]);
			out.m.tex.clut   = s->clut;
			out.m.tex.tpage  = s->tpage;
			out.m.tex.window = 0xE2000000u;
			out.m.flags |= QGF_MAP;
			if (qc_mdl_flags(mdls[q]) & 8)            /* EF_ROTATE */
				out.m.flags |= QGF_ROTATE;
		}
		buf_put(&table, &out, sizeof out);
		nmodels++;
	}
	if (nmodels)
		printf("qcook:   %d own models (items, flames), %u B of geometry\n", nmodels, (unsigned) geo.len);
}

void items_write(buf_t *f, uint32_t *ofs_models, uint32_t *num_models, uint32_t *ofs_mgeo,
                 uint32_t *len_mgeo)
{
	buf_align4(f);
	*ofs_models = (uint32_t) f->len;
	*num_models = (uint32_t) nmodels;
	buf_put(f, table.p, table.len);
	buf_align4(f);
	*ofs_mgeo = (uint32_t) f->len;
	*len_mgeo = (uint32_t) geo.len;
	buf_put(f, geo.p, geo.len);
	buf_align4(f);
}
