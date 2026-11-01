/*
 * q1psx — Quake's menu (WinQuake menu.c), transcribed for the pad: the
 * main menu (M_Main_Draw / M_Main_Key), the single player menu with its
 * New Game, Load and Save (the new game's page is the game options' page,
 * M_GameOptions_Draw: the skill, the episode and the level, Quake's level
 * select; Load and Save, M_Load_Draw / M_Save_Draw, on the memory card:
 * engine/card.h), the options (M_Options_Draw / M_AdjustSliders, with the
 * PlayStation's mouse and vibration) and their pages: the controls
 * (M_Keys_Draw, read only: the controls are fixed) and, in the place of
 * the video modes, the graphics settings (engine/settings.h). Pictures and
 * text are gfx.wad's and the pak's (formats/qgame.h QGA_*: the 4-bit aux
 * pictures), drawn through engine/ui.c at Quake's coordinates on its
 * 320 x 240 screen.
 */

#include <string.h>

#include "card.h"
#include "game.h"
#include "gdata.h"
#include "hud.h"
#include "mem.h"
#include "menu.h"
#include "pad.h"
#include "q_fmt.h"
#include "render.h"
#include "settings.h"
#include "snd.h"
#include "ui.h"
#include "vid.h"
#include "world.h"

enum { M_NONE, M_MAIN, M_SINGLEPLAYER, M_GAMEOPTIONS, M_NEWGAME, M_LOAD, M_SAVE, M_CARD, M_FORMAT,
       M_OPTIONS, M_KEYS, M_GRAPHICS };

/* Only what the PlayStation port does: the main menu's Single Player and
   Options (rows 0 and 2 of mainmenu.lmp, the only ones the cook keeps:
   rows 0 and 1 of its picture; no multiplayer, help or quit: tools/qcook/
   game.c AUX_BANDS), the single player menu's three (sp_menu.lmp), the
   options the port handles; the graphics page's rows (engine/settings.h
   SET_*) */
static const uint8_t main_rows[]  = { 0, 1 };
static const uint8_t sp_rows[]    = { 0, 1, 2 };
#define MAIN_ITEMS         (int) sizeof main_rows
#define SINGLEPLAYER_ITEMS (int) sizeof sp_rows
enum { O_KEYS, O_RESET, O_BRIGHTNESS, O_MOUSE_SPEED, O_MUSIC, O_SOUND, O_INVERT, O_USE_MOUSE, O_SWAP,
       O_VIBRATION, O_GRAPHICS, OPTIONS_ITEMS };
#define SLIDER_RANGE       10
#define HINT_SHADE         88          /* the graphics page's hint: under the rows' 128 */
#define HINT_GAP           4           /* px between the rows and the hint, then the
                                          note: 16 rows end it at row 192, where
                                          Quake's status bars begin */
#define GREY               64          /* an entry that cannot be taken now */

static int m_state = M_NONE;
static int m_main_cursor, m_singleplayer_cursor, options_cursor, graphics_cursor;
static int load_cursor, keys_scheme;
static int m_entersound;               /* menu2.wav after the frame is drawn */
static menu_hooks_t hooks;
static int frame_count;                /* the cursor animations' clock (realtime) */
static uint8_t options_were[CARD_SETTINGS];   /* the settings as the options found them */
static const char *card_msg[4];        /* M_CARD's lines */

void menu_init(const menu_hooks_t *h)
{
	hooks = *h;
}

int menu_active(void)
{
	return m_state != M_NONE;
}

static void local_sound(int s)       /* S_LocalSound */
{
	static const int32_t origin[3] = { 0, 0, 0 };
	snd_start(ENT_PLAYER, -1, s, origin, 255, ATTN_NONE);
}

void menu_open(void)
{
	m_state = M_MAIN;
	m_entersound = 1;
}

/* --------------------------------------------------------- the levels -- */

/* menu.c's levels and episodes (the deathmatch arena left out: single
   player only); an episode is offered when its first level is on the disc
   (the shareware disc: Welcome to Quake and Doomed Dimension) */
