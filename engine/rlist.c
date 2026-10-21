#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bmfrag.h"
#include "gdata.h"
#include "mdl.h"
#include "prof.h"
#include "render.h"
#include "rlist.h"
#include "stream.h"
#include "world.h"

r_item_t r_items[RL_MAX_ITEMS];
uint16_t r_lists[256];
static r_ent_t r_ents[RL_MAX_ENTS];
static int     n_items, n_ents, n_lists;
static qw_leaf *touched[RL_MAX_LISTS];
#define RL_SMALL __attribute__((optimize("Os")))   /* the rare ways: their room first */

/* a list's head when the brush models were in (rl_pieces_done), index + 1,
   | LP_CHAINED once its pieces are chained; [0] (no list) 0: r_lists' upper
   half (the walk's ids are 7 bits) */
#define rl_list_pieces (r_lists + 128)
#define LP_CHAINED 0x8000

#ifdef Q1PSX_BENCH
static uint32_t order_calls, order_cycles;
extern int ents_debug;
#endif

void rl_begin(void)
{
	n_items = n_ents = n_lists = 0;
	mdl_new_frame();
#ifdef Q1PSX_BENCH
	if (ents_debug)                             /* (the debug frame's places: rl_order_print) */
		order_calls = order_cycles = 0;
#endif
}

int rl_num_items(void)
{
	return n_items;
}

static int lists_used;      /* (the last frame's, for the bench) */

int rl_num_lists(void)
{
	return lists_used;
}

/* Quake's AngleVectors: forward, right, up of (pitch, yaw, roll), x4096 */
static void angle_vectors(int pitch, int yaw, int roll, int32_t f[3], int32_t r[3], int32_t u[3])
{
	int32_t sy = r_sin(yaw), cy = r_cos(yaw), sp = r_sin(pitch), cp = r_cos(pitch);
	int32_t sr = r_sin(roll), cr = r_cos(roll);
	f[0] = (cp * cy) >> 12;
	f[1] = (cp * sy) >> 12;
	f[2] = -sp;
	r[0] = ((-sr * sp >> 12) * cy >> 12) + (cr * sy >> 12);
	r[1] = ((-sr * sp >> 12) * sy >> 12) - (cr * cy >> 12);
	r[2] = -sr * cp >> 12;
	u[0] = ((cr * sp >> 12) * cy >> 12) + (sr * sy >> 12);
	u[1] = ((cr * sp >> 12) * sy >> 12) - (sr * cy >> 12);
	u[2] = cr * cp >> 12;
}

/* the entity's GTE rotation: view rotation x model-to-world, and its
   translation, by the GTE (engine/ent_mat.s). Quake's model axes are
   forward, -right, up (r_alias.c R_AliasSetUpTransform, with the pitch
   negated: entity pitches are backward): the model-to-world matrix's
   columns */
void rl_ent_matrix(r_ent_t *e, const int16_t cols[3][4], const int16_t o[4]);

static void ent_rotation(r_ent_t *e, int yaw, int pitch, int roll, const int16_t o[4])
{
	int32_t f[3], r[3], u[3];
	angle_vectors(-pitch, yaw, roll, f, r, u);
	int16_t cols[3][4];
	for (int i = 0; i < 3; i++) {
		cols[0][i] = (int16_t) f[i];
		cols[1][i] = (int16_t) -r[i];
		cols[2][i] = (int16_t) u[i];
	}
	cols[0][3] = cols[1][3] = cols[2][3] = 0;
	rl_ent_matrix(e, cols, o);
}

static void ent_model(r_ent_t *e, const qg_model *m, int frame, int light)
{
	if (frame < 0 || frame >= m->num_frames)
		frame = 0;
	const uint8_t *recs;
	if (m->flags & QGF_MAP) {               /* a map's own model: its block, its textures */
		e->pool = (uint32_t) ((const qw_vert *) (world.mgeo + m->ofs_frames) + (uint32_t) frame * m->num_verts);
		recs    = world.mgeo + m->ofs_records;
	} else {
		e->pool = (uint32_t) gd_frame(m, frame);
		recs    = gd_records(m);
	}
	if (m->flags & QGF_SPRITE) {            /* record k = frame k */
		const uint8_t *r = recs;
		for (int k = 0; k < frame; k++)
			r += ((const qw_record *) r)->size;
		e->recs     = (uint32_t) r;
		e->recs_end = (uint32_t) (r + ((const qw_record *) r)->size);
	} else {
		e->recs     = (uint32_t) recs;
		e->recs_end = (uint32_t) (recs + m->len_records);
	}
	e->tex = (m->flags & QGF_MAP) ? (uint32_t) world.model_tex[m - world.models] : (uint32_t) &m->tex;
	if (light > 170) light = 170;
	if (light < 0) light = 0;
	e->light = (uint32_t) light;
	e->dots  = 0;
	e->flags = 0;
}

