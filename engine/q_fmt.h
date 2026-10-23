/*
 * q1psx — snprintf for the screen's text: %d %u %x %s %c and %%, a width,
 * the '-' (to the left) and '0' (zeros) flags; C99's return (the length the
 * whole text takes). libc's vsnprintf it replaces was 4 KB of RAM
 * (docs/porting-notes.md §77).
 */

#ifndef Q1PSX_Q_FMT_H
#define Q1PSX_Q_FMT_H

int q_snprintf(char *buf, int size, const char *fmt, ...);

#endif
