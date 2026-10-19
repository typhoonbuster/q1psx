/*
 * q1psx — CD-ROM file loading.
 *
 * Files are read whole, in double-speed mode, straight into the level arena.
 * Names are ISO9660 paths as built by iso/iso.xml, e.g. "\\MAPS\\E1M1_G2.WLD;1":
 * a file in the root or in a directory of the root. A lookup reads its
 * directory into the level arena's top (released at once: the arena must
 * have room for it, 24 KB for \MAPS on a disc with every map), never into
 * the SDK's heap (psxcd's CdSearchFile and CdOpenDir: see cd.c).
 */

#ifndef Q1PSX_CD_H
#define Q1PSX_CD_H

#include <stdint.h>

#define CD_SECTOR 2048           /* a data sector's bytes */

void  cd_init(void);

/* Called about once per frame while a read is in flight, with the bytes read
   so far (whole sectors). It may draw and flip a frame (a loading bar). */
typedef void (*cd_poll_fn)(uint32_t bytes_done);

/* Load a whole file into the level arena. Returns NULL if it is missing or
   the read fails; *size receives the exact byte size. With `poll`, the read
   runs in the background and `poll` is called until it is done; NULL
   blocks. */
void *cd_load(const char *name, uint32_t *size, cd_poll_fn poll);
uint32_t cd_last_lba(void);      /* the last file cd_load found: its first sector */

/* Is the file on the disc? (No read of the file; *size may be NULL.) Each
   call reads its directory again (a seek and a read: ~80 ms on the drive). */
int   cd_exists(const char *name, uint32_t *size);

/* The file's first sector (CdIntToPos's numbering) and its size, as
   cd_exists; either pointer may be NULL. */
int   cd_find(const char *name, uint32_t *lba, uint32_t *size);

/* The files of a directory ("\\MAPS"), one call of `fn` each with the
   ISO name ("E1M1_G2.WLD;1") and the size: one directory read for a whole
   listing (world_scan_maps asked cd_exists 128 times: 10 s on the drive).
   Returns the count, 0 if the directory is missing. */
typedef void (*cd_list_fn)(const char *name, uint32_t size);
int   cd_list_dir(const char *path, cd_list_fn fn);

#endif