r_ent_t *rl_model(const qg_model *m, int frame, const int32_t org[3], int yaw, int pitch,
                  int roll, int light)
{
	if (n_ents >= RL_MAX_ENTS)
		return 0;
	r_ent_t *e = &r_ents[n_ents++];
	ent_model(e, m, frame, light);
	/* translation: the view's, plus the origin through the view rotation */
	int16_t o[4];
	for (int k = 0; k < 3; k++)
		o[k] = (int16_t) (org[k] * QW_VSCALE / R_POS_SUB);
	o[3] = 0;
	if (m->flags & QGF_SPRITE) {            /* a billboard: its quad is in the view plane */
		memset(e->rot, 0, sizeof e->rot);
		e->rot[0] = e->rot[4] = e->rot[8] = R_MATRIX_ONE;
		rl_ent_matrix(e, 0, o);
	} else {
		ent_rotation(e, yaw, pitch, roll, o);
	}
	if (!(m->flags & QGF_FLAT))
		e->dots = (uint32_t) (gdata.dots + ((yaw * QG_LIGHT_YAWS / ANG_TURN) & (QG_LIGHT_YAWS - 1)) * QG_NORMALS);
	return e;
}

/* A streamed model (formats/qstream.h): its frame expanded (engine/mdl.c),
   its matrix the model-to-world columns scaled by its steps (Quake's
   R_AliasSetUpTransform folds the scale in the same way), the origin moved
   by its own origin turned with it. */
r_ent_t *rl_model_stream(int a, int frame, const int32_t org[3], int yaw, int pitch, int roll, int light)
{
	const qst_model *m = stream_model(a);
	if (!m || n_ents >= RL_MAX_ENTS)
		return 0;
	if (frame < 0 || frame >= m->num_frames)
		frame = 0;
	const qw_vert *pool = mdl_frame(a, m, frame);
	if (!pool)
		return 0;
	r_ent_t *e = &r_ents[n_ents++];
	e->pool     = (uint32_t) pool;
	e->recs     = (uint32_t) ((const uint8_t *) m + m->ofs_records);
	e->recs_end = e->recs + m->len_records;
	e->tex      = (uint32_t) stream_model_tex(a);
	if (light > 170) light = 170;
	if (light < 0) light = 0;
	e->light = (uint32_t) light;
	e->dots  = 0;
	e->flags = 0;
	int32_t f[3], r[3], u[3];
	angle_vectors(-pitch, yaw, roll, f, r, u);
	int16_t cols[3][4], o[4];
	for (int i = 0; i < 3; i++) {
		cols[0][i] = (int16_t) ((f[i] * m->scale[0] + 32768) >> 16);
		cols[1][i] = (int16_t) ((-r[i] * m->scale[1] + 32768) >> 16);
		cols[2][i] = (int16_t) ((u[i] * m->scale[2] + 32768) >> 16);
		int32_t mo = f[i] * m->origin[0] - r[i] * m->origin[1] + u[i] * m->origin[2];   /* x 4096 x 16 */
		o[i] = (int16_t) (org[i] * QW_VSCALE / R_POS_SUB + ((mo + 32768) >> 16));
	}
	cols[0][3] = cols[1][3] = cols[2][3] = 0;
	o[3] = 0;
	rl_ent_matrix(e, (const int16_t (*)[4]) cols, o);
	if (!(m->flags & QSTM_FLAT))
		e->dots = (uint32_t) (gdata.dots + ((yaw * QG_LIGHT_YAWS / ANG_TURN) & (QG_LIGHT_YAWS - 1)) * QG_NORMALS);
	return e;
}

