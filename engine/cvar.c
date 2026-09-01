/*
 * q1psx — console variables. Ported from WinQuake cvar.c.
 */

#include <stdio.h>
#include <string.h>

#include "sys.h"
#include "zone.h"
#include "qcommon.h"
#include "cmd.h"
#include "cvar.h"

cvar_t *cvar_vars;
static char *cvar_null_string = "";

cvar_t *Cvar_FindVar(const char *var_name)
{
	for (cvar_t *var = cvar_vars; var; var = var->next)
		if (!strcmp(var_name, var->name))
			return var;
	return NULL;
}

q_fixed Cvar_VariableValue(const char *var_name)
{
	cvar_t *var = Cvar_FindVar(var_name);
	return var ? var->value : 0;
}

char *Cvar_VariableString(const char *var_name)
{
	cvar_t *var = Cvar_FindVar(var_name);
	return var ? var->string : cvar_null_string;
}

char *Cvar_CompleteVariable(const char *partial)
{
	int len = (int) strlen(partial);
	if (!len)
		return NULL;

	for (cvar_t *var = cvar_vars; var; var = var->next)
		if (!strncmp(partial, var->name, len))
			return (char *) var->name;
	return NULL;
}

void Cvar_Set(const char *var_name, const char *value)
{
	cvar_t *var = Cvar_FindVar(var_name);
	if (!var) {
		/* a bug in C code if this happens */
		printf("Cvar_Set: variable %s not found\n", var_name);
		return;
	}

	Z_Free(var->string);
	var->string = Z_Malloc((int) strlen(value) + 1);
	strcpy(var->string, value);
	var->value = Q_atofix(var->string);

	/* var->server broadcast is wired up with the server (phase 7+) */
}

void Cvar_SetValue(const char *var_name, q_fixed value)
{
	char val[32];
	Q_fixtoa(value, val, sizeof val);
	Cvar_Set(var_name, val);
}

void Cvar_RegisterVariable(cvar_t *variable)
{
	if (Cvar_FindVar(variable->name)) {
		printf("Can't register variable %s, already defined\n", variable->name);
		return;
	}
	if (Cmd_Exists(variable->name)) {
		printf("Cvar_RegisterVariable: %s is a command\n", variable->name);
		return;
	}

	/* copy the value off — future sets Z_Free it */
	const char *oldstr = variable->string;
	variable->string = Z_Malloc((int) strlen(oldstr) + 1);
	strcpy(variable->string, oldstr);
	variable->value = Q_atofix(variable->string);

	variable->next = cvar_vars;
	cvar_vars = variable;
}

qboolean Cvar_Command(void)
{
	cvar_t *v = Cvar_FindVar(Cmd_Argv(0));
	if (!v)
		return false;

	if (Cmd_Argc() == 1) {
		printf("\"%s\" is \"%s\"\n", v->name, v->string);
		return true;
	}

	Cvar_Set(v->name, Cmd_Argv(1));
	return true;
}