typedef struct { const char *name, *description; } level_t;
typedef struct { const char *description; uint8_t first, levels; } episode_t;
static const level_t levels[] = {
	{ "start", "Entrance" },
	{ "e1m1", "Slipgate Complex" }, { "e1m2", "Castle of the Damned" }, { "e1m3", "The Necropolis" },
	{ "e1m4", "The Grisly Grotto" }, { "e1m5", "Gloom Keep" }, { "e1m6", "The Door To Chthon" },
	{ "e1m7", "The House of Chthon" }, { "e1m8", "Ziggurat Vertigo" },
	{ "e2m1", "The Installation" }, { "e2m2", "Ogre Citadel" }, { "e2m3", "Crypt of Decay" },
	{ "e2m4", "The Ebon Fortress" }, { "e2m5", "The Wizard's Manse" }, { "e2m6", "The Dismal Oubliette" },
	{ "e2m7", "Underearth" },
	{ "e3m1", "Termination Central" }, { "e3m2", "The Vaults of Zin" }, { "e3m3", "The Tomb of Terror" },
	{ "e3m4", "Satan's Dark Delight" }, { "e3m5", "Wind Tunnels" }, { "e3m6", "Chambers of Torment" },
	{ "e3m7", "The Haunted Halls" },
	{ "e4m1", "The Sewage System" }, { "e4m2", "The Tower of Despair" }, { "e4m3", "The Elder God Shrine" },
	{ "e4m4", "The Palace of Hate" }, { "e4m5", "Hell's Atrium" }, { "e4m6", "The Pain Maze" },
	{ "e4m7", "Azure Agony" }, { "e4m8", "The Nameless City" },
	{ "end", "Shub-Niggurath's Pit" },
};
static const episode_t episodes[] = {
	{ "Welcome to Quake", 0, 1 }, { "Doomed Dimension", 1, 8 }, { "Realm of Black Magic", 9, 7 },
	{ "Netherworld", 16, 7 }, { "The Elder World", 23, 8 }, { "Final Level", 31, 1 },
};
#define EPISODES (int) (sizeof episodes / sizeof episodes[0])
static int startepisode, startlevel, startskill = 1;

const char *menu_level_name(const char *map)
{
	for (size_t i = 0; map && i < sizeof levels / sizeof levels[0]; i++) {
		const char *a = levels[i].name, *b = map;
		while (*a && (*a == *b || *a == *b + 32))
			a++, b++;
		if (!*a && !*b)
			return levels[i].description;
	}
	return 0;
}