r_ent_t *rl_bmodel(const qw_bmodel *bm, const int32_t org[3])
{
	if (n_ents >= RL_MAX_ENTS)
		return 0;
	r_ent_t *e = &r_ents[n_ents++];
	const uint32_t *vr = (const uint32_t *) r_state.rot;   /* unrotated: the view's rotation */
	uint32_t       *er = (uint32_t *) e->rot;
	er[0] = vr[0]; er[1] = vr[1]; er[2] = vr[2]; er[3] = vr[3]; er[4] = vr[4];
	int16_t o[4];
	for (int k = 0; k < 3; k++)
		o[k] = (int16_t) (org[k] * QW_VSCALE / R_POS_SUB);
	o[3] = 0;
	if (!(org[0] | org[1] | org[2])) {            /* in place: the view's translation */
		e->tr[0] = r_state.tr[0];
		e->tr[1] = r_state.tr[1];
		e->tr[2] = r_state.tr[2];
	} else {
		rl_ent_matrix(e, 0, o);                   /* (the translation only) */
	}
	e->pool     = (uint32_t) world.verts;
	e->recs     = bm->root;
	e->recs_end = 0;
	e->tex      = (uint32_t) world.textures;
	e->dots     = 0;
	e->light    = 0;
	e->flags    = RE_BMODEL;
	for (int k = 0; k < 3; k++)
		e->cam[k] = r_state.cam[k] - o[k];
	return e;
}

void rl_view_model(r_ent_t *e)
{
	if (!e)
		return;
	for (int i = 0; i < 6; i++)
		e->rot[i] = (int16_t) (e->rot[i] * 4);
	e->tr[0] *= 4;
	e->tr[1] *= 4;
	e->flags = RE_VIEW;
}

/* the leaf's list id in its vis word's top byte, with bit 31 set: the walk
   tests the word's sign for "anything to draw here" (r_world.s wl_ents) */
static int leaf_list(qw_leaf *leaf)
{
	int id = (int) (leaf->vis >> 24) & 0x7f;
	if (!id) {
		if (n_lists >= RL_MAX_LISTS)
			return 0;
		touched[n_lists++] = leaf;
		id = n_lists;
		leaf->vis |= (uint32_t) (id | 0x80) << 24;
		r_lists[id] = 0;
		rl_list_pieces[id] = 0;
	}
	return id;
}

/* item n into list id after item `after` (index + 1; 0: at the head, drawn
   last of the leaf) */
static inline void list_insert(int id, unsigned after, int n)
{
	r_item_t *it = &r_items[n];
	if (!after) {
		it->next = r_lists[id];
		r_lists[id] = (uint16_t) (n + 1);
	} else {
		it->next = r_items[after - 1].next;
		r_items[after - 1].next = (uint16_t) (n + 1);
	}
}

/* An alias model or a particle among its leaf's brush-model pieces (docs/
   porting-notes.md §50.1, §51): engine/rl_order.s, hand-written; the C
   below (ORDER_C=1 builds it alone, ORDER_C=2 checks the asm by it, call
   by call) is its specification. */
void rl_order(int id, int n, const int32_t org[3], const int32_t *box);

#if Q1PSX_ORDER_C
/* The last of list id's pieces items (in the list's order: their models'
   walks, back to front) to be drawn over point p (1/R_POS_SUB units):
   index + 1, 0 if none. A model's items over p come first (its walk's
   front): the first one under p ends that model's. At the first ask the
   leaf's pieces items are chained (z; x -1: their node not looked for),
   from the first of them: the list holds nothing else of this frame's yet
   but brush models drawn whole (one drawn after a model's pieces in its
   leaf heads the list). */
static RL_SMALL unsigned pieces_over(int id, const int32_t p[3])
{
	unsigned head = rl_list_pieces[id];
	if (!(head & LP_CHAINED)) {
		r_item_t *last = 0;
		unsigned first = 0;
		for (unsigned i = head; i; i = r_items[i - 1].next) {
			r_item_t *it = &r_items[i - 1];
			if (it->type != RI_PIECES)
				continue;
			it->x = -1;
			if (last)
				last->z = (int16_t) i;
			else
				first = i;
			last = it;
		}
		if (last)
			last->z = 0;
		head = first;                           /* (0: brush models drawn whole only) */
		rl_list_pieces[id] = (uint16_t) (head | LP_CHAINED);
	}
	unsigned over = 0;
	int model = -1, ask = 0;
	for (unsigned i = head & ~LP_CHAINED; i; i = (uint16_t) r_items[i - 1].z) {
		r_item_t *it = &r_items[i - 1];
		if (it->colour != model) {
			model = it->colour;
			ask = bmf_may_hide(model, p);
		}
		if (!ask)
			continue;
		if (it->x < 0)
			it->x = (int16_t) bmf_pieces_node(model, (const uint8_t *) it->ent);
		if (bmf_node_over(model, it->x, p))
			over = i;
		else
			ask = 0;                                /* (the model's next ones: under p too) */
	}
	return over;
}

