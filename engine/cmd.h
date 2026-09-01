/*
 * q1psx — command buffer and command execution
 *
 * Ported from id-Software/Quake WinQuake/cmd.{c,h} (GPLv2, (C) 1996-1997 Id
 * Software). Core only for now — deferred: aliases, `exec` (needs the file
 * system), `stuffcmds` (needs argv), `wait`, `cmd`/ForwardToServer (network).
 */

#pragma once

#include "qcommon.h"

/* --- command text buffer --- */
void Cbuf_Init(void);                 /* allocates the text buffer on the hunk */
void Cbuf_AddText(const char *text);   /* append to the end   */
void Cbuf_InsertText(const char *text);/* prepend before pending commands */
void Cbuf_Execute(void);               /* run \n / ; separated lines */
int  Cbuf_Pending(void);               /* bytes still queued */

/* --- command dispatch --- */
typedef void (*xcommand_t)(void);

typedef enum {
	src_client,    /* from a network connection */
	src_command    /* from the command buffer   */
} cmd_source_t;

extern cmd_source_t cmd_source;

void      Cmd_Init(void);
void      Cmd_Shutdown(void);   /* drop all commands + buffer (used by selftest) */
void      Cmd_AddCommand(const char *cmd_name, xcommand_t function);
qboolean  Cmd_Exists(const char *cmd_name);
char     *Cmd_CompleteCommand(const char *partial);

int       Cmd_Argc(void);
char     *Cmd_Argv(int arg);           /* "" (never NULL) if out of range */
char     *Cmd_Args(void);

void      Cmd_TokenizeString(const char *text);
void      Cmd_ExecuteString(const char *text, cmd_source_t src);
