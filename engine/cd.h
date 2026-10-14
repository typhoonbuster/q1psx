/*
 * q1psx — CD-ROM file loading.
 *
 * Files are read whole, in double-speed mode, straight into the level arena.
 * Names are ISO9660 paths as built by iso/iso.xml, e.g. "\\START.WLD;1".
 */

#ifndef Q1PSX_CD_H
#define Q1PSX_CD_H

#include <stdint.h>

void  cd_init(void);

/* Load a whole file into the level arena. Returns NULL if it is missing or
   the read fails; *size receives the exact byte size. */
void *cd_load(const char *name, uint32_t *size);

#endif
