/*
 * qcook — what the release's menu needs before any map is loaded: `qcook
 * menu --pak <pak0.pak> --sounds <menu.snd> --conback <conback.dat>`.
 *
 * The menu's own sound bank (formats/qsound.h, sound.c sound_build_menu):
 * its clicks, and CD track 2, which Quake's first demo forces.
 * The console's background (formats/qmenu.h): gfx/conback.lmp, what Quake
 * draws under its menu with no world and no demo (M_Draw,
 * Draw_ConsoleBackground), with the palette as an opaque CLUT.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qcook.h"
#include "../../formats/qmenu.h"

static void conback_write(const uint8_t *pak, long pak_len, const char *out_path)
{
	uint32_t len, plen;
	const uint8_t *lmp = pak_find(pak, pak_len, "gfx/conback.lmp", &len);
	const uint8_t *pal = pak_find(pak, pak_len, "gfx/palette.lmp", &plen);
	if (!lmp || len < 8 || !pal || plen < 768)
		die("gfx/conback.lmp or gfx/palette.lmp missing");
	uint32_t w = lmp[0] | lmp[1] << 8 | lmp[2] << 16 | (uint32_t) lmp[3] << 24;
	uint32_t h = lmp[4] | lmp[5] << 8 | lmp[6] << 16 | (uint32_t) lmp[7] << 24;
	if (w != 320 || h != 200 || len < 8 + w * h)
		die("gfx/conback.lmp: %ux%u, not Quake's 320x200", w, h);

	qcb_header hd;
	memset(&hd, 0, sizeof hd);
	memcpy(hd.magic, QCB_MAGIC, 4);
	hd.w = (uint16_t) w;
	hd.h = (uint16_t) h;
	hd.ofs_clut   = 64;
	hd.ofs_pixels = 64 + 512;
	uint32_t size = hd.ofs_pixels + w * h;               /* (64 000: whole 64-byte units) */
	uint8_t *f = xcalloc(size, 1);
	memcpy(f, &hd, sizeof hd);
	uint16_t *clut = (uint16_t *) (f + hd.ofs_clut);
	for (int i = 0; i < 256; i++) {
		int r = pal[i * 3], g = pal[i * 3 + 1], b = pal[i * 3 + 2];
		uint16_t c = (uint16_t) ((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
		clut[i] = c ? c : 0x8000;                        /* black, not transparent */
	}
	memcpy(f + hd.ofs_pixels, lmp + 8, w * h);
	int n255 = 0;
	for (uint32_t i = 0; i < w * h; i++)
		n255 += lmp[8 + i] == 255;
	FILE *fp = fopen(out_path, "wb");
	if (!fp || fwrite(f, 1, size, fp) != size)
		die("cannot write %s", out_path);
	fclose(fp);
	free(f);
	printf("qcook: conback %ux%u, %u bytes (%d texels of colour 255, drawn as it is)\n", w, h, size, n255);
}

int cmd_menu(int argc, char **argv)
{
	const char *pak_path = NULL, *snd_path = NULL, *cb_path = NULL;
	for (int i = 2; i < argc - 1; i++) {
		if (!strcmp(argv[i], "--pak"))     pak_path = argv[i + 1];
		if (!strcmp(argv[i], "--sounds"))  snd_path = argv[i + 1];
		if (!strcmp(argv[i], "--conback")) cb_path  = argv[i + 1];
	}
	if (!pak_path || !snd_path || !cb_path)
		die("usage: qcook menu --pak <pak0.pak> --sounds <menu.snd> --conback <conback.dat>");
	long pak_len;
	uint8_t *pak = read_file(pak_path, &pak_len);
	sound_build_menu(pak, pak_len, snd_path);
	conback_write(pak, pak_len, cb_path);
	free(pak);
	return 0;
}
