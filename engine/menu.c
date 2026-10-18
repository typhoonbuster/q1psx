/*
 * q1psx — Quake's menu (WinQuake menu.c), transcribed for the pad: the
 * main menu (M_Main_Draw / M_Main_Key), the single player menu and its
 * new-game question (SCR_ModalMessage), the options (M_Options_Draw /
 * M_AdjustSliders) and, in the place of the video modes, the graphics
 * settings (engine/settings.h). Pictures and text are gfx.wad's and the
 * pak's (formats/qgame.h QGA_*: the 4-bit aux pictures), drawn through
 * engine/ui.c at Quake's coordinates on its 320 x 240 screen.
 */

#include <stdio.h>
#include <string.h>

#include "game.h"
#include "gdata.h"
#include "hud.h"
#include "menu.h"
#include "render.h"
#include "settings.h"
#include "snd.h"
#include "ui.h"
#include "vid.h"
#include "world.h"

enum { M_NONE, M_MAIN, M_SINGLEPLAYER, M_NEWGAME, M_OPTIONS, M_GRAPHICS };

#define MAIN_ITEMS          5
#define SINGLEPLAYER_ITEMS  3
#define OPTIONS_ITEMS      13
#define GRAPHICS_ITEMS      8
#define SLIDER_RANGE       10
#define GREY               56          /* a greyed entry's shade (128: as it is) */

static int m_state = M_NONE;
static int m_main_cursor, m_singleplayer_cursor, graphics_cursor;
static int options_cursor = 2;         /* (the first two are greyed: Reset to defaults) */
static int m_entersound;               /* menu2.wav after the frame is drawn */
static menu_hooks_t hooks;
static int frame_count;                /* the cursor animations' clock (realtime) */

/* the entries with no PlayStation counterpart */
static const uint8_t main_grey[MAIN_ITEMS]                 = { 0, 1, 0, 1, 1 };
static const uint8_t singleplayer_grey[SINGLEPLAYER_ITEMS] = { 0, 1, 1 };
static const uint8_t options_grey[OPTIONS_ITEMS]           = { 1, 1, 0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0 };

void menu_init(const menu_hooks_t *h)
{
	hooks = *h;
}

int menu_active(void)
{
	return m_state != M_NONE;
}

static void local_sound(int s)              /* S_LocalSound */
{
	static const int32_t origin[3] = { 0, 0, 0 };
	snd_start(ENT_PLAYER, -1, s, origin, 255, ATTN_NONE);
}

void menu_open(void)
{
	m_state = M_MAIN;
	m_entersound = 1;
#if Q1PSX_PADSCRIPT
	printf("menu: open\n");
#endif
}

/* ------------------------------------------------------------- drawing -- */

static int aux_w(int id)
{
	return gdata.aux ? gdata.aux[id].w : 0;
}

static int aux_h(int id)
{
	return gdata.aux ? gdata.aux[id].h : 0;
}

/* M_Print: the bronze characters (+128); M_PrintWhite */
static void m_print(int x, int y, const char *s, int shade)
{
	ui_text_shaded(x, y, s, 128, shade);
}

static void m_print_white(int x, int y, const char *s)
{
	ui_text_shaded(x, y, s, 0, 128);
}

/* the qplaque on the left, a title centred */
static void draw_header(int title)
{
	ui_aux(QGA_QPLAQUE, 16, 4);
	ui_aux(title, (320 - aux_w(title)) / 2, 4);
}

/* a big menu picture (mainmenu, sp_menu): its rows of 20 per entry, the
   greyed ones darker; the spinning dot at the cursor */
static void draw_big_menu(int pic, int items, const uint8_t *grey, int cursor)
{
	for (int i = 0; i < items; i++) {
		int h = i == items - 1 ? aux_h(pic) - i * 20 : 20;
		ui_aux_rows(pic, 72, 32 + i * 20, i * 20, h, grey[i] ? GREY : 128);
	}
	int f = frame_count * 10 / 60 % 6;           /* (int) (host_time * 10) % 6 */
	ui_aux(QGA_MENUDOT1 + f, 54, 32 + cursor * 20);
}