/* item n (a model at depth x, or a particle) into list id after the pieces
   in front of its point, then after the models nearer than it (a particle:
   none). The point: org, or with `box` (an edict's mins then maxs) org
   plus the box's centre. */
RL_SMALL void rl_order_c(int id, int n, const int32_t org[3], const int32_t *box)
{
	int32_t c[3];
	if (box) {
		for (int k = 0; k < 3; k++)
			c[k] = org[k] + (box[k] + box[k + 3]) / 2;
		org = c;
	}
	unsigned after = pieces_over(id, org);
	if (r_items[n].type == 1) {
		int16_t z = r_items[n].x;
		for (unsigned nx = after ? r_items[after - 1].next : r_lists[id];
		     nx && r_items[nx - 1].type == 1 && r_items[nx - 1].x < z; nx = r_items[nx - 1].next)
			after = nx;
	}
	list_insert(id, after, n);
}
#endif

#if Q1PSX_ORDER_C == 2
/* The asm checked by the C, ask by ask: both from the same state, the C's
   first; the state an ask can touch is the list's head, its pieces' head,
   and the first three words of its items and of item n (next; x; z). */
#define ORD_KEEP 160             /* a list's items kept (a longer one goes unchecked) */
typedef struct {
	uint16_t head, pieces;
	int      count;
	uint16_t idx[ORD_KEEP];
	uint32_t w[ORD_KEEP][3];
} ord_state;
static ord_state ord_keep, ord_c, ord_a;
static uint32_t  ord_checks, ord_bad, ord_unchecked, ord_probes;

static int ord_take(ord_state *s, int id, int n)
{
	const uint32_t *w;
	s->head   = r_lists[id];
	s->pieces = rl_list_pieces[id];
	s->count  = 0;
	for (unsigned i = s->head;; i = r_items[i - 1].next) {
		unsigned at = i ? i : (unsigned) n + 1;   /* (the list's items, then n) */
		if (s->count >= ORD_KEEP)
			return 0;
		s->idx[s->count] = (uint16_t) at;
		w = (const uint32_t *) &r_items[at - 1];
		s->w[s->count][0] = w[0];
		s->w[s->count][1] = w[1];
		s->w[s->count][2] = w[2];
		s->count++;
		if (!i)
			return 1;
	}
}

static void ord_put(const ord_state *s, int id)
{
	r_lists[id] = s->head;
	rl_list_pieces[id] = s->pieces;
	for (int k = 0; k < s->count; k++) {
		uint32_t *w = (uint32_t *) &r_items[s->idx[k] - 1];
		w[0] = s->w[k][0];
		w[1] = s->w[k][1];
		w[2] = s->w[k][2];
	}
}

static int ord_same(const ord_state *a, const ord_state *b)
{
	if (a->head != b->head || a->pieces != b->pieces || a->count != b->count)
		return 0;
	for (int k = 0; k < a->count; k++)
		if (a->idx[k] != b->idx[k] || a->w[k][0] != b->w[k][0] || a->w[k][1] != b->w[k][1] ||
		    a->w[k][2] != b->w[k][2])
			return 0;
	return 1;
}

static void ord_print(const char *who, const ord_state *s)
{
	printf("ordasm %s head %u pieces %04x:", who, s->head, s->pieces);
	for (int k = 0; k < s->count && k < 20; k++)
		printf(" %u:%08x/%08x/%08x", s->idx[k], (unsigned) s->w[k][0], (unsigned) s->w[k][1],
		       (unsigned) s->w[k][2]);
	printf("\n");
}