static int episode_on_disc(int e)
{
	return world_find_map(levels[episodes[e].first].name) >= 0;
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
   kept, one under the other, each at its shade; the spinning dot at the
   cursor */
static void draw_big_menu(int pic, const uint8_t *rows, int items, int cursor, const uint8_t *shade)
{
	for (int i = 0; i < items; i++)
		ui_aux_rows(pic, 72, 32 + i * 20, rows[i] * 20, 20, shade ? shade[i] : 128);
	int f = frame_count * 10 / 60 % 6;           /* (int) (host_time * 10) % 6 */
	ui_aux(QGA_MENUDOT1 + f, 54, 32 + cursor * 20);
}

/* M_DrawTextBox: the box's corners and edges around width x lines
   characters */
static void draw_text_box(int x, int y, int width, int lines)
{
	int cx = x, cy = y;
	ui_aux(QGA_BOX_TL, cx, cy);
	for (int n = 0; n < lines; n++)
		ui_aux(QGA_BOX_ML, cx, cy += 8);
	ui_aux(QGA_BOX_BL, cx, cy + 8);
	for (cx += 8; width > 0; width -= 2, cx += 16) {
		cy = y;
		ui_aux(QGA_BOX_TM, cx, cy);
		for (int n = 0; n < lines; n++)
			ui_aux(n == 1 ? QGA_BOX_MM2 : QGA_BOX_MM, cx, cy += 8);
		ui_aux(QGA_BOX_BM, cx, cy + 8);
	}
	cy = y;
	ui_aux(QGA_BOX_TR, cx, cy);
	for (int n = 0; n < lines; n++)
		ui_aux(QGA_BOX_MR, cx, cy += 8);
	ui_aux(QGA_BOX_BR, cx, cy + 8);
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

/* M_DrawCheckbox */
static void draw_checkbox(int x, int y, int on)
{
	m_print(x, y, on ? "on" : "off", 128);
}

/* the blinking cursor (12, 13) */
static void draw_cursor_at(int x, int y)
{
	ui_char(x, y, 12 + (frame_count * 4 / 60 & 1), 128);
}

static void draw_cursor(int y)
{
	draw_cursor_at(200, y);
}

/* A text centred on the screen, at a shade (M_Print's bronze) or white */
static void m_print_centred(int y, const char *s, int shade)
{
	m_print((VID_W - (int) strlen(s) * 8) / 2, y, s, shade);
}

/* SCR_ModalMessage's screen: lines centred from 35 % down
   (SCR_DrawNotifyString) */
static void draw_message(const char *const *lines, int n)
{
	for (int i = 0; i < n; i++)
		if (lines[i])
			m_print_white((VID_W - (int) strlen(lines[i]) * 8) / 2, VID_H * 35 / 100 + i * 8, lines[i]);
}

static int can_save(void)
{
	return hooks.can_save && hooks.can_save();
}

static void main_draw(void)
{
	draw_header(QGA_TTL_MAIN);
	draw_big_menu(QGA_MAINMENU, main_rows, MAIN_ITEMS, m_main_cursor, 0);
}

static void singleplayer_draw(void)
{
	uint8_t shade[SINGLEPLAYER_ITEMS] = { 128, 128, (uint8_t) (can_save() ? 128 : GREY) };
	draw_header(QGA_TTL_SGL);
	draw_big_menu(QGA_SP_MENU, sp_rows, SINGLEPLAYER_ITEMS, m_singleplayer_cursor, shade);
}

/* M_GameOptions_Draw, single player's rows: begin game, the skill, the
   episode and the level (its name under it) */
static const uint8_t gameoptions_cursor_table[] = { 40, 56, 64, 72 };
#define NUM_GAMEOPTIONS (int) sizeof gameoptions_cursor_table
static int gameoptions_cursor;

static void gameoptions_draw(void)
{
	static const char *const skill[4] = { "Easy difficulty", "Normal difficulty", "Hard difficulty",
	                                      "Nightmare difficulty" };
	draw_header(QGA_TTL_SGL);
	draw_text_box(152, 32, 10, 1);
	m_print(160, 40, "begin game", 128);
	m_print(0, 56, "            Skill", 128);
	m_print(160, 56, skill[startskill], 128);
	m_print(0, 64, "         Episode", 128);
	m_print(160, 64, episodes[startepisode].description, 128);
	m_print(0, 72, "           Level", 128);
	const level_t *l = &levels[episodes[startepisode].first + startlevel];
	m_print(160, 72, l->description, 128);
	m_print(160, 80, l->name, 128);
	draw_cursor_at(144, gameoptions_cursor_table[gameoptions_cursor]);
}

static void newgame_draw(void)
{
	static const char *const lines[] = {
		"Are you sure you want to", "start a new game?", "", "Cross: yes   Circle: no" };
	draw_message(lines, 4);
}

/* a slot's line (M_ScanSaves' comment): the level's name, its skill */
static void slot_line(int i, char *buf, int size)
{
	static const char *const skill[4] = { "Easy", "Normal", "Hard", "Nightmare" };
	const card_slot_t *s = &card_save.slot[i];
	if (!s->map[0]) {
		q_snprintf(buf, size, "%s", i ? "--- UNUSED SLOT ---" : "--- NO AUTOSAVE ---");
		return;
	}
	const char *name = menu_level_name(s->map);
	q_snprintf(buf, size, "%-21s %-9s %s", name ? name : s->map, skill[s->start.skill & 3], i ? "" : "auto");
}

/* M_Load_Draw: the autosave, then the twelve slots; M_Save_Draw: the
   twelve slots, the note that a save keeps the level's start */
static void load_draw(int save)
{
	char line[48];
	int first = save ? 1 : 0;
	ui_aux(save ? QGA_P_SAVE : QGA_P_LOAD, (320 - aux_w(save ? QGA_P_SAVE : QGA_P_LOAD)) / 2, 4);
	for (int i = first; i <= CARD_SLOTS; i++) {
		slot_line(i, line, sizeof line);
		m_print(16, 32 + 8 * (i - first), line, 128);
	}
	draw_cursor_at(8, 32 + load_cursor * 8);
	m_print_centred(32 + 8 * (CARD_SLOTS + 1 - first) + 8,
	                save ? "a save keeps the start of the level" : "a game loads at its level's start",
	                HINT_SHADE);
}

static void card_draw(void)
{
	draw_message(card_msg, 4);
}

static void format_draw(void)
{
	static const char *const lines[] = {
		"The memory card in slot 1", "is not formatted. Format it?", "", "Cross: yes   Circle: no" };
	draw_message(lines, 4);
}

static void options_draw(void)
{
	static const char *const label[OPTIONS_ITEMS] = {
		"    Customize controls", "     Reset to defaults", "            Brightness", "           Mouse Speed",
		"       CD Music Volume", "          Sound Volume", "         Invert Y Axis", "             Use Mouse",
		"    Swap Mouse Buttons", "             Vibration", "              Graphics" };
	draw_header(QGA_P_OPTION);
	for (int i = 0; i < OPTIONS_ITEMS; i++)
		m_print(16, 32 + i * 8, label[i], 128);
	draw_slider(220, 32 + O_BRIGHTNESS * 8, settings.brightness);   /* ((1 - v_gamma) / 0.5) */
	draw_slider(220, 32 + O_MOUSE_SPEED * 8, (settings.mouse_speed - 2) / 2);   /* (sensitivity - 1) / 10 */
	draw_slider(220, 32 + O_MUSIC * 8, settings.music);
	draw_slider(220, 32 + O_SOUND * 8, settings.volume);
	draw_checkbox(220, 32 + O_INVERT * 8, settings.invert_y);
	draw_checkbox(220, 32 + O_USE_MOUSE * 8, settings.mouse);
	draw_checkbox(220, 32 + O_SWAP * 8, settings.swap_mouse);
	draw_checkbox(220, 32 + O_VIBRATION * 8, settings.vibration);
	draw_cursor(32 + options_cursor * 8);
	if (options_cursor == O_USE_MOUSE && settings.mouse)   /* (port 2 polled: is one there?) */
		m_print_centred(32 + OPTIONS_ITEMS * 8 + HINT_GAP,
		                pad.mouse ? "the mouse plays, the pad moves" : "no mouse found in port 2", HINT_SHADE);
}

/* M_Keys_Draw, read only: each action's buttons in one of the three
   layouts (engine/main.c player_cmd), left and right switching */
enum { KS_ANALOG, KS_DIGITAL, KS_MOUSE, KEY_SCHEMES };
static void keys_draw(void)
{
	static const char *const scheme[KEY_SCHEMES] = {
		"analog controller", "digital controller", "mouse and controller" };
	static const char *const rows[KEY_SCHEMES][12][2] = {
		{ { "attack", "R2 or SQUARE" }, { "change weapon", "R1" }, { "previous weapon", "L1" },
		  { "jump / swim up", "L2 or CROSS" }, { "move", "LEFT STICK or D-PAD" }, { "turn and look", "RIGHT STICK" },
		  { "walk", "TRIANGLE" }, { "center view", "CIRCLE" }, { "menu", "START" } },
		{ { "attack", "R2 or SQUARE" }, { "change weapon", "R1" }, { "previous weapon", "L1" },
		  { "jump / swim up", "L2 or CROSS" }, { "walk forward", "UP" }, { "backpedal", "DOWN" },
		  { "turn left", "LEFT" }, { "turn right", "RIGHT" }, { "sidestep", "TRIANGLE + LEFT/RIGHT" },
		  { "look up / down", "TRIANGLE + UP/DOWN" }, { "center view", "CIRCLE" }, { "menu", "START" } },
		{ { "attack", 0 }, { "jump / swim up", 0 }, { "turn and look", "MOUSE" },
		  { "walk forward", "UP or TRIANGLE" }, { "backpedal", "DOWN or CROSS" }, { "step left", "LEFT or SQUARE" },
		  { "step right", "RIGHT or CIRCLE" }, { "move", "either STICK" }, { "change weapon", "L1 or R1" },
		  { "previous weapon", "L2 or R2" }, { "menu", "START" } },
	};
	char head[40];
	ui_aux(QGA_TTL_CSTM, (320 - aux_w(QGA_TTL_CSTM)) / 2, 4);
	q_snprintf(head, sizeof head, "< %s >", scheme[keys_scheme]);
	m_print_white((VID_W - (int) strlen(head) * 8) / 2, 32, head);
	for (int i = 0; i < 12 && rows[keys_scheme][i][0]; i++) {
		const char *k = rows[keys_scheme][i][1];
		if (!k)                                   /* the mouse's buttons, as swapped */
			k = (i == 0) != (settings.swap_mouse != 0) ? "MOUSE1 (left)" : "MOUSE2 (right)";
		m_print(16, 48 + 8 * i, rows[keys_scheme][i][0], 128);
		m_print(140, 48 + 8 * i, k, 128);
	}
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

static void draw_state(void)
{
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
	case M_GAMEOPTIONS:  gameoptions_draw(); break;
	case M_NEWGAME:      newgame_draw(); break;
	case M_LOAD:         load_draw(0); break;
	case M_SAVE:         load_draw(1); break;
	case M_CARD:         card_draw(); break;
	case M_FORMAT:       format_draw(); break;
	case M_OPTIONS:      options_draw(); break;
	case M_KEYS:         keys_draw(); break;
	case M_GRAPHICS:     graphics_draw(); break;
	default: break;
	}
	if (m_entersound) {
		local_sound(S_MENU2);
		m_entersound = 0;
	}
}

void menu_draw(void)
{
	frame_count++;
	if (m_state != M_NONE)
		draw_state();
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
		if (main_rows[m_main_cursor] == 0) {
			m_state = M_SINGLEPLAYER;
		} else {
			m_state = M_OPTIONS;
			settings_pack(options_were);
		}
		break;
	}
}

/* a card message (M_CARD), back to the single player menu on any key */
static void card_message(const char *a, const char *b, const char *c)
{
	card_msg[0] = a;
	card_msg[1] = b;
	card_msg[2] = c;
	card_msg[3] = 0;
	m_state = M_CARD;
}

/* the card read for Load (save 0) or Save (1): their list, or what keeps
   them from it */
static void card_menu(int save)
{
	int state = card_load();
	load_cursor = 0;
	switch (state) {
	case CARD_NONE:
		card_message("No memory card in slot 1.", 0, 0);
		return;
	case CARD_ERROR:
		card_message("The memory card in slot 1", "could not be read.", 0);
		return;
	case CARD_UNFORMATTED:
		if (save) {
			m_state = M_FORMAT;
			return;
		}
		card_message("The memory card in slot 1", "is not formatted.", 0);
		return;
	case CARD_FULL:
		if (save) {
			card_message("The memory card in slot 1 is full:", "Quake needs 1 free block.", 0);
			return;
		}
		card_message("There is no Quake save on the", "memory card in slot 1.", 0);
		return;
	case CARD_NO_FILE:
		if (!save) {
			card_message("There is no Quake save on the", "memory card in slot 1.", 0);
			return;
		}
		break;
	}
	m_state = save ? M_SAVE : M_LOAD;
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
		switch (sp_rows[m_singleplayer_cursor]) {
		case 0:
			m_state = M_GAMEOPTIONS;
			gameoptions_cursor = 0;
			break;
		case 1:
			card_menu(0);
			break;
		default:
			if (can_save())                       /* (M_Menu_Save_f: no game, the
			                                         intermission, a dead player: nothing) */
				card_menu(1);
			break;
		}
		break;
	}
}

