/*
 * q1psx — console variables
 *
 * Ported from id-Software/Quake WinQuake/cvar.{c,h} (GPLv2, (C) 1996-1997 Id
 * Software). One change vs stock: `value` is `q_fixed` (20.12), not `float` —
 * consistent with the 20.12 decision, and PSn00bSDK has no atof/strtof anyway.
 *
 * A cvar_t needs only `name` and `string` set (optionally `archive`) before
 * Cvar_RegisterVariable(). After that `value` is the parsed number and cvars
 * can be read directly (`developer.value`) or by name.
 *
 * Deferred: Cvar_WriteVariables (config save — phase 10).
 */

#pragma once

#include "qcommon.h"

typedef struct cvar_s {
	const char     *name;
	char           *string;
	qboolean        archive;   /* saved to config on quit */
	qboolean        server;    /* notifies players when changed */
	q_fixed         value;     /* parsed from string (20.12) */
	struct cvar_s  *next;
} cvar_t;

void      Cvar_RegisterVariable(cvar_t *variable);
void      Cvar_Set(const char *var_name, const char *value);
void      Cvar_SetValue(const char *var_name, q_fixed value);

q_fixed   Cvar_VariableValue(const char *var_name);   /* 0 if undefined */
char     *Cvar_VariableString(const char *var_name);  /* "" if undefined */
char     *Cvar_CompleteVariable(const char *partial);

qboolean  Cvar_Command(void);   /* console print/set; called from Cmd_ExecuteString */

cvar_t   *Cvar_FindVar(const char *var_name);
extern cvar_t *cvar_vars;
