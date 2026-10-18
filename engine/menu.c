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

/* Only what the PlayStation port does: the main menu's Single Player and
   Options (rows 0 and 2 of mainmenu.lmp), the single player menu's New Game
   (row 0 of sp_menu.lmp), the options the port handles; the graphics page's
   rows (its fidelity only when the disc carries more than one level) */
static const uint8_t main_rows[]  = { 0, 2 };
static const uint8_t sp_rows[]    = { 0 };
#define MAIN_ITEMS         (int) sizeof main_rows
#define SINGLEPLAYER_ITEMS (int) sizeof sp_rows
enum { O_RESET, O_MUSIC, O_SOUND, O_GRAPHICS, OPTIONS_ITEMS };
enum { G_FID, G_TEX, G_DITHER, G_WARP, G_T, G_BUDGET, G_HUD, G_DLIGHTS, G_WEAPON, G_FPS, G_TICK, G_COUNT };
#define SLIDER_RANGE       10
#define GREY               56          /* a greyed entry's shade (128: as it is) */

static int m_state = M_NONE;
static int m_main_cursor, m_singleplayer_cursor, options_cursor, graphics_cursor;
static int m_entersound;               /* menu2.wav after the frame is drawn */
static menu_hooks_t hooks;
static int frame_count;                /* the cursor animations' clock (realtime) */

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

/* a big menu picture (mainmenu, sp_menu): the rows of 20 of the entries
   kept, one under the other; the spinning dot at the cursor */
static void draw_big_menu(int pic, const uint8_t *rows, int items, int cursor)
{
	for (int i = 0; i < items; i++)
		ui_aux_rows(pic, 72, 32 + i * 20, rows[i] * 20, 20, 128);
	int f = frame_count * 10 / 60 % 6;           /* (int) (host_time * 10) % 6 */
	ui_aux(QGA_MENUDOT1 + f, 54, 32 + cursor * 20);
}

/* M_DrawSlider */
static void draw_slider(int x, int y, int tenths)
{
	ui_char(x - 8, y, 128, 128);
	int i;
	for (i = 0; i < SLIDER_RANGE; i++)
		ui_char(x + i * 8, y, 129, 128);
	ui_char(x + i * 8, y, 130, 128);
	ui_char(x + (SLIDER_RANGE - 1) * 8 * tenths / 10, y, 131, 128);
}

/* the options' and the graphics page's blinking cursor */
static void draw_cursor(int y)
{
	ui_char(200, y, 12 + (frame_count * 4 / 60 & 1), 128);
}

static void main_draw(void)
{
	draw_header(QGA_TTL_MAIN);
	draw_big_menu(QGA_MAINMENU, main_rows, MAIN_ITEMS, m_main_cursor);
}

static void singleplayer_draw(void)
{
	draw_header(QGA_TTL_SGL);
	draw_big_menu(QGA_SP_MENU, sp_rows, SINGLEPLAYER_ITEMS, m_singleplayer_cursor);
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
		"     Reset to defaults", "       CD Music Volume", "          Sound Volume",
		"              Graphics" };
	draw_header(QGA_P_OPTION);
	for (int i = 0; i < OPTIONS_ITEMS; i++)
		m_print(16, 32 + i * 8, label[i], 128);
	draw_slider(220, 32 + O_MUSIC * 8, settings.music);
	draw_slider(220, 32 + O_SOUND * 8, settings.volume);
	draw_cursor(32 + options_cursor * 8);
}

/* the graphics page's rows: the fidelity only when the disc has a choice */
static int graphics_rows(uint8_t rows[G_COUNT])
{
	int n = 0;
	for (int g = 0; g < G_COUNT; g++)
		if (g != G_FID || settings_fid_step(settings.fidelity, 1) != settings.fidelity)
			rows[n++] = (uint8_t) g;
	return n;
}

/* the budget does nothing outside the budget mode: greyed (the threshold
   works in every mode: the fixed mode's, the others' floor) */
static int graphics_grey(int g)
{
	return g == G_BUDGET && settings.warp != R_WARP_BUDGET;
}