static void begin_game(void)
{
	m_state = M_NONE;
	int m = world_find_map(levels[episodes[startepisode].first + startlevel].name);
	if (m >= 0 && hooks.begin_game)
		hooks.begin_game(m, startskill);
}

/* M_NetStart_Change for the single player's rows */
static void gameoptions_change(int dir)
{
	switch (gameoptions_cursor) {
	case 1:
		startskill = wrap(startskill, dir, 4);
		break;
	case 2:
		do
			startepisode = wrap(startepisode, dir, EPISODES);
		while (!episode_on_disc(startepisode));
		startlevel = 0;
		break;
	case 3:
		startlevel = wrap(startlevel, dir, episodes[startepisode].levels);
		break;
	}
}

static void gameoptions_key(int k)
{
	switch (k) {
	case K_ESCAPE:
		m_state = M_SINGLEPLAYER;
		m_entersound = 1;
		break;
	case K_UP:
	case K_DOWN:
		local_sound(S_MENU1);
		gameoptions_cursor = wrap(gameoptions_cursor, k == K_DOWN ? 1 : -1, NUM_GAMEOPTIONS);
		break;
	case K_LEFT:
	case K_RIGHT:
		if (gameoptions_cursor == 0)
			break;
		local_sound(S_MENU3);
		gameoptions_change(k == K_RIGHT ? 1 : -1);
		break;
	case K_ENTER:
		local_sound(S_MENU2);
		if (gameoptions_cursor != 0) {
			gameoptions_change(1);
			break;
		}
		if (hooks.in_game && hooks.in_game())
			m_state = M_NEWGAME;              /* "Are you sure ..." */
		else
			begin_game();
		break;
	}
}

