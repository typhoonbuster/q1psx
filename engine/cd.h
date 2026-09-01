/*
 * q1psx — CD-ROM access
 *
 * Phase 2.3: bring up the drive, load a whole file (`cd_load`).
 * Phase 3.3: locate a file and read an arbitrary byte range out of it
 *            (`cd_find` + `cd_read_at`) — the primitive the PAK layer streams
 *            individual lumps through.
 */

#pragma once

#include <stdint.h>
#include <psxcd.h>

typedef CdlFILE cd_file_t;   /* { CdlLOC pos; int size; char name[16]; } */

void cd_init(void);

/* Locate `name` (ISO path, e.g. "\\PAK0.PAK;1"). Returns 1 on success. */
int cd_find(const char *name, cd_file_t *out);

/*
 * Read `len` bytes starting `offset` bytes into `file` (offset need not be
 * sector-aligned). Uses the high hunk transiently for a sector-aligned bounce
 * read, so Memory_Init must have run. Returns bytes read, or -1.
 */
int cd_read_at(const cd_file_t *file, int offset, void *buf, int len);

/*
 * Load an entire file into buf (>= size rounded up to 2048, 4-byte aligned).
 * Returns the file size, or -1.
 */
int cd_load(const char *name, void *buf, int buf_cap);
