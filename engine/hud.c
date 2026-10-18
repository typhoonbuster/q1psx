#include <stdint.h>
#include <psxgpu.h>

#include <stdio.h>
#include <string.h>

#include "game.h"
#include "gdata.h"
#include "hud.h"
#include "level.h"
#include "render.h"
#include "ui.h"
#include "vid.h"
#include "world.h"

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
	int32_t  face, weapons_shown;
	uint32_t wflash;             /* the 7 weapon icons' frames, 3 bits each */
} hud_state_t;

/* cl.item_gettime: when each item bit came (CL_ParseClientdata) */
static uint32_t seen_items;
static int      item_gettime[32];
typedef struct {
	uint32_t    magic;
	hud_state_t state;
	uint32_t   *head, *tail;                    /* the chain (head 0: nothing to draw) */
	uint32_t    bytes;                          /* of packets after the header */
	uint32_t    packets[];
} hud_block_t;
static uint8_t  *blk_at, *blk_end;              /* the block being built */
static uint32_t *blk_head, *blk_tail;

/* where the drawing goes: the cached block (the Q2 HUD), or straight to
   the frame's overlay chain (the original HUD's parts) */
static void *(*emit)(int bytes);

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
	if (s != style)
		hud_invalidate();
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

/* a part of a picture, its texels (sx, sy) .. + (w, h), at (x, y); its
   page's DR_TPAGE when the page changes */
static void pic_sub(int id, int x, int y, int sx, int sy, int w, int h)
{
	const qg_pic *p = &gdata.pics[id];
	if (p->tpage != cur_tpage) {
		DR_TPAGE *t = emit(sizeof *t);
		if (!t)
			return;
		setDrawTPage(t, 0, 0, p->tpage);
		cur_tpage = p->tpage;
	}
	SPRT *s = emit(sizeof *s);
	if (!s)
		return;
	setSprt(s);
	setXY0(s, x, y);
	setUV0(s, p->u + sx, p->v + sy);
	setWH(s, w, h);
	setRGB0(s, 128, 128, 128);
	s->clut = gdata.clut;
}

/* a picture at (x, y) */
static void pic(int id, int x, int y)
{
	const qg_pic *p = &gdata.pics[id];
	pic_sub(id, x, y, 0, 0, p->w, p->h);
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
				DR_TPAGE *t = emit(sizeof *t);
				if (!t) return;
				setDrawTPage(t, 0, 0, p->tpage);
				cur_tpage = p->tpage;
			}
			SPRT_8 *s = emit(sizeof *s);
			if (!s) return;
			setSprt8(s);
			setXY0(s, x + i * 8, y);
			setUV0(s, p->u + d[i] * 8, p->v);
			setRGB0(s, 128, 128, 128);
			s->clut = gdata.clut;
		}
	}
}

/* Sbar_DrawFace: the powerups' faces first */
static int face_pic(void)
{
	if ((pl.items & (IT_INVISIBILITY | IT_INVULNERABILITY)) == (IT_INVISIBILITY | IT_INVULNERABILITY))
		return QGP_FACE_INV2;
	if (pl.items & IT_QUAD)
		return QGP_FACE_QUAD;
	if (pl.items & IT_INVISIBILITY)
		return QGP_FACE_INVIS;
	if (pl.items & IT_INVULNERABILITY)
		return QGP_FACE_INVUL2;
	int f = pl.health >= 100 ? 4 : pl.health / 20;
	if (f < 0) f = 0;
	/* sb_faces[f]: [4] is "face1" (healthy) .. [0] "face5" (near death) */
	return (game_tick_count <= pl.pain_until ? QGP_FACE_P1 : QGP_FACE1) + 4 - f;
}

/* the armour's picture by the item (-1: none) */
static int armor_pic(void)
{
	if (pl.items & IT_ARMOR3) return QGP_ARMOR3;
	if (pl.items & IT_ARMOR2) return QGP_ARMOR2;
	if (pl.items & IT_ARMOR1) return QGP_ARMOR1;
	return -1;
}

/* Sbar_DrawInventory's flashon: a weapon just picked up cycles the 5 flash
   frames at 10 a second for 1 s; then 1 the active one, 0 the others */
