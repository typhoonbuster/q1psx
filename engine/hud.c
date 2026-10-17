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
		DR_TPAGE *t = vid_ovl_alloc(sizeof *t);
		if (!t)
			return;
		setDrawTPage(t, 0, 0, p->tpage);
		cur_tpage = p->tpage;
	}
	SPRT *s = vid_ovl_alloc(sizeof *s);
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
				DR_TPAGE *t = vid_ovl_alloc(sizeof *t);
				if (!t) return;
				setDrawTPage(t, 0, 0, p->tpage);
				cur_tpage = p->tpage;
			}
			SPRT_8 *s = vid_ovl_alloc(sizeof *s);
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

void hud_draw(void)
{
	if (!gdata.ok)
		return;
	cur_tpage = -1;
	if (style == HUD_ORIGINAL)
		draw_original();
	else
		draw_q2psx();
}