static void graphics_draw(void)
{
	static const char *const label[G_COUNT] = {
		"              Fidelity", "              Textures", "             Dithering",
		"          Warp control", "             Threshold", "                Budget",
		"                   HUD", "        Dynamic lights", "           Draw weapon",
		"        Frame rate cap", "             Tick rate" };
	uint8_t rows[G_COUNT];
	int n = graphics_rows(rows);
	char val[24];
	draw_header(QGA_P_OPTION);
	for (int i = 0; i < n; i++) {
		int g = rows[i], shade = graphics_grey(g) ? GREY : 128, y = 32 + i * 8;
		const char *l = label[g];
		if (g == G_T && settings.warp != R_WARP_FIXED)
			l = "         Min threshold";          /* the dynamic modes' floor */
		m_print(16, y, l, shade);
		switch (g) {
		case G_FID:    snprintf(val, sizeof val, "%s", fidelity_name[settings.fidelity]); break;
		case G_TEX:    snprintf(val, sizeof val, "%s", settings.tex4 ? "4-bit" : "8-bit"); break;
		case G_DITHER: snprintf(val, sizeof val, "%s", settings.dither ? "on" : "off"); break;
		case G_WARP:   snprintf(val, sizeof val, "%s", warp_mode_name[settings.warp]); break;
		case G_T:      snprintf(val, sizeof val, "%d px", warp_t_px[settings.t]); break;
		case G_BUDGET: snprintf(val, sizeof val, "%d tris", warp_budgets[settings.budget]); break;
		case G_HUD:    snprintf(val, sizeof val, "%s", hud_style_name[settings.hud]); break;
		case G_DLIGHTS: snprintf(val, sizeof val, "%s", settings.dlights ? "on" : "off"); break;
		case G_WEAPON: snprintf(val, sizeof val, "%s", settings.viewmodel ? "on" : "off"); break;
		case G_FPS:    snprintf(val, sizeof val, "%d fps", fps_caps[settings.fps_cap]); break;
		default:       snprintf(val, sizeof val, "%d Hz", tick_rates[settings.tick]); break;
		}
		m_print(220, y, val, shade);
	}
	draw_cursor(32 + graphics_cursor * 8);
	if (hooks.loaded_fid && (settings.fidelity != hooks.loaded_fid() || settings.tex4 != hooks.loaded_tex4())) {
		m_print_white(16, 32 + (n + 1) * 8, "The fidelity and the textures");
		m_print_white(16, 32 + (n + 2) * 8, "change with the next level.");
	}
}

void menu_draw(void)
{
	frame_count++;
	if (m_state == M_NONE)
		return;
	ui_begin();
	ui_fade();                                    /* Draw_FadeScreen */
	hud_invalidate();                             /* (the fade darkens the bars too) */
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

static int wrap(int v, int d, int n)
{
	return (v + n + d) % n;
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
		m_main_cursor = wrap(m_main_cursor, k == K_DOWN ? 1 : -1, MAIN_ITEMS);
		break;
	case K_ENTER:
		m_entersound = 1;
		m_state = main_rows[m_main_cursor] == 0 ? M_SINGLEPLAYER : M_OPTIONS;
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
		m_singleplayer_cursor = wrap(m_singleplayer_cursor, k == K_DOWN ? 1 : -1, SINGLEPLAYER_ITEMS);
		break;
	case K_ENTER:
		m_entersound = 1;
		if (hooks.in_game && hooks.in_game())
			m_state = M_NEWGAME;              /* "Are you sure ..." */
		else
			new_game();
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
	int *v = options_cursor == O_MUSIC ? &settings.music : options_cursor == O_SOUND ? &settings.volume : 0;
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
		if (options_cursor == O_RESET) {          /* exec default.cfg */
			settings_defaults();
			settings_apply();
		} else if (options_cursor == O_GRAPHICS) {
			m_state = M_GRAPHICS;
			graphics_cursor = 0;
		} else {
			options_adjust(1);
		}
		break;
	case K_UP:
	case K_DOWN:
		local_sound(S_MENU1);
		options_cursor = wrap(options_cursor, k == K_DOWN ? 1 : -1, OPTIONS_ITEMS);
		break;
	case K_LEFT:
	case K_RIGHT:
		options_adjust(k == K_RIGHT ? 1 : -1);
		break;
	}
}

static void graphics_adjust(int g, int d)
{
	local_sound(S_MENU3);
	switch (g) {
	case G_FID:    settings.fidelity = settings_fid_step(settings.fidelity, d); break;
	case G_TEX:    settings.tex4 ^= 1; break;
	case G_DITHER: settings.dither ^= 1; break;
	case G_WARP:   settings.warp = wrap(settings.warp, d, R_WARP_MODES); break;
	case G_T:      settings.t = wrap(settings.t, d, num_warp_t); break;
	case G_BUDGET: settings.budget = wrap(settings.budget, d, num_warp_budget); break;
	case G_HUD:    settings.hud = wrap(settings.hud, d, HUD_STYLES); break;
	case G_DLIGHTS: settings.dlights ^= 1; break;
	case G_WEAPON: settings.viewmodel ^= 1; break;
	case G_FPS:    settings.fps_cap = wrap(settings.fps_cap, d, num_fps_caps); break;
	default:       settings.tick = wrap(settings.tick, d, num_tick_rates); break;
	}
	settings_apply();                             /* the fidelity and the textures: at a load */
}

static void graphics_key(int k)
{
	uint8_t rows[G_COUNT];
	int n = graphics_rows(rows);
	if (graphics_cursor >= n)
		graphics_cursor = n - 1;
	switch (k) {
	case K_ESCAPE:
		m_state = M_OPTIONS;
		m_entersound = 1;
		break;
	case K_UP:
	case K_DOWN:                                  /* (the greyed budget skipped) */
		local_sound(S_MENU1);
		for (int i = 0; i < n; i++) {
			graphics_cursor = wrap(graphics_cursor, k == K_DOWN ? 1 : -1, n);
			if (!graphics_grey(rows[graphics_cursor]))
				break;
		}
		break;
	case K_LEFT:
	case K_RIGHT:
	case K_ENTER:
		if (!graphics_grey(rows[graphics_cursor]))
			graphics_adjust(rows[graphics_cursor], k == K_LEFT ? -1 : 1);
		break;
	}
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