static int weapon_frame(int i)
{
	int flashon = (game_tick_count - item_gettime[i]) * 10 / GAME_HZ;
	if (flashon >= 10)
		return pl.weapon == W_SHOTGUN + i ? 1 : 0;
	return flashon % 5 + 2;
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

/* the 7 weapon icons: inv, inv2 (selected), inva1..5 (just picked up),
   Quake's order */
static void weapon_row(int x, int y)
{
	for (int i = 0; i < 7; i++) {
		if (!(pl.items & (IT_SHOTGUN << i)))
			continue;
		int f = weapon_frame(i);
		pic(f == 0 ? QGP_INV_SHOTGUN + i : f == 1 ? QGP_INV2_SHOTGUN + i : QGP_INVA_SHOTGUN + (f - 2) * 7 + i,
		    x + i * 24, y);
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

/* ---- the original HUD, part by part --------------------------------------

   Its rows (192 .. 239) are below the 3D view: no frame clears them, each
   framebuffer keeps the HUD last drawn into it. So a part (the inventory
   bar's ammo counts, its weapons, its items; the status bar's armour, face,
   health and ammo, or the dead player's scoreboard) is drawn into a buffer
   only when what it shows differs from what that buffer shows: its
   background (the bar's columns behind it) and its pictures. Quake redrew
   the whole bar for every page after any change (sb_updates); a part is
   the unit here. Whatever else draws over those rows (the menu's fade, the
   intermission, a load, the other HUD style) invalidates both buffers. */
enum { HP_AMMOS, HP_WEAPONS, HP_ITEMS, HP_ARMOR, HP_FACE, HP_HEALTH, HP_AMMO, HP_SCORE, HP_COUNT };
static uint32_t part_shown[2][HP_COUNT][2];    /* per buffer: what each part shows */
static uint8_t  part_valid[2][HP_COUNT];

void hud_invalidate(void)
{
	memset(part_valid, 0, sizeof part_valid);
}

/* the bar's columns x0 .. x0 + w, its rows sy .. sy + h, from its two
   160-wide halves, at the bar's place (y: its top row) */
static void bar_bg(int left, int right, int y, int x0, int sy, int w, int h)
{
	if (x0 < 160) {
		int ww = x0 + w > 160 ? 160 - x0 : w;
		pic_sub(left, x0, y + sy, x0, sy, ww, h);
	}
	if (x0 + w > 160) {
		int s = x0 > 160 ? x0 : 160;
		pic_sub(right, s, y + sy, s - 160, sy, x0 + w - s, h);
	}
}

/* Sbar_SoloScoreboard: the dead player's bar, over the scorebar */
static void solo_scoreboard(void)
{
	char str[48];
	int secs = (game_tick_count - level_start_tick) / GAME_HZ;
	ui_begin();
	snprintf(str, sizeof str, "Monsters:%3d /%3d", level_killed_monsters(), level_total_monsters());
	ui_text(8, 216 + 4, str);
	snprintf(str, sizeof str, "Secrets :%3d /%3d", level_found_secrets(), level_total_secrets());
	ui_text(8, 216 + 12, str);
	snprintf(str, sizeof str, "Time :%3d:%d%d", secs / 60, secs % 60 / 10, secs % 10);
	ui_text(184, 216 + 4, str);
	const char *name = world.title ? world.title : "";
	ui_text(232 - (int) strlen(name) * 4, 216 + 12, name);
	cur_tpage = -1;                                /* (ui set its own pages) */
}

/* what a part shows (two words), and drawing it */
static void part_state(int p, uint32_t s[2])
{
	s[1] = 0;
	switch (p) {
	case HP_AMMOS:
		s[0] = (uint32_t) pl.ammo[0] | (uint32_t) pl.ammo[1] << 10 | (uint32_t) pl.ammo[2] << 20;
		s[1] = (uint32_t) pl.ammo[3];
		break;
	case HP_WEAPONS: {
		uint32_t f = 0;
		for (int i = 0; i < 7; i++)
			if (pl.items & (IT_SHOTGUN << i))
				f |= (uint32_t) (weapon_frame(i) + 1) << (3 * i);
		s[0] = f;
		break;
	}
	case HP_ITEMS:  s[0] = pl.items >> 17; break;
	case HP_ARMOR:  s[0] = (pl.items & IT_INVULNERABILITY) ? 1 : 2 + (uint32_t) armor_pic() + 64 * (uint32_t) pl.armor; break;
	case HP_FACE:   s[0] = (uint32_t) face_pic(); break;
	case HP_HEALTH: s[0] = (uint32_t) pl.health; break;
	case HP_AMMO:   s[0] = (uint32_t) (ammo_pic() + 1) | (uint32_t) game_current_ammo() << 8; break;
	default: {                                      /* the scoreboard: its numbers, the second */
		s[0] = (uint32_t) level_killed_monsters() | (uint32_t) level_found_secrets() << 16;
		s[1] = (uint32_t) ((game_tick_count - level_start_tick) / GAME_HZ);
		break;
	}
	}
}

static void part_draw(int p)
{
	switch (p) {
	case HP_AMMOS:
		bar_bg(QGP_IBAR_L, QGP_IBAR_R, 192, 0, 0, 192, 8);
		for (int i = 0; i < AMMO_COUNT; i++)
			smallnum((6 * i + 1) * 8 - 2, 192, pl.ammo[i]);
		break;
	case HP_WEAPONS:
		bar_bg(QGP_IBAR_L, QGP_IBAR_R, 192, 0, 8, 192, 16);
		weapon_row(0, 200);
		break;
	case HP_ITEMS:
		bar_bg(QGP_IBAR_L, QGP_IBAR_R, 192, 192, 0, 128, 24);
		for (int i = 0; i < 6; i++)
			if (pl.items & (1u << (17 + i)))
				pic(QGP_KEY1 + i, 192 + i * 16, 200);
		for (int i = 0; i < 4; i++)
			if (pl.items & (1u << (28 + i)))
				pic(QGP_SIGIL1 + i, 288 + i * 8, 200);
		break;
	case HP_ARMOR:
		bar_bg(QGP_SBAR_L, QGP_SBAR_R, 216, 0, 0, 112, 24);
		if (pl.items & IT_INVULNERABILITY) {      /* 666 and the disc */
			num(24, 216, 666, 3, 1);
			ui_begin();
			ui_aux(QGA_DISC, 0, 216);
			cur_tpage = -1;
		} else {
			num(24, 216, pl.armor, 3, pl.armor <= 25);
			if (armor_pic() >= 0)
				pic(armor_pic(), 0, 216);
		}
		break;
	case HP_FACE:
		bar_bg(QGP_SBAR_L, QGP_SBAR_R, 216, 112, 0, 24, 24);
		pic(face_pic(), 112, 216);
		break;
	case HP_HEALTH:
		bar_bg(QGP_SBAR_L, QGP_SBAR_R, 216, 136, 0, 88, 24);
		num(136, 216, pl.health, 3, pl.health <= 25);
		break;
	case HP_AMMO: {
		bar_bg(QGP_SBAR_L, QGP_SBAR_R, 216, 224, 0, 96, 24);
		int ap = ammo_pic();
		if (ap >= 0) {
			pic(ap, 224, 216);
			num(248, 216, game_current_ammo(), 3, game_current_ammo() <= 10);
		}
		break;
	}
	default:                                       /* Sbar_Draw, dead: the scorebar */
		bar_bg(QGP_SCOREBAR_L, QGP_SCOREBAR_R, 216, 0, 0, 320, 24);
		solo_scoreboard();
		break;
	}
}

/* Sbar_Draw: the inventory bar at 192, the status bar at 216 (or the
   scoreboard when dead), each part drawn into this buffer if it changed */
static void draw_original_parts(void)
{
	int buf = vid_back();
	emit = vid_ovl_alloc;
	cur_tpage = -1;
	for (int p = 0; p < HP_COUNT; p++) {
		int sbar = p >= HP_ARMOR && p <= HP_AMMO;
		if ((sbar && pl.dead) || (p == HP_SCORE && !pl.dead)) {
			part_valid[buf][p] = 0;                /* (not on the bar now) */
			continue;
		}
		uint32_t s[2];
		part_state(p, s);
		if (part_valid[buf][p] && part_shown[buf][p][0] == s[0] && part_shown[buf][p][1] == s[1])
			continue;
		part_draw(p);
		part_shown[buf][p][0] = s[0];
		part_shown[buf][p][1] = s[1];
		part_valid[buf][p]    = 1;
	}
}

/* the corners of a full-screen view */
static void draw_q2psx(void)
{
	if (pl.dead)
		return;
	pic(face_pic(), 8, 212);
	num(36, 212, pl.health, 3, pl.health <= 25);
	if (pl.items & IT_INVULNERABILITY) {          /* 666 and the disc (hud_draw) */
		num(148, 212, 666, 3, 1);
	} else if (pl.armor > 0) {
		if (armor_pic() >= 0)
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
	s->face   = face_pic();
	s->wflash = 0;
	for (int i = 0; i < 7; i++)
		if (pl.items & (IT_SHOTGUN << i))
			s->wflash |= (uint32_t) weapon_frame(i) << (3 * i);
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

/* ------------------------------------------------------- centerprint -- */

static char center_text[256];
static int  center_until = -1;        /* game tick */

void hud_centerprint(const char *s)
{
#ifdef Q1PSX_BENCH
	printf("centerprint: %s\n", s);
#endif
	size_t n = strlen(s);
	if (n >= sizeof center_text)
		n = sizeof center_text - 1;
	memcpy(center_text, s, n);
	center_text[n] = 0;
	center_until = game_tick_count + 2 * GAME_HZ;   /* scr_centertime 2 */
}

void hud_centerprint_clear(void)
{
	center_until = -1;
}

/* ------------------------------------------------------------- notify -- */

#define NOTIFY_LINES 4                /* NUM_CON_TIMES */
#define NOTIFY_WIDTH 38               /* con_linewidth: 320 / 8 - 2 */
static char notify_text[NOTIFY_LINES][NOTIFY_WIDTH + 1];
static int  notify_tick[NOTIFY_LINES];
static int  notify_count;             /* lines printed (con_current + 1) */

static void notify_line(const char *s, int n)
{
	char *t = notify_text[notify_count % NOTIFY_LINES];
	memcpy(t, s, (size_t) n);
	t[n] = 0;
	notify_tick[notify_count % NOTIFY_LINES] = game_tick_count;
	notify_count++;
}

/* Con_Print: lines at '\n', a word that does not fit to the next line */
void hud_notify(const char *s)
{
#ifdef Q1PSX_BENCH
	printf("notify: %s\n", s);
#endif
	while (*s) {
		int n = 0, cut = -1;
		while (s[n] && s[n] != '\n' && n < NOTIFY_WIDTH) {
			if (s[n] == ' ')
				cut = n;
			n++;
		}
		if (s[n] && s[n] != '\n' && cut > 0)  /* wrap before the word */
			n = cut;
		notify_line(s, n);
		s += n;
		while (*s == ' ' || *s == '\n')
			s++;
	}
}

void hud_level_start(void)
{
	seen_items = 0;
	notify_count = 0;
	for (int i = 0; i < NOTIFY_LINES; i++)
		notify_tick[i] = -1000000;
}

/* Con_DrawNotify: the lines younger than con_notifytime (3 s), from the top */
static void draw_notify(void)
{
	int y = 0, first = notify_count - NOTIFY_LINES, began = 0;
	for (int i = first < 0 ? 0 : first; i < notify_count; i++) {
		int k = i % NOTIFY_LINES;
		if (game_tick_count - notify_tick[k] > 3 * GAME_HZ)
			continue;
		if (!began) {
			ui_begin();
			began = 1;
		}
		ui_text(8, y, notify_text[k]);
		y += 8;
	}
}

/* SCR_DrawCenterString: each line centred, from 35 % of the screen down
   (from 48 when there are more than 4 lines); during the finale the text
   comes out 8 characters a second (`reveal` of them, -1 all) */
static void draw_center_string(const char *text, int reveal)
{
	int lines = 1;
	for (const char *p = text; *p; p++)
		lines += *p == '\n';
	int y = lines <= 4 ? VID_H * 35 / 100 : 48;
	const char *p = text;
	for (;;) {
		int l = 0;
		while (p[l] && p[l] != '\n' && l < 40)
			l++;
		int x = (VID_W - l * 8) / 2;
		if (reveal >= 0 && reveal < l) {          /* the finale's typing: its last line */
			ui_chars(x, y, p, reveal, 0, 128);
			return;
		}
		ui_chars(x, y, p, l, 0, 128);
		if (reveal >= 0)
			reveal -= l;
		y += 8;
		p += l;
		if (*p == '\n')
			p++;
		if (!*p)
			break;
	}
}

/* a big-digit picture into the overlay chain (the HUD block is cached) */
static int ovl_tpage = -1;
static void ovl_pic(int id, int x, int y)
{
	const qg_pic *p = &gdata.pics[id];
	if (p->tpage != ovl_tpage) {
		DR_TPAGE *t = vid_ovl_alloc(sizeof *t);
		if (!t)
			return;
		setDrawTPage(t, 0, 0, p->tpage);
		ovl_tpage = p->tpage;
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

/* Sbar_IntermissionNumber: right-aligned big digits */
static void inter_number(int x, int y, int num, int digits)
{
	char str[12];
	int l = 0, neg = num < 0;
	unsigned u = (unsigned) (neg ? -num : num);
	do { str[l++] = (char) ('0' + u % 10); u /= 10; } while (u && l < 10);
	if (neg) str[l++] = '-';
	int start = l > digits ? digits : l;
	if (l < digits)
		x += (digits - l) * 24;
	for (int i = start - 1; i >= 0; i--) {
		ovl_pic(str[i] == '-' ? QGP_NUM_MINUS : QGP_NUM_0 + (str[i] - '0'), x, y);
		x += 24;
	}
}

/* Sbar_IntermissionOverlay; Sbar_FinaleOverlay and the text */
static void draw_intermission(void)
{
	ui_begin();
	ovl_tpage = -1;
	if (level_intermission == 1) {
		ui_aux(QGA_COMPLETE, 64, 24);
		ui_aux(QGA_INTER, 0, 56);
		ovl_tpage = -1;
		int secs = level_completed_ticks / GAME_HZ, dig = secs / 60, num = secs - dig * 60;
		inter_number(160, 64, dig, 3);
		ovl_pic(QGP_NUM_COLON, 234, 64);
		ovl_pic(QGP_NUM_0 + num / 10, 246, 64);
		ovl_pic(QGP_NUM_0 + num % 10, 266, 64);
		inter_number(160, 104, level_found_secrets(), 3);
		ovl_pic(QGP_NUM_SLASH, 232, 104);
		inter_number(240, 104, level_total_secrets(), 3);
		inter_number(160, 144, level_killed_monsters(), 3);
		ovl_pic(QGP_NUM_SLASH, 232, 144);
		inter_number(240, 144, level_total_monsters(), 3);
	} else if (level_intermission >= 2 && level_finale_text) {   /* SVC_FINALE: cl.intermission 2 */
		ui_aux(QGA_FINALE_L, (VID_W - 288) / 2, 16);
		ui_aux(QGA_FINALE_R, (VID_W - 288) / 2 + 144, 16);
		draw_center_string(level_finale_text, (game_tick_count - level_finale_tick) * 8 / GAME_HZ);
	}
}

void hud_draw(void)
{
	if (!gdata.ok)
		return;
	if (level_intermission) {
		draw_intermission();
		hud_invalidate();                         /* (its pictures cover the bars' rows) */
		return;
	}
	/* CL_ParseClientdata: the items just received */
	uint32_t gained = pl.items & ~seen_items;
	for (int j = 0; gained; j++, gained >>= 1)
		if (gained & 1)
			item_gettime[j] = game_tick_count;
	seen_items = pl.items;

	draw_notify();
	if (game_tick_count <= center_until) {
		ui_begin();
		draw_center_string(center_text, -1);
	}
	if (style == HUD_ORIGINAL) {                  /* the bars below the view: by parts */
		draw_original_parts();
		return;
	}
	/* the Q2 layout lies over the view, cleared every frame: drawn every
	   frame, its packets built only when what it shows changes */
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
			emit = hud_alloc;
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
	if ((pl.items & IT_INVULNERABILITY) && !pl.dead) {   /* Sbar's draw_disc over the armour */
		ui_begin();
		ui_aux(QGA_DISC, 124, 212);
	}
}
