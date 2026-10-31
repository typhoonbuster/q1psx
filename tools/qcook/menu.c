/*
 * qcook — what the release's menu needs before any map is loaded: `qcook
 * menu --pak <pak0.pak> --sounds <menu.snd> --conback <conback.dat>`.
 *
 * The menu's own sound bank (formats/qsound.h, sound.c sound_build_menu):
 * its clicks, and CD track 2, which Quake's first demo forces.
 * The console's background (formats/qmenu.h): gfx/conback.lmp, what Quake
 * draws under its menu with no world and no demo (M_Draw,
 * Draw_ConsoleBackground), with the palette as an opaque CLUT and the
 * version number in its corner; the boot's loading plaque and bar glyphs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "qcook.h"
#include "../../formats/qmenu.h"

/* the console font (gfx.wad's conchars, 128 x 128, 0 transparent) */
static const uint8_t *conchars(const uint8_t *pak, long pak_len)
{
	uint32_t wlen;
	const uint8_t *w = pak_find(pak, pak_len, "gfx.wad", &wlen);
	if (!w || wlen < 12 || memcmp(w, "WAD2", 4))
		die("gfx.wad missing");
	uint32_t n = w[4] | w[5] << 8 | w[6] << 16 | (uint32_t) w[7] << 24;
	uint32_t ofs = w[8] | w[9] << 8 | w[10] << 16 | (uint32_t) w[11] << 24;
	for (uint32_t i = 0; i < n; i++) {
		const uint8_t *e = w + ofs + i * 32;
		if ((size_t) (e + 32 - w) > wlen)
			break;
		uint32_t pos = e[0] | e[1] << 8 | e[2] << 16 | (uint32_t) e[3] << 24;
		if (!strncasecmp((const char *) e + 16, "conchars", 16) && pos + 128 * 128 <= wlen)
			return w + pos;
	}
	die("gfx.wad: conchars missing");
}

/* a picture lump's size and texels */
static const uint8_t *lmp_pic(const uint8_t *pak, long pak_len, const char *name, uint32_t *w, uint32_t *h)
{
	uint32_t len;
	const uint8_t *lmp = pak_find(pak, pak_len, name, &len);
	if (!lmp || len < 8)
		die("%s missing", name);
	*w = lmp[0] | lmp[1] << 8 | lmp[2] << 16 | (uint32_t) lmp[3] << 24;
	*h = lmp[4] | lmp[5] << 8 | lmp[6] << 16 | (uint32_t) lmp[7] << 24;
	if (len < 8 + *w * *h)
		die("%s: a bad picture", name);
	return lmp + 8;
}

/* the palette as PS1 colours: index 0 transparent or black */
static void clut_write(uint16_t *clut, const uint8_t *pal, int zero_transparent)
{
	for (int i = 0; i < 256; i++) {
		int r = pal[i * 3], g = pal[i * 3 + 1], b = pal[i * 3 + 2];
		uint16_t c = (uint16_t) ((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
		clut[i] = c ? c : 0x8000;                        /* black, not transparent */
	}
	if (zero_transparent)
		clut[0] = 0;
}

static void conback_write(const uint8_t *pak, long pak_len, const char *out_path)
{
	uint32_t plen, w, h, lw, lh;
	const uint8_t *pix = lmp_pic(pak, pak_len, "gfx/conback.lmp", &w, &h);
	const uint8_t *pal = pak_find(pak, pak_len, "gfx/palette.lmp", &plen);
	if (!pal || plen < 768)
		die("gfx/palette.lmp missing");
	if (w != 320 || h != 200)
		die("gfx/conback.lmp: %ux%u, not Quake's 320x200", w, h);
	const uint8_t *plaque = lmp_pic(pak, pak_len, "gfx/loading.lmp", &lw, &lh);
	if (lw > QCB_GLYPHS_U || lh > QCB_BOOT_H)
		die("gfx/loading.lmp: %ux%u, larger than the boot strip's room", lw, lh);
	const uint8_t *font = conchars(pak, pak_len);

	qcb_header hd;
	memset(&hd, 0, sizeof hd);
	memcpy(hd.magic, QCB_MAGIC, 4);
	hd.w = (uint16_t) w;
	hd.h = (uint16_t) h;
	hd.ofs_clut       = 64;
	hd.ofs_glyph_clut = 64 + 512;
	hd.ofs_pixels     = 64 + 1024;
	hd.ofs_boot       = hd.ofs_pixels + w * h;           /* (64 000: whole 64-byte units) */
	uint32_t size = hd.ofs_boot + QCB_BOOT_W * QCB_BOOT_H;   /* (4608: whole units too) */
	uint8_t *f = xcalloc(size, 1);
	memcpy(f, &hd, sizeof hd);
	clut_write((uint16_t *) (f + hd.ofs_clut), pal, 0);
	clut_write((uint16_t *) (f + hd.ofs_glyph_clut), pal, 1);

	/* the picture, the version number written in as Draw_ConsoleBackground
	   does: from row 186, ending 11 texels from the right edge, each font
	   texel that is not 0 written + 0x60 (Draw_CharToConback) */
	uint8_t *dst = f + hd.ofs_pixels;
	memcpy(dst, pix, w * h);
	const char *ver = QCB_VERSION;
	int vx = (int) w - 11 - 8 * (int) strlen(ver);
	for (int i = 0; ver[i]; i++) {
		int c = (uint8_t) ver[i];
		const uint8_t *src = font + (c >> 4) * 1024 + (c & 15) * 8;
		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 8; x++)
				if (src[y * 128 + x])
					dst[(186 + y) * w + vx + i * 8 + x] = (uint8_t) (0x60 + src[y * 128 + x]);
	}

	/* the boot strip: the plaque, then the bar's six glyphs */
	uint8_t *boot = f + hd.ofs_boot;
	for (uint32_t y = 0; y < lh; y++)
		memcpy(boot + y * QCB_BOOT_W, plaque + y * lw, lw);
	static const uint8_t glyph[6] = { 128, 129, 130, 29, 30, 31 };
	for (int g = 0; g < 6; g++) {
		const uint8_t *src = font + (glyph[g] >> 4) * 1024 + (glyph[g] & 15) * 8;
		for (int y = 0; y < 8; y++)
			memcpy(boot + y * QCB_BOOT_W + QCB_GLYPHS_U + g * 8, src + y * 128, 8);
	}

	int n255 = 0;
	for (uint32_t i = 0; i < w * h; i++)
		n255 += pix[i] == 255;
	FILE *fp = fopen(out_path, "wb");
	if (!fp || fwrite(f, 1, size, fp) != size)
		die("cannot write %s", out_path);
	fclose(fp);
	free(f);
	printf("qcook: conback %ux%u, \"%s\" in its corner, the boot strip %dx%d, %u bytes (%d texels of colour "
	       "255, drawn as they are)\n", w, h, ver, QCB_BOOT_W, QCB_BOOT_H, size, n255);
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
