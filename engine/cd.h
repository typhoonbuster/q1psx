/*
 * q1psx — CD-ROM access (phase 2.3)
 *
 * Minimal wrapper over psxcd: bring up the drive, then load a whole file into a
 * caller-provided buffer. This is enough to validate the read path; the real
 * PAK streaming layer comes later (phase 3.3 / the CD layout work).
 */

#pragma once

#include <stdint.h>

void cd_init(void);

/*
 * Load the file at ISO path `name` (absolute, e.g. "\\HELLO.TXT;1"; the ";1"
 * and leading slash are optional) into `buf`. `buf` must be 4-byte aligned and
 * at least the file size rounded up to a 2048-byte sector.
 *
 * Returns the file size in bytes, or -1 if the file was not found / the buffer
 * is too small / the read failed.
 */
int cd_load(const char *name, void *buf, int buf_cap);
