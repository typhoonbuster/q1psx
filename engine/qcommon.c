/*
 * q1psx — common helpers
 *
 * Ported from id-Software/Quake WinQuake/common.c (GPLv2, (C) 1996-1997 Id
 * Software). See qcommon.h for what is included and what is deferred.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "sys.h"
#include "zone.h"
#include "qcommon.h"

/* ================================================================= links == */

void ClearLink(link_t *l)
{
	l->prev = l->next = l;
}

void RemoveLink(link_t *l)
{
	l->next->prev = l->prev;
	l->prev->next = l->next;
}

void InsertLinkBefore(link_t *l, link_t *before)
{
	l->next = before;
	l->prev = before->prev;
	l->prev->next = l;
	l->next->prev = l;
}

void InsertLinkAfter(link_t *l, link_t *after)
{
	l->next = after->next;
	l->prev = after;
	l->prev->next = l;
	l->next->prev = l;
}

/* ============================================================== byte order == */
/* PS1 is little-endian: Little* is identity, Big* swaps. */

static short ShortSwap(short l)
{
	byte b1 = l & 255, b2 = (l >> 8) & 255;
	return (b1 << 8) + b2;
}

static int LongSwap(int l)
{
	byte b1 = l & 255, b2 = (l >> 8) & 255, b3 = (l >> 16) & 255, b4 = (l >> 24) & 255;
	return ((int) b1 << 24) + ((int) b2 << 16) + ((int) b3 << 8) + b4;
}

static float FloatSwap(float f)
{
	union { float f; byte b[4]; } in, out;
	in.f = f;
	out.b[0] = in.b[3];
	out.b[1] = in.b[2];
	out.b[2] = in.b[1];
	out.b[3] = in.b[0];
	return out.f;
}

short LittleShort(short l) { return l; }
short BigShort(short l)    { return ShortSwap(l); }
int   LittleLong(int l)    { return l; }
int   BigLong(int l)       { return LongSwap(l); }
float LittleFloat(float l) { return l; }
float BigFloat(float l)    { return FloatSwap(l); }

/* ================================================================ Q_atoi == */

int Q_atoi(const char *str)
{
	int val = 0, sign = 1, c;

	if (*str == '-') {
		sign = -1;
		str++;
	}

	if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
		str += 2;
		for (;;) {
			c = *str++;
			if (c >= '0' && c <= '9')
				val = (val << 4) + c - '0';
			else if (c >= 'a' && c <= 'f')
				val = (val << 4) + c - 'a' + 10;
			else if (c >= 'A' && c <= 'F')
				val = (val << 4) + c - 'A' + 10;
			else
				return val * sign;
		}
	}

	if (str[0] == '\'')
		return sign * str[1];

	for (;;) {
		c = *str++;
		if (c < '0' || c > '9')
			return val * sign;
		val = val * 10 + c - '0';
	}
}

/* ================================================================ sizebuf == */

void SZ_Alloc(sizebuf_t *buf, int startsize)
{
	if (startsize < 256)
		startsize = 256;
	buf->data    = Hunk_AllocName(startsize, "sizebuf");
	buf->maxsize = startsize;
	buf->cursize = 0;
}

void SZ_Clear(sizebuf_t *buf)
{
	buf->cursize = 0;
}

void *SZ_GetSpace(sizebuf_t *buf, int length)
{
	void *data;

	if (buf->cursize + length > buf->maxsize) {
		if (!buf->allowoverflow)
			Sys_Error("SZ_GetSpace: overflow without allowoverflow set");
		if (length > buf->maxsize)
			Sys_Error("SZ_GetSpace: %i is > full buffer size", length);
		buf->overflowed = true;
		printf("SZ_GetSpace: overflow\n");
		SZ_Clear(buf);
	}

	data = buf->data + buf->cursize;
	buf->cursize += length;
	return data;
}

void SZ_Write(sizebuf_t *buf, const void *data, int length)
{
	memcpy(SZ_GetSpace(buf, length), data, length);
}

void SZ_Print(sizebuf_t *buf, const char *data)
{
	int len = (int) strlen(data) + 1;

	if (buf->cursize && buf->data[buf->cursize - 1])
		memcpy(SZ_GetSpace(buf, len), data, len);          /* no trailing 0 */
	else
		memcpy((byte *) SZ_GetSpace(buf, len - 1) - 1, data, len); /* over trailing 0 */
}

/* ================================================================= parse == */

char     com_token[1024];
qboolean com_eof;

char *COM_Parse(char *data)
{
	int c;
	int len = 0;

	com_token[0] = 0;

	if (!data)
		return NULL;

skipwhite:
	while ((c = *data) <= ' ') {
		if (c == 0)
			return NULL;              /* end of file */
		data++;
	}

	if (c == '/' && data[1] == '/') { /* // comment */
		while (*data && *data != '\n')
			data++;
		goto skipwhite;
	}

	if (c == '\"') {                   /* quoted string */
		data++;
		for (;;) {
			c = *data++;
			if (c == '\"' || !c) {
				com_token[len] = 0;
				return data;
			}
			com_token[len++] = c;
		}
	}

	if (c == '{' || c == '}' || c == ')' || c == '(' || c == '\'' || c == ':') {
		com_token[len++] = c;
		com_token[len] = 0;
		return data + 1;
	}

	do {                              /* regular word */
		com_token[len++] = c;
		data++;
		c = *data;
		if (c == '{' || c == '}' || c == ')' || c == '(' || c == '\'' || c == ':')
			break;
	} while (c > 32);

	com_token[len] = 0;
	return data;
}

char *va(const char *format, ...)
{
	static char string[1024];
	va_list     argptr;

	va_start(argptr, format);
	vsnprintf(string, sizeof(string), format, argptr);
	va_end(argptr);

	return string;
}

/* ======================================================== filename utils == */

char *COM_SkipPath(char *pathname)
{
	char *last = pathname;

	while (*pathname) {
		if (*pathname == '/')
			last = pathname + 1;
		pathname++;
	}
	return last;
}

void COM_StripExtension(const char *in, char *out)
{
	while (*in && *in != '.')
		*out++ = *in++;
	*out = 0;
}

void COM_FileBase(const char *in, char *out)
{
	const char *s, *s2;

	s = in + strlen(in) - 1;
	while (s != in && *s != '.')
		s--;

	for (s2 = s; s2 != in && *s2 != '/'; s2--)
		;

	if (s - s2 < 2)
		strcpy(out, "?model?");
	else {
		s--;
		strncpy(out, s2 + 1, s - s2);
		out[s - s2] = 0;
	}
}

void COM_DefaultExtension(char *path, const char *extension)
{
	char *src = path + strlen(path) - 1;

	while (*src != '/' && src != path) {
		if (*src == '.')
			return;                  /* it has an extension */
		src--;
	}

	strcat(path, extension);
}
