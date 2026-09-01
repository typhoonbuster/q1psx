/*
 * q1psx — Quake for the PlayStation 1
 *
 * Phase 1.5 skeleton: the smallest program that boots on the PS1 and puts a
 * stable image on screen, so the repo's CMake/mkpsxiso build is proven
 * reproducible end to end (ELF -> PS-EXE -> .bin/.cue).
 *
 * Deliberately NOT double-buffered yet — that, plus the flicker/tearing
 * validation, is Phase 2.1. Nothing here is Quake engine code.
 */

#include <psxgpu.h>

#define SCREEN_W 320
#define SCREEN_H 240

static DISPENV disp_env;
static DRAWENV draw_env;

int main(void) {
	ResetGraph(0);

	SetDefDispEnv(&disp_env, 0, 0, SCREEN_W, SCREEN_H);
	SetDefDrawEnv(&draw_env, 0, 0, SCREEN_W, SCREEN_H);

	/* auto-clear the framebuffer to a dark blue every time the draw env is applied */
	setRGB0(&draw_env, 40, 20, 80);
	draw_env.isbg = 1;

	PutDispEnv(&disp_env);
	PutDrawEnv(&draw_env);

	SetDispMask(1);

	for (;;) {
		VSync(0);
	}

	return 0;
}
