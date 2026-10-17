#include <stdint.h>
#include <psxgpu.h>

#include "game.h"
#include "gdata.h"
#include "hud.h"
#include "render.h"
#include "vid.h"

const char *const hud_style_name[HUD_STYLES] = { "original", "q2 psx" };

static int style = HUD_ORIGINAL;
static int cur_tpage = -1;

/* The HUD's packets are built only when what they show changes (Quake's
   sb_updates): they live in the arena's HUD block (vid_hud_block), each
   buffer's block tagged with the state it shows. A frame whose state its
   block shows just links the block; one whose state the other buffer's
   block shows (the usual case after a change) copies that; otherwise the
   packets are built. The block: a header, then the packets as a chain. */
#define HUD_MAGIC 0x31445548u                   /* "HUD1" */
typedef struct {
	int32_t  style, dead, health, armor, weapon, ammo[AMMO_COUNT];
	uint32_t items;
	int32_t  pain, weapons_shown;
} hud_state_t;
typedef struct {
	uint32_t    magic;
	hud_state_t state;
	uint32_t   *head, *tail;                    /* the chain (head 0: nothing to draw) */
	uint32_t    bytes;                          /* of packets after the header */
	uint32_t    packets[];
} hud_block_t;
static uint8_t  *blk_at, *blk_end;              /* the block being built */
static uint32_t *blk_head, *blk_tail;

static void *hud_alloc(int bytes)
{
	uint32_t *p = (uint32_t *) blk_at;
	if (blk_at + bytes > blk_end)
		return 0;
	blk_at += bytes;
	setlen(p, (bytes - 4) / 4);
	termPrim(p);
	if (blk_tail)
		setaddr(blk_tail, p);
	else
		blk_head = p;
	blk_tail = p;
	return p;
}

void hud_set_style(int s)
{
	style = s;
	int rows = s == HUD_ORIGINAL ? 192 : 240;
	vid_set_viewport(rows);
	r_set_viewport(rows);
}

int hud_style(void)
{
	return style;
}

/* ------------------------------------------------------------- drawing -- */

/* a picture at (x, y); its page's DR_TPAGE when the page changes */
static void pic(int id, int x, int y)
{
	const qg_pic *p = &gdata.pics[id];
	if (p->tpage != cur_tpage) {
		DR_TPAGE *t = hud_alloc(sizeof *t);
		if (!t)
			return;
		setDrawTPage(t, 0, 0, p->tpage);
		cur_tpage = p->tpage;
	}
	SPRT *s = hud_alloc(sizeof *s);
	if (!s)
		return;
	setSprt(s);
	setXY0(s, x, y);
	setUV0(s, p->u, p->v);
	setWH(s, p->w, p->h);
	setRGB0(s, 128, 128, 128);
	s->clut = gdata.clut;
}

/* Sbar_DrawNum: `digits` big digits, right-aligned, red when `red` */
static void num(int x, int y, int value, int digits, int red)
{
	char str[12];
	int  n = 0, neg = value < 0;
	if (neg) value = -value;
	do {
		str[n++] = (char) ('0' + value % 10);
		value /= 10;
	} while (value && n < 10);
	if (neg) str[n++] = '-';
	if (n > digits) n = digits;
	x += (digits - n) * 24;
	int base = red ? QGP_ANUM_0 : QGP_NUM_0;
	for (int i = n - 1; i >= 0; i--) {
		pic(str[i] == '-' ? base + 10 : base + (str[i] - '0'), x, y);
		x += 24;
	}
}

/* the inventory bar's small yellow digits: 3 right-aligned, 8 px apart */
static void smallnum(int x, int y, int value)
{
	const qg_pic *p = &gdata.pics[QGP_SMALLNUM];
	if (value < 0) value = 0;
	if (value > 999) value = 999;
	int d[3] = { value / 100, value / 10 % 10, value % 10 }, lead = 1;
	for (int i = 0; i < 3; i++) {
		if (d[i] || !lead || i == 2) {
			lead = 0;
			if (p->tpage != cur_tpage) {
				DR_TPAGE *t = hud_alloc(sizeof *t);
				if (!t) return;
				setDrawTPage(t, 0, 0, p->tpage);
				cur_tpage = p->tpage;
			}
			SPRT_8 *s = hud_alloc(sizeof *s);
			if (!s) return;
			setSprt8(s);
			setXY0(s, x + i * 8, y);
			setUV0(s, p->u + d[i] * 8, p->v);
			setRGB0(s, 128, 128, 128);
			s->clut = gdata.clut;
		}
	}
}

static int face_pic(void)
{
	int f = pl.health >= 100 ? 4 : pl.health / 20;
	if (f < 0) f = 0;
	return (game_tick_count <= pl.pain_until ? QGP_FACE_P1 : QGP_FACE1) + f;
}

static int armor_pic(void)
{
	if (pl.items & IT_ARMOR3) return QGP_ARMOR3;
	if (pl.items & IT_ARMOR2) return QGP_ARMOR2;
	return QGP_ARMOR1;
}

static int ammo_pic(void)
{
	switch (game_ammo_type()) {
	case AMMO_SHELLS:  return QGP_AMMO_SHELLS;
	case AMMO_NAILS:   return QGP_AMMO_NAILS;
	case AMMO_ROCKETS: return QGP_AMMO_ROCKET;
	case AMMO_CELLS:   return QGP_AMMO_CELLS;
	default:           return -1;
	}
}

/* the 7 weapon icons: inv2 (selected) / inv, Quake's order */
static void weapon_row(int x, int y)
{
	for (int i = 0; i < 7; i++) {
		if (!(pl.items & (IT_SHOTGUN << i)))
			continue;
		int sel = pl.weapon == W_SHOTGUN + i;
		pic((sel ? QGP_INV2_SHOTGUN : QGP_INV_SHOTGUN) + i, x + i * 24, y);
	}
}

