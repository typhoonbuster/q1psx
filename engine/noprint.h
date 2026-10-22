/*
 * q1psx — the release builds print nothing: CMake forces this header into
 * every C file of a release (not a bench) build. A console has no TTY to
 * read: printf's calls and their format strings leave the code (the
 * screen's text, snprintf, stays). The arguments are not evaluated: a call
 * with a side effect is made before the printf (engine/pad.c pad_init).
 * docs/porting-notes.md §60.
 */

#ifndef Q1PSX_NOPRINT_H
#define Q1PSX_NOPRINT_H

#include <stdio.h>      /* (its declaration first, the macro after) */

#define printf(...) ((void) 0)

#endif
