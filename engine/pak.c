/*
 * q1psx — PAK archive reader. See pak.h.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sys.h"
#include "zone.h"
#include "qcommon.h"
#include "cd.h"
#include "pak.h"

#define CD_SECTOR       2048
#define MAX_FILES_IN_PAK 4096   /* Quake's limit is 2048; be generous */

typedef struct __attribute__((packed)) {
	char    id[4];      /* "PACK" */
	int32_t dirofs;
	int32_t dirlen;
} pak_header_t;

int pak_open(const char *iso_name, pak_t *pak)
{
	if (!cd_find(iso_name, &pak->cdfile)) {
		printf("pak_open: %s not found on disc\n", iso_name);
		return 0;
	}

	pak_header_t header;
	if (cd_read_at(&pak->cdfile, 0, &header, sizeof header) < 0) {
		printf("pak_open: %s header read failed\n", iso_name);
		return 0;
	}

	if (memcmp(header.id, "PACK", 4) != 0) {
		printf("pak_open: %s is not a PACK file\n", iso_name);
		return 0;
	}

	int dirofs = LittleLong(header.dirofs);
	int dirlen = LittleLong(header.dirlen);
	int n      = dirlen / (int) sizeof(pak_entry_t);

	if (n <= 0 || n > MAX_FILES_IN_PAK) {
		printf("pak_open: %s has a bad directory (%d entries)\n", iso_name, n);
		return 0;
	}

	pak->numfiles = n;
	pak->files    = Hunk_AllocName(n * (int) sizeof(pak_entry_t), "pakdir");

	if (cd_read_at(&pak->cdfile, dirofs, pak->files, n * (int) sizeof(pak_entry_t)) < 0) {
		printf("pak_open: %s directory read failed\n", iso_name);
		return 0;
	}

	for (int i = 0; i < n; i++) {
		pak->files[i].filepos = LittleLong(pak->files[i].filepos);
		pak->files[i].filelen = LittleLong(pak->files[i].filelen);
	}

	printf("pak_open: %s — %d files, dir at %d (%d bytes)\n",
	       iso_name, n, dirofs, dirlen);
	return 1;
}

int pak_find(const pak_t *pak, const char *name)
{
	for (int i = 0; i < pak->numfiles; i++)
		if (!Q_strcasecmp(pak->files[i].name, name))
			return i;
	return -1;
}

int pak_read_lump(const pak_t *pak, int index, void *buf, int cap)
{
	if (index < 0 || index >= pak->numfiles)
		return -1;

	const pak_entry_t *e = &pak->files[index];
	if (e->filelen > cap)
		return -1;

	if (cd_read_at(&pak->cdfile, e->filepos, buf, e->filelen) < 0)
		return -1;

	return e->filelen;
}

void pak_list(const pak_t *pak)
{
	int total = 0;

	printf("--- %d entries in pak ---\n", pak->numfiles);
	for (int i = 0; i < pak->numfiles; i++) {
		printf("  %-52.52s %8d\n", pak->files[i].name, pak->files[i].filelen);
		total += pak->files[i].filelen;
	}
	printf("--- %d entries, %d bytes total ---\n", pak->numfiles, total);
}
