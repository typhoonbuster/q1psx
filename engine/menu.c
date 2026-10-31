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
   Options (rows 0 and 2 of mainmenu.lmp, the only ones the cook keeps:
   rows 0 and 1 of its picture; no multiplayer, help or quit: tools/qcook/
   game.c AUX_BANDS), the single player menu's New Game (row 0 of
   sp_menu.lmp), the options the port handles; the graphics page's rows
   (engine/settings.h SET_*) */
static const uint8_t main_rows[]  = { 0, 1 };
static const uint8_t sp_rows[]    = { 0 };
#define MAIN_ITEMS         (int) sizeof main_rows
#define SINGLEPLAYER_ITEMS (int) sizeof sp_rows
enum { O_RESET, O_BRIGHTNESS, O_MUSIC, O_SOUND, O_GRAPHICS, OPTIONS_ITEMS };
#define SLIDER_RANGE       10
#define GREY               56          /* a greyed entry's shade (128: as it is) */
#define HINT_SHADE         88          /* the graphics page's hint: under the rows' 128 */
#define HINT_GAP           4           /* px between the rows and the hint, then the
                                          note: 16 rows end it at row 192, where
                                          Quake's status bars begin */

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
		"     Reset to defaults", "            Brightness", "       CD Music Volume",
		"          Sound Volume", "              Graphics" };
	draw_header(QGA_P_OPTION);
	for (int i = 0; i < OPTIONS_ITEMS; i++)
		m_print(16, 32 + i * 8, label[i], 128);
	draw_slider(220, 32 + O_BRIGHTNESS * 8, settings.brightness);   /* ((1 - v_gamma) / 0.5) */
	draw_slider(220, 32 + O_MUSIC * 8, settings.music);
	draw_slider(220, 32 + O_SOUND * 8, settings.volume);
	draw_cursor(32 + options_cursor * 8);
}

/* A text centred on the screen (M_Print's bronze), at a shade */
static void m_print_centred(int y, const char *s, int shade)
{
	m_print((VID_W - (int) strlen(s) * 8) / 2, y, s, shade);
}

/* What the highlighted row's value does to the speed and to the picture:
   two dim lines centred under the rows, the speed's first */
static void graphics_hint(int g, int y)
{
	const char *line[2];
	settings_hint(g, line);
	m_print_centred(y, line[0], HINT_SHADE);
	m_print_centred(y + 8, line[1], HINT_SHADE);
}

static void graphics_draw(void)
{
	static const char *const label[SET_ROWS] = {
		"                   HUD", "          Anti-warping", "        Dynamic lights",
		"        Frame rate cap", "       World tick rate" };
	char val[24];
	draw_header(QGA_P_OPTION);
	for (int g = 0; g < SET_ROWS; g++) {
		int y = 32 + g * 8;
		m_print(16, y, label[g], 128);
		settings_value(g, val, sizeof val);
		m_print(220, y, val, 128);
	}
	draw_cursor(32 + graphics_cursor * 8);
	graphics_hint(graphics_cursor, 32 + SET_ROWS * 8 + HINT_GAP);
}

void menu_draw(void)
{
	frame_count++;
	if (m_state == M_NONE)
		return;
	ui_begin();
	if (hooks.no_world && hooks.no_world()) {
		ui_conback();                             /* the console down: Draw_ConsoleBackground */
	} else {
		ui_fade();                                /* Draw_FadeScreen */
		hud_invalidate();                         /* (the fade darkens the bars too) */
	}
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

/* M_AdjustSliders: the brightness (v_gamma a twentieth a step) and the
   volumes, a tenth a step */
static void options_adjust(int dir)
{
	local_sound(S_MENU3);
	int *v = options_cursor == O_MUSIC ? &settings.music : options_cursor == O_SOUND ? &settings.volume
	       : options_cursor == O_BRIGHTNESS ? &settings.brightness : 0;
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
	settings_step(g, d);
	settings_apply();
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
		graphics_cursor = wrap(graphics_cursor, k == K_DOWN ? 1 : -1, SET_ROWS);
		break;
	case K_LEFT:
	case K_RIGHT:
	case K_ENTER:
		graphics_adjust(graphics_cursor, k == K_LEFT ? -1 : 1);
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
