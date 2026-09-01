/*
 * q1psx — PAK archive reader
 *
 * Quake ships all its data in `pak0.pak` (shareware) / `pak1.pak` (retail): a
 * flat "PACK" header + a directory of 64-byte entries, all little-endian. The
 * archive itself stays on the disc; individual lumps are streamed out on demand
 * via cd_read_at() (the PS1 has nowhere near enough RAM to hold a whole pak).
 *
 * Format from id-Software/Quake WinQuake/common.c:1225.
 *
 * Phase 3.3: open a pak, cache its directory, list it over serial.
 */

#pragma once

#include <stdint.h>

#include "cd.h"

/* on-disk directory entry, 64 bytes, little-endian — also the runtime form */
typedef struct __attribute__((packed)) {
	char    name[56];
	int32_t filepos;
	int32_t filelen;
} pak_entry_t;

typedef struct {
	cd_file_t    cdfile;     /* the pak file on the CD */
	int          numfiles;
	pak_entry_t *files;      /* directory, hunk-allocated */
} pak_t;

/* Open the pak at ISO path `iso_name` (e.g. "\\PAK0.PAK;1"); reads the header
   and caches the directory on the low hunk. Returns 1 on success. */
int pak_open(const char *iso_name, pak_t *pak);

/* Directory index of `name` (e.g. "progs.dat", "maps/e1m1.bsp"), or -1. */
int pak_find(const pak_t *pak, const char *name);

/* Stream lump `index` into `buf` (must hold at least filelen bytes rounded up
   to a sector). Returns the lump size, or -1. */
int pak_read_lump(const pak_t *pak, int index, void *buf, int cap);

/* Print every entry (name + size) to serial — the phase 3.3 validation. */
void pak_list(const pak_t *pak);