/* keys, powerups (sb_items), sigils, 16 px apart */
static int item_row(int x, int y)
{
	for (int i = 0; i < 6; i++)
		if (pl.items & (1u << (17 + i))) {
			pic(QGP_KEY1 + i, x, y);
			x += 16;
		}
	for (int i = 0; i < 4; i++)
		if (pl.items & (1u << (28 + i))) {
			pic(QGP_SIGIL1 + i, x, y);
			x += 8;
		}
	return x;
}

/* Sbar_Draw: the inventory bar at 192, the status bar at 216 */
static void draw_original(void)
{
	pic(QGP_IBAR_L, 0, 192);
	pic(QGP_IBAR_R, 160, 192);
	weapon_row(0, 200);
	for (int i = 0; i < AMMO_COUNT; i++)
		smallnum((6 * i + 1) * 8 - 2, 192, pl.ammo[i]);
	for (int i = 0; i < 6; i++)
		if (pl.items & (1u << (17 + i)))
			pic(QGP_KEY1 + i, 192 + i * 16, 200);
	for (int i = 0; i < 4; i++)
		if (pl.items & (1u << (28 + i)))
			pic(QGP_SIGIL1 + i, 288 + i * 8, 200);

	if (pl.dead) {
		pic(QGP_SCOREBAR_L, 0, 216);
		pic(QGP_SCOREBAR_R, 160, 216);
		return;
	}
	pic(QGP_SBAR_L, 0, 216);
	pic(QGP_SBAR_R, 160, 216);
	num(24, 216, pl.armor, 3, pl.armor <= 25);
	if (pl.armor > 0)
		pic(armor_pic(), 0, 216);
	pic(face_pic(), 112, 216);
	num(136, 216, pl.health, 3, pl.health <= 25);
	int ap = ammo_pic();
	if (ap >= 0) {
		pic(ap, 224, 216);
		num(248, 216, game_current_ammo(), 3, game_current_ammo() <= 10);
	}
}

/* the corners of a full-screen view */
static void draw_q2psx(void)
{
	if (pl.dead)
		return;
	pic(face_pic(), 8, 212);
	num(36, 212, pl.health, 3, pl.health <= 25);
	if (pl.armor > 0) {
		pic(armor_pic(), 124, 212);
		num(148, 212, pl.armor, 3, pl.armor <= 25);
	}
	int ap = ammo_pic();
	if (ap >= 0) {
		num(216, 212, game_current_ammo(), 3, game_current_ammo() <= 10);
		pic(ap, 292, 212);
	}
	/* the weapons for 2 s after a change, top right; keys and powerups top left */
	if (game_tick_count - pl.weapon_switch_tick < 2 * GAME_HZ)
		weapon_row(120, 8);
	item_row(8, 8);
}

static void hud_state(hud_state_t *s)
{
	s->style  = style;
	s->dead   = pl.dead;
	s->health = pl.health;
	s->armor  = pl.armor;
	s->weapon = pl.weapon;
	for (int i = 0; i < AMMO_COUNT; i++)
		s->ammo[i] = pl.ammo[i];
	s->items  = pl.items;
	s->pain   = game_tick_count <= pl.pain_until;
	s->weapons_shown = game_tick_count - pl.weapon_switch_tick < 2 * GAME_HZ;
}

static int hud_state_same(const hud_state_t *a, const hud_state_t *b)
{
	const uint32_t *x = (const uint32_t *) a, *y = (const uint32_t *) b;
	for (unsigned i = 0; i < sizeof *a / 4; i++)
		if (x[i] != y[i])
			return 0;
	return 1;
}

/* the other block's packets into this one, the chain relinked */
static void hud_copy(hud_block_t *b, const hud_block_t *o)
{
	const uint32_t *src = o->packets;
	uint32_t *dst = b->packets, *prev = 0;
	b->head = o->head ? dst : 0;
	for (uint32_t at = 0; at < o->bytes; ) {
		uint32_t n = (src[0] >> 24) + 1;        /* words, the tag included */
		for (uint32_t k = 1; k < n; k++)
			dst[k] = src[k];
		if (prev)
			setaddr(prev, dst);
		setlen(dst, n - 1);
		termPrim(dst);
		prev = dst;
		src += n;
		dst += n;
		at  += n * 4;
	}
	b->tail  = prev;
	b->bytes = o->bytes;
	b->state = o->state;
	b->magic = HUD_MAGIC;
}

void hud_draw(void)
{
	if (!gdata.ok)
		return;
	hud_state_t now;
	hud_state(&now);
	hud_block_t *b = vid_hud_block(0);
	if (b->magic != HUD_MAGIC || !hud_state_same(&b->state, &now)) {
		const hud_block_t *o = vid_hud_block(1);
		if (o->magic == HUD_MAGIC && hud_state_same(&o->state, &now)) {
			hud_copy(b, o);
		} else {
			blk_at   = (uint8_t *) b->packets;
			blk_end  = (uint8_t *) b + VID_HUD_BYTES;
			blk_head = blk_tail = 0;
			cur_tpage = -1;
			if (style == HUD_ORIGINAL)
				draw_original();
			else
				draw_q2psx();
			b->head  = blk_head;
			b->tail  = blk_tail;
			b->bytes = (uint32_t) (blk_at - (uint8_t *) b->packets);
			b->state = now;
			b->magic = HUD_MAGIC;
		}
	}
	if (b->head)
		vid_ovl_append(b->head, b->tail);
}