/* `keep`: the state put back after (a probe's ask) */
static void order_check(int id, int n, const int32_t org[3], const int32_t *box, int keep)
{
	if (!ord_take(&ord_keep, id, n)) {
		ord_unchecked++;
		if (!keep)
			rl_order(id, n, org, box);
		return;
	}
	rl_order_c(id, n, org, box);
	ord_take(&ord_c, id, n);
	ord_put(&ord_keep, id);
	rl_order(id, n, org, box);
	ord_take(&ord_a, id, n);
	ord_checks++;
	if (!ord_same(&ord_c, &ord_a) && ord_bad++ < 8) {
		printf("ordasm MISMATCH list %d item %d type %d x %d at %d %d %d%s\n", id, n, r_items[n].type,
		       r_items[n].x, (int) org[0], (int) org[1], (int) org[2], box ? " + box" : "");
		ord_print("before", &ord_keep);
		ord_print("C     ", &ord_c);
		ord_print("asm   ", &ord_a);
	}
	if (keep)
		ord_put(&ord_keep, id);
}

void rl_order_probe(void)
{
	static const int32_t box[6] = { -16 * 8 - 1, -16 * 8, -24 * 8 + 3, 16 * 8, 16 * 8 + 1, 32 * 8 };
	if (n_items >= RL_MAX_ITEMS)
		return;
	int n = n_items;                            /* (a free item: never in a list once put back) */
	int32_t eye[3];
	for (int k = 0; k < 3; k++)
		eye[k] = r_state.cam[k] * (R_POS_SUB / QW_VSCALE);
	for (int id = 1; id <= n_lists; id++) {
		if (!rl_list_pieces[id])
			continue;
		int model = -1;
		for (unsigned i = r_lists[id]; i; i = r_items[i - 1].next) {
			const r_item_t *pc = &r_items[i - 1];
			if (pc->type != RI_PIECES || pc->colour == model)
				continue;
			model = pc->colour;
			int32_t lo[3], hi[3], c[3], p[3];
			bmf_model_box(model, lo, hi);
			for (int k = 0; k < 3; k++)
				c[k] = lo[k] + (hi[k] - lo[k]) / 2;
			for (int a = 0; a < 27 + 6 + 12; a++) {
				for (int k = 0; k < 3; k++) {
					if (a < 27) {                   /* its corners, middles, centre, 8 units out */
						int s = a / (k == 0 ? 1 : k == 1 ? 3 : 9) % 3;
						p[k] = s == 0 ? lo[k] - 64 : s == 1 ? c[k] : hi[k] + 64;
					} else if (a < 33) {             /* far beyond it from the eye, or behind */
						static const int32_t far[6] = { 16, 64, 1024, -16, -64, -1024 };
						p[k] = c[k] + (c[k] - eye[k]) * far[a - 27];
					} else {                         /* far along an axis: the model's space past
						                                16 bits, and past the axial planes'
						                                x 4096 */
						int b = a - 33;
						p[k] = c[k] + (b / 2 % 3 != k ? 0 : (b & 1 ? -1 : 1) * (b < 6 ? 70000 : 1 << 21));
					}
				}
				for (int t = 0; t < 3; t++) {        /* a particle, a model with its box, without */
					r_item_t *it = &r_items[n];
					memset(it, 0, sizeof *it);
					it->type = (uint8_t) (t != 0);
					it->x = (int16_t) (t ? (a * 37 + id * 11) % 300 - 60 : p[0] / 2);
					order_check(id, n, p, t == 1 ? box : 0, 1);
					ord_probes++;
				}
			}
		}
	}
}
#endif

#ifdef Q1PSX_BENCH
void rl_order_print(const char *name)
{
	if (order_calls)
		printf("order %s: %u places among pieces, %u cycles\n", name, (unsigned) order_calls,
		       (unsigned) order_cycles);
#if Q1PSX_ORDER_C == 2
	extern uint32_t bmf_order_stats[2];
	printf("ordasm %s: %u checked (%u made up), %u mismatches, %u unchecked; the C's general planes %u (%u "
	       "wide)\n", name, (unsigned) ord_checks, (unsigned) ord_probes, (unsigned) ord_bad,
	       (unsigned) ord_unchecked, (unsigned) bmf_order_stats[0], (unsigned) bmf_order_stats[1]);
	ord_checks = ord_probes = ord_bad = ord_unchecked = 0;
	bmf_order_stats[0] = bmf_order_stats[1] = 0;
#endif
}
#endif

/* item n's place among the pieces of list id (the rare way: a list with
   brush models' pieces): the asm's (ORDER_C=1 the C's, 2 both); the
   bench's debug frame times them */
