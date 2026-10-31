/*
 * q1psx — 2D drawing over the frame: the game data's aux pictures (the
 * intermission's, the menu's: formats/qgame.h QGA_*) and text in Quake's
 * console font (conchars: Draw_Character, chars 128-255 the bronze set).
 * Everything goes to the overlay chain (vid.h), drawn in the order added.
 */

#ifndef Q1PSX_UI_H
#define Q1PSX_UI_H

/* before a run of ui_* calls in a frame (another module may have set the
   GPU's texture page since) */
void ui_begin(void);

/* an aux picture at (x, y); `shade` 128 as it is, less to darken (a greyed
   menu entry) */
void ui_aux(int id, int x, int y);
void ui_aux_shaded(int id, int x, int y, int shade);

/* a part of an aux picture: its rows sy .. sy + h at (x, y) */
void ui_aux_rows(int id, int x, int y, int sy, int h, int shade);

/* Draw_Character, a string of them; `add` 128 the bronze set (M_Print);
   `shade` as above */
void ui_char(int x, int y, int c, int shade);
void ui_chars(int x, int y, const char *s, int n, int add, int shade);   /* n of s, a run */
void ui_text(int x, int y, const char *s);
void ui_text_shaded(int x, int y, const char *s, int add, int shade);

/* Draw_FadeScreen: the screen darkened */
void ui_fade(void);

/* The console's background (formats/qmenu.h), what the release shows
   before any map is loaded (Quake's forced console: no world, no demo).
   ui_conback_load reads it into VRAM where the map's texture pages go
   (through the level arena: before a map's load), 1 done; ui_conback draws
   it over the whole screen (Draw_ConsoleBackground (vid.height)) while it
   is there; ui_conback_forget: a map's textures took its place. */
int  ui_conback_load(void);
void ui_conback(void);
void ui_conback_forget(void);

#endif