/* M_DrawSlider */
static void draw_slider(int x, int y, int tenths, int shade)
{
	ui_char(x - 8, y, 128, shade);
	int i;
	for (i = 0; i < SLIDER_RANGE; i++)
		ui_char(x + i * 8, y, 129, shade);
	ui_char(x + i * 8, y, 130, shade);
	ui_char(x + (SLIDER_RANGE - 1) * 8 * tenths / 10, y, 131, shade);
}

/* M_DrawCheckbox: "on" / "off" in the bronze characters */
static void draw_checkbox(int x, int y, int on, int shade)
{
	m_print(x, y, on ? "on" : "off", shade);
}

/* the options' and the graphics page's blinking cursor */
static void draw_cursor(int y)
{
	ui_char(200, y, 12 + (frame_count * 4 / 60 & 1), 128);
}

static void main_draw(void)
{
	draw_header(QGA_TTL_MAIN);
	draw_big_menu(QGA_MAINMENU, MAIN_ITEMS, main_grey, m_main_cursor);
}

static void singleplayer_draw(void)
{
	draw_header(QGA_TTL_SGL);
	draw_big_menu(QGA_SP_MENU, SINGLEPLAYER_ITEMS, singleplayer_grey, m_singleplayer_cursor);
}

/* SCR_ModalMessage's screen: the text centred from 35 % down (SCR_DrawNotifyString) */
static void newgame_draw(void)
{
	static const char *const lines[] = {
		"Are you sure you want to", "start a new game?", "", "Cross: yes   Circle: no" };
	for (int i = 0; i < 4; i++)
		m_print_white((VID_W - (int) strlen(lines[i]) * 8) / 2, VID_H * 35 / 100 + i * 8, lines[i]);
}

static void options_draw(void)
{
	static const char *const label[OPTIONS_ITEMS] = {
		"    Customize controls", "         Go to console", "     Reset to defaults",
		"           Screen size", "            Brightness", "           Mouse Speed",
		"       CD Music Volume", "          Sound Volume", "            Always Run",
		"          Invert Mouse", "            Lookspring", "            Lookstrafe",
		"              Graphics" };
	draw_header(QGA_P_OPTION);
	for (int i = 0; i < OPTIONS_ITEMS; i++)
		m_print(16, 32 + i * 8, label[i], options_grey[i] ? GREY : 128);
	draw_slider(220, 56, 7, GREY);                /* viewsize 100 of 30 .. 120 */
	draw_slider(220, 64, 0, GREY);                /* gamma 1 */
	draw_slider(220, 72, 2, GREY);                /* sensitivity 3 of 1 .. 11 */
	draw_slider(220, 80, settings.music, 128);
	draw_slider(220, 88, settings.volume, 128);
	draw_checkbox(220, 96, 1, GREY);              /* (the walk is Triangle's) */
	draw_checkbox(220, 104, 0, GREY);
	draw_checkbox(220, 112, 0, GREY);
	draw_checkbox(220, 120, 0, GREY);
	draw_cursor(32 + options_cursor * 8);
}

/* which graphics entries do nothing as the others are set: the budget is
   the budget mode's alone; the fidelity has no other level on an ultra disc
   (the threshold works in every mode: the fixed mode's, the others' floor) */
static int graphics_grey(int i)
{
	switch (i) {
	case 0:  return settings_fid_step(settings.fidelity, 1) == settings.fidelity;
	case 5:  return settings.warp != R_WARP_BUDGET;
	default: return 0;
	}
}