static void newgame_key(int k)
{
	if (k == K_ENTER)
		begin_game();
	else if (k == K_ESCAPE)
		m_state = M_GAMEOPTIONS;
}

static void load_key(int k, int save)
{
	int n = save ? CARD_SLOTS : CARD_SLOTS + 1;
	switch (k) {
	case K_ESCAPE:
		m_state = M_SINGLEPLAYER;
		m_entersound = 1;
		break;
	case K_ENTER:
		local_sound(S_MENU2);
		if (save) {
			m_state = M_NONE;                     /* Host_Savegame_f's messages, the game on */
			if (hooks.save_game)
				hooks.save_game(load_cursor + 1);
			return;
		}
		if (!card_save.slot[load_cursor].map[0])
			return;                               /* (!loadable[load_cursor]) */
		m_state = M_NONE;
		if (hooks.load_game)
			hooks.load_game(load_cursor);
		return;
	case K_UP:
	case K_LEFT:
	case K_DOWN:
	case K_RIGHT:
		local_sound(S_MENU1);
		load_cursor = wrap(load_cursor, k == K_DOWN || k == K_RIGHT ? 1 : -1, n);
		break;
	}
}

static void format_key(int k)
{
	if (k == K_ENTER) {
		if (card_format() != CARD_OK) {
			card_message("The memory card in slot 1", "could not be formatted.", 0);
			return;
		}
		card_menu(1);
	} else if (k == K_ESCAPE) {
		m_state = M_SINGLEPLAYER;
	}
}