#if defined(Q1PSX_BENCH) || Q1PSX_ORDER_C
static __attribute__((noinline)) void place_by_pieces(int id, int n, const int32_t org[3], const int32_t *box)
{
#ifdef Q1PSX_BENCH
	uint32_t t0 = ents_debug ? prof_now() : 0;
#endif
#if Q1PSX_ORDER_C == 1
	rl_order_c(id, n, org, box);
#elif Q1PSX_ORDER_C == 2
	order_check(id, n, org, box, 0);
#else
	rl_order(id, n, org, box);
#endif
#ifdef Q1PSX_BENCH
	if (ents_debug) {
		order_calls++;
		order_cycles += prof_cycles(t0, prof_now());
	}
#endif
}
#else
#define place_by_pieces rl_order
#endif

void rl_add_pieces(const qw_leaf *leaf, const uint8_t *recs, uint32_t len, uint32_t tag)
{
	if (!leaf || !len || n_items >= RL_MAX_ITEMS)
		return;
	int id = leaf_list((qw_leaf *) leaf);
	if (!id)
		return;
	r_item_t *it = &r_items[n_items];
	typedef uint16_t __attribute__((may_alias)) half_t;
	it->next = r_lists[id];                     /* (the frame's first: pieces only so far) */
	*(half_t *) &it->type = (uint16_t) tag;     /* (RI_PIECES, its model in colour: one store) */
	it->pad  = (uint16_t) len;
	it->ent  = (uint32_t) recs;
	r_lists[id] = (uint16_t) (n_items + 1);
	n_items++;
}

void rl_pieces_done(void)
{
	for (int id = 1; id <= n_lists; id++)
		rl_list_pieces[id] = r_lists[id];
}

void rl_add_model(const qw_leaf *leaf, r_ent_t *e, const int32_t org[3], const int32_t *box)
{
	if (!leaf || !e || n_items >= RL_MAX_ITEMS)
		return;
	int id = leaf_list((qw_leaf *) leaf);
	if (!id)
		return;
	int n = n_items++;
	r_item_t *it = &r_items[n];
	it->type   = 1;
	it->colour = 0;
	it->ent    = (uint32_t) e;
	if (!org) {                                 /* a brush model drawn whole */
		it->x = INT16_MAX;
		list_insert(id, 0, n);
		return;
	}
	int32_t z = e->tr[2] >> 3;                  /* its depth: its matrix's, 2 world units
	                                               (any map's in an int16) */
	it->x = (int16_t) z;
	if (rl_list_pieces[id]) {
		place_by_pieces(id, n, org, box);       /* (its place among them: from its box's centre) */
		return;
	}
	/* after the nearer models (the list's head: the nearest, drawn last) */
	unsigned after = 0, nx = r_lists[id];
	for (; nx && r_items[nx - 1].type == 1 && r_items[nx - 1].x < z; nx = r_items[nx - 1].next)
		after = nx;
	it->next = (uint16_t) nx;
	if (after)
		r_items[after - 1].next = (uint16_t) (n + 1);
	else
		r_lists[id] = (uint16_t) (n + 1);
}

void rl_add_particle(const qw_leaf *leaf, const int32_t pos[3], int colour)
{
	if (!leaf || n_items >= RL_MAX_ITEMS)
		return;
	int id = leaf_list((qw_leaf *) leaf);
	if (!id)
		return;
	r_item_t *it = &r_items[n_items];
	it->type   = 0;
	it->colour = (uint8_t) colour;
	it->x = (int16_t) (pos[0] * QW_VSCALE / R_POS_SUB);
	it->y = (int16_t) (pos[1] * QW_VSCALE / R_POS_SUB);
	it->z = (int16_t) (pos[2] * QW_VSCALE / R_POS_SUB);
	it->ent = 0;
	int n = n_items++;
	if (rl_list_pieces[id])
		place_by_pieces(id, n, pos, 0);
	else
		list_insert(id, 0, n);                  /* (over the models: the frame adds the
		                                           particles last) */
}

void rl_end(void)
{
	for (int i = 0; i < n_lists; i++)
		touched[i]->vis &= 0x00ffffffu;
#ifdef Q1PSX_BENCH
	lists_used = n_lists;
#endif
	n_lists = 0;
}