static void graphics_draw(void)
{
	static const char *const label[GRAPHICS_ITEMS] = {
		"              Fidelity", "              Textures", "             Dithering",
		"          Warp control", "             Threshold", "                Budget",
		"                   HUD", "        Dynamic lights" };
	char val[24];
	draw_header(QGA_P_OPTION);
	for (int i = 0; i < GRAPHICS_ITEMS; i++) {
		int shade = graphics_grey(i) ? GREY : 128, y = 32 + i * 8;
		const char *l = label[i];
		if (i == 4 && settings.warp != R_WARP_FIXED)
			l = "         Min threshold";          /* the dynamic modes' floor */
		m_print(16, y, l, shade);
		switch (i) {
		case 0: snprintf(val, sizeof val, "%s", fidelity_name[settings.fidelity]); break;
		case 1: snprintf(val, sizeof val, "%s", settings.tex4 ? "4-bit" : "8-bit"); break;
		case 2: snprintf(val, sizeof val, "%s", settings.dither ? "on" : "off"); break;
		case 3: snprintf(val, sizeof val, "%s", warp_mode_name[settings.warp]); break;
		case 4: snprintf(val, sizeof val, "%d px", warp_t_px[settings.t]); break;
		case 5: snprintf(val, sizeof val, "%d tris", warp_budgets[settings.budget]); break;
		case 6: snprintf(val, sizeof val, "%s", hud_style_name[settings.hud]); break;
		default: snprintf(val, sizeof val, "%s", settings.dlights ? "on" : "off"); break;
		}
		m_print(220, y, val, shade);
	}
	draw_cursor(32 + graphics_cursor * 8);
	if (hooks.loaded_fid && (settings.fidelity != hooks.loaded_fid() || settings.tex4 != hooks.loaded_tex4())) {
		m_print_white(16, 32 + (GRAPHICS_ITEMS + 1) * 8, "The fidelity and the textures");
		m_print_white(16, 32 + (GRAPHICS_ITEMS + 2) * 8, "change with the next level.");
	}
}

void menu_draw(void)
{
	frame_count++;
	if (m_state == M_NONE)
		return;
	ui_begin();
	ui_fade();                                    /* Draw_FadeScreen */
	ui_begin();
	switch (m_state) {
	case M_MAIN:         main_draw(); break;
	case M_SINGLEPLAYER: singleplayer_draw(); break;
	case M_NEWGAME:      newgame_draw(); break;
	case M_OPTIONS:      options_draw(); break;
	case M_GRAPHICS:     graphics_draw(); break;
	default: break;
	}
	if (m_entersound) {
		local_sound(S_MENU2);
		m_entersound = 0;
	}
}

/* ---------------------------------------------------------------- keys -- */

enum { K_NONE, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_ENTER, K_ESCAPE };

/* the cursor over the entries that do something */
static int step_cursor(int c, int d, int items, const uint8_t *grey, int (*grey_fn)(int))
{
	for (int i = 0; i < items; i++) {
		c = (c + items + d) % items;
		if (!(grey ? grey[c] : grey_fn(c)))
			break;
	}
	return c;
}

static void main_key(int k)
{
	switch (k) {
	case K_ESCAPE:
		m_state = M_NONE;
		break;
	case K_DOWN:
	case K_UP:
		local_sound(S_MENU1);
		m_main_cursor = step_cursor(m_main_cursor, k == K_DOWN ? 1 : -1, MAIN_ITEMS, main_grey, 0);
		break;
	case K_ENTER:
		m_entersound = 1;
		if (m_main_cursor == 0)
			m_state = M_SINGLEPLAYER;
		else if (m_main_cursor == 2)
			m_state = M_OPTIONS;
		break;
	}
}

static void new_game(void)
{
#if Q1PSX_PADSCRIPT
	printf("menu: new game\n");
#endif
	m_state = M_NONE;
	if (hooks.new_game)
		hooks.new_game();
}

static void singleplayer_key(int k)
{
	switch (k) {
	case K_ESCAPE:
		m_state = M_MAIN;
		m_entersound = 1;
		break;
	case K_DOWN:
	case K_UP:
		local_sound(S_MENU1);
		m_singleplayer_cursor = step_cursor(m_singleplayer_cursor, k == K_DOWN ? 1 : -1,
		                                    SINGLEPLAYER_ITEMS, singleplayer_grey, 0);
		break;
	case K_ENTER:
		m_entersound = 1;
		if (m_singleplayer_cursor == 0) {
			if (hooks.in_game && hooks.in_game())
				m_state = M_NEWGAME;          /* "Are you sure ..." */
			else
				new_game();
		}
		break;
	}
}

static void newgame_key(int k)
{
	if (k == K_ENTER)
		new_game();
	else if (k == K_ESCAPE)
		m_state = M_SINGLEPLAYER;
}

/* M_AdjustSliders: the volumes, a tenth a step */
static void options_adjust(int dir)
{
	local_sound(S_MENU3);
	int *v = options_cursor == 6 ? &settings.music : options_cursor == 7 ? &settings.volume : 0;
	if (!v)
		return;
	*v += dir;
	if (*v < 0)  *v = 0;
	if (*v > 10) *v = 10;
	settings_apply();
}