/* M_AdjustSliders: the brightness (v_gamma a twentieth a step), the mouse
   speed (sensitivity a half), the volumes (a tenth), the checkboxes */
static void options_adjust(int dir)
{
	local_sound(S_MENU3);
	int *v = 0, lo = 0, hi = 10;
	switch (options_cursor) {
	case O_BRIGHTNESS:  v = &settings.brightness; break;
	case O_MOUSE_SPEED: v = &settings.mouse_speed; lo = 2; hi = 22; break;
	case O_MUSIC:       v = &settings.music; break;
	case O_SOUND:       v = &settings.volume; break;
	case O_INVERT:      settings.invert_y ^= 1; break;
	case O_USE_MOUSE:   settings.mouse ^= 1; break;
	case O_SWAP:        settings.swap_mouse ^= 1; break;
	case O_VIBRATION:   settings.vibration ^= 1; break;
	}
	if (v) {
		*v += dir;
		if (*v < lo) *v = lo;
		if (*v > hi) *v = hi;
	}
	settings_apply();
}

/* the options left: the settings onto the card when they changed and the
   card holds the game's file (read again first: another card's slots are
   not overwritten with these) */
static void options_leave(void)
{
	uint8_t now[CARD_SETTINGS];
	settings_pack(now);
	if (!memcmp(now, options_were, sizeof now))
		return;
	if (card_load() == CARD_OK)
		card_store();
}

static void options_key(int k)
{
	switch (k) {
	case K_ESCAPE:
		m_state = M_MAIN;
		m_entersound = 1;
		options_leave();
		break;
	case K_ENTER:
		m_entersound = 1;
		if (options_cursor == O_RESET) {          /* exec default.cfg */
			settings_defaults();
			settings_apply();
		} else if (options_cursor == O_KEYS) {
			m_state = M_KEYS;
			keys_scheme = settings.mouse && pad.mouse ? KS_MOUSE : pad.analog ? KS_ANALOG : KS_DIGITAL;
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
		if (options_cursor != O_KEYS && options_cursor != O_RESET && options_cursor != O_GRAPHICS)
			options_adjust(k == K_RIGHT ? 1 : -1);
		break;
	}
}

static void keys_key(int k)
{
	if (k == K_ESCAPE || k == K_ENTER) {
		m_state = M_OPTIONS;
		m_entersound = 1;
	} else if (k == K_LEFT || k == K_RIGHT) {
		local_sound(S_MENU1);
		keys_scheme = wrap(keys_scheme, k == K_RIGHT ? 1 : -1, KEY_SCHEMES);
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
   (after half a second, ten a second); the menus take the pad alone (a
   mouse plays only) */
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
	switch (m_state) {
	case M_MAIN:         main_key(k); break;
	case M_SINGLEPLAYER: singleplayer_key(k); break;
	case M_GAMEOPTIONS:  gameoptions_key(k); break;
	case M_NEWGAME:      newgame_key(k); break;
	case M_LOAD:         load_key(k, 0); break;
	case M_SAVE:         load_key(k, 1); break;
	case M_CARD:         if (k == K_ENTER || k == K_ESCAPE) { m_state = M_SINGLEPLAYER; m_entersound = 1; } break;
	case M_FORMAT:       format_key(k); break;
	case M_OPTIONS:      options_key(k); break;
	case M_KEYS:         keys_key(k); break;
	case M_GRAPHICS:     graphics_key(k); break;
	default: break;
	}
}
