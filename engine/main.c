/*
 * q1psx — R0 placeholder.
 *
 * The engine is built with performance as constraint #1, see
 * docs/architecture.md. This file only keeps the build, the CD image and
 * the headless boot check green until R1.1 (boot + double buffer + packet
 * arenas) replaces it.
 */

#include <stdio.h>
#include <psxgpu.h>

#define SCREEN_W 320
#define SCREEN_H 240

int main(void)
{
	DISPENV disp[2];
	DRAWENV draw[2];

	ResetGraph(0);
	for (int i = 0; i < 2; i++) {
		SetDefDispEnv(&disp[i], 0, i ? 0 : SCREEN_H, SCREEN_W, SCREEN_H);
		SetDefDrawEnv(&draw[i], 0, i ? SCREEN_H : 0, SCREEN_W, SCREEN_H);
		setRGB0(&draw[i], 24, 24, 32);
		draw[i].isbg = 1;
	}

	printf("q1psx: R0 skeleton\n");
	SetDispMask(1);

	for (int buf = 0;; buf ^= 1) {
		DrawSync(0);
		VSync(0);
		PutDispEnv(&disp[buf]);
		PutDrawEnv(&draw[buf]);
	}
}