static void options_key(int k)
{
	switch (k) {
	case K_ESCAPE:
		m_state = M_MAIN;
		m_entersound = 1;
		break;
	case K_ENTER:
		m_entersound = 1;
		if (options_cursor == 2) {                /* exec default.cfg */
			settings_defaults();
			settings_apply();
		} else if (options_cursor == 12) {
			m_state = M_GRAPHICS;
			if (graphics_grey(graphics_cursor))   /* (one fidelity on the disc) */
				graphics_cursor = step_cursor(graphics_cursor, 1, GRAPHICS_ITEMS, 0, graphics_grey);
		} else {
			options_adjust(1);
		}
		break;
	case K_UP:
	case K_DOWN:
		local_sound(S_MENU1);
		options_cursor = step_cursor(options_cursor, k == K_DOWN ? 1 : -1, OPTIONS_ITEMS, options_grey, 0);
		break;
	case K_LEFT:
	case K_RIGHT:
		options_adjust(k == K_RIGHT ? 1 : -1);
		break;
	}
}

static int wrap(int v, int d, int n)
{
	return (v + n + d) % n;
}

static void graphics_adjust(int d)
{
	local_sound(S_MENU3);
	switch (graphics_cursor) {
	case 0: settings.fidelity = settings_fid_step(settings.fidelity, d); break;
	case 1: settings.tex4 ^= 1; break;
	case 2: settings.dither ^= 1; break;
	case 3: settings.warp = wrap(settings.warp, d, R_WARP_MODES); break;
	case 4: settings.t = wrap(settings.t, d, num_warp_t); break;
	case 5: settings.budget = wrap(settings.budget, d, num_warp_budget); break;
	case 6: settings.hud = wrap(settings.hud, d, HUD_STYLES); break;
	default: settings.dlights ^= 1; break;
	}
	settings_apply();                             /* the fidelity and the textures: at a load */
}

static void graphics_key(int k)
{
	switch (k) {
	case K_ESCAPE:
		m_state = M_OPTIONS;
		m_entersound = 1;
		break;
	case K_UP:
	case K_DOWN:
		local_sound(S_MENU1);
		graphics_cursor = step_cursor(graphics_cursor, k == K_DOWN ? 1 : -1, GRAPHICS_ITEMS, 0, graphics_grey);
		break;
	case K_LEFT:
	case K_RIGHT:
	case K_ENTER:
		if (!graphics_grey(graphics_cursor))
			graphics_adjust(k == K_LEFT ? -1 : 1);
		break;
	}
	/* the entry under the cursor greyed by the change (the budget when the
	   warp leaves the budget mode): the cursor stays, as on any entry */
}

/* the pad as Quake's keys: one a frame, the arrows repeating while held
   (after half a second, ten a second) */
static int pad_key(const pad_t *p)
{
	static int held_dir, held_frames;
	if (p->pressed & PAD_START)
		return K_ESCAPE;                          /* the menu key: back, or out at the main menu */
	if (p->pressed & (PAD_TRIANGLE | PAD_CIRCLE))
		return K_ESCAPE;
	if (p->pressed & PAD_CROSS)
		return K_ENTER;
	static const uint16_t dirs[4] = { PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT };
	for (int i = 0; i < 4; i++)
		if (p->pressed & dirs[i]) {
			held_dir = i;
			held_frames = 0;
			return K_UP + i;
		}
	if (p->held & dirs[held_dir]) {
		if (++held_frames >= 30 && (held_frames - 30) % 6 == 0)
			return K_UP + held_dir;
	} else {
		held_frames = 0;
	}
	return K_NONE;
}

void menu_key(const pad_t *p)
{
	if (m_state == M_NONE)
		return;
	int k = pad_key(p);
	if (k == K_NONE)
		return;
#if Q1PSX_PADSCRIPT
	printf("menu: state %d key %d (main %d sp %d options %d graphics %d)\n", m_state, k, m_main_cursor,
	       m_singleplayer_cursor, options_cursor, graphics_cursor);
#endif
	switch (m_state) {
	case M_MAIN:         main_key(k); break;
	case M_SINGLEPLAYER: singleplayer_key(k); break;
	case M_NEWGAME:      newgame_key(k); break;
	case M_OPTIONS:      options_key(k); break;
	case M_GRAPHICS:     graphics_key(k); break;
	default: break;
	}
}
