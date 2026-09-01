/*
 * q1psx — common definitions and helpers
 *
 * Ported from id-Software/Quake WinQuake/common.{h,c} (GPLv2, (C) 1996-1997 Id
 * Software), trimmed to what a PS1 build needs: types, intrusive linked lists,
 * sizebuf_t, byte-order helpers, the COM_Parse tokenizer and filename utils.
 *
 * Left out for now (added when a phase needs them):
 *   - MSG_* network serialization (no networking)
 *   - the file system / PAK loader (phase 3.3)
 *   - COM_CheckRegistered (needs the file system)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "q_fixed.h"

typedef uint8_t byte;
typedef int     qboolean;   /* Quake code uses bare true/false (stdbool) */

#define MAX_QPATH   64       /* max length of a quake game pathname */

/* ------------------------------------------------------------------ links -- */

typedef struct link_s {
	struct link_s *prev, *next;
} link_t;

void ClearLink(link_t *l);
void RemoveLink(link_t *l);
void InsertLinkBefore(link_t *l, link_t *before);
void InsertLinkAfter(link_t *l, link_t *after);

/* (type *)STRUCT_FROM_LINK(link_t *link, type, member) */
#define STRUCT_FROM_LINK(l, t, m) ((t *)((byte *)(l) - (size_t)&(((t *)0)->m)))

/* --------------------------------------------------------------- sizebuf -- */

typedef struct sizebuf_s {
	qboolean allowoverflow;   /* if false, Sys_Error on overflow */
	qboolean overflowed;      /* set true if a write overflowed  */
	byte    *data;
	int      maxsize;
	int      cursize;
} sizebuf_t;

void  SZ_Alloc(sizebuf_t *buf, int startsize);   /* backed by Hunk_AllocName */
void  SZ_Clear(sizebuf_t *buf);
void *SZ_GetSpace(sizebuf_t *buf, int length);
void  SZ_Write(sizebuf_t *buf, const void *data, int length);
void  SZ_Print(sizebuf_t *buf, const char *data);   /* strcats onto the buf */

/* ------------------------------------------------------------ byte order -- */
/* The PS1 R3000 is little-endian, so the Little* helpers are the identity and
 * the Big* helpers byteswap. Kept as real functions so call sites and any
 * &Little* references port unchanged. */

short LittleShort(short l);
short BigShort(short l);
int   LittleLong(int l);
int   BigLong(int l);
float LittleFloat(float l);
float BigFloat(float l);

/* ------------------------------------------------------- libc shorthands -- */
/* Quake spells these Q_*; map them straight onto libc. */

#define Q_memset(d, f, c)    memset((d), (f), (c))
#define Q_memcpy(d, s, c)    memcpy((d), (s), (c))
#define Q_memcmp(a, b, c)    memcmp((a), (b), (c))
#define Q_strcpy(d, s)       strcpy((d), (s))
#define Q_strncpy(d, s, c)   strncpy((d), (s), (c))
#define Q_strlen(s)          ((int)strlen(s))
#define Q_strrchr(s, c)      strrchr((s), (c))
#define Q_strcat(d, s)       strcat((d), (s))
#define Q_strcmp(a, b)       strcmp((a), (b))
#define Q_strncmp(a, b, c)   strncmp((a), (b), (c))

/* PSn00bSDK's libc has no str*casecmp */
int     Q_strcasecmp(const char *s1, const char *s2);
int     Q_strncasecmp(const char *s1, const char *s2, int n);

int     Q_atoi(const char *str);
q_fixed Q_atofix(const char *str);              /* "-1.5" / "800" / "0x20" -> 20.12 */
void    Q_fixtoa(q_fixed v, char *buf, int cap); /* 20.12 -> "1.5" (trims zeros)     */

/* --------------------------------------------------------------- parsing -- */

extern char     com_token[1024];
extern qboolean com_eof;

char *COM_Parse(char *data);   /* parse one token into com_token */

char *va(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* --------------------------------------------------------- filename utils -- */

char *COM_SkipPath(char *pathname);
void  COM_StripExtension(const char *in, char *out);
void  COM_FileBase(const char *in, char *out);
void  COM_DefaultExtension(char *path, const char *extension);
