/*
 * q1psx — command buffer and execution. Ported from WinQuake cmd.c (core).
 */

#include <stdio.h>
#include <string.h>

#include "sys.h"
#include "zone.h"
#include "qcommon.h"
#include "cvar.h"
#include "cmd.h"

#define MAX_ARGS 80

cmd_source_t cmd_source;

/* ============================================================ Cbuf ======= */

static sizebuf_t cmd_text;

void Cbuf_Init(void)
{
	SZ_Alloc(&cmd_text, 8192);   /* commands + script files */
}

void Cbuf_AddText(const char *text)
{
	int l = (int) strlen(text);

	if (cmd_text.cursize + l >= cmd_text.maxsize) {
		printf("Cbuf_AddText: overflow\n");
		return;
	}
	SZ_Write(&cmd_text, text, l);
}

void Cbuf_InsertText(const char *text)
{
	char *temp    = NULL;
	int   templen = cmd_text.cursize;

	if (templen) {
		temp = Z_Malloc(templen);
		memcpy(temp, cmd_text.data, templen);
		SZ_Clear(&cmd_text);
	}

	Cbuf_AddText(text);

	if (templen) {
		SZ_Write(&cmd_text, temp, templen);
		Z_Free(temp);
	}
}

int Cbuf_Pending(void)
{
	return cmd_text.cursize;
}

void Cbuf_Execute(void)
{
	while (cmd_text.cursize) {
		/* find a \n or ; line break, ignoring ; inside quotes */
		char *text   = (char *) cmd_text.data;
		int   quotes = 0;
		int   i;

		for (i = 0; i < cmd_text.cursize; i++) {
			if (text[i] == '"')
				quotes++;
			if (!(quotes & 1) && text[i] == ';')
				break;
			if (text[i] == '\n')
				break;
		}

		char line[1024];
		if (i > (int) sizeof(line) - 1)
			i = sizeof(line) - 1;
		memcpy(line, text, i);
		line[i] = 0;

		/* remove the line from the buffer before executing (exec/alias may
		   insert text at the front) */
		if (i == cmd_text.cursize) {
			cmd_text.cursize = 0;
		} else {
			i++;
			cmd_text.cursize -= i;
			memmove(text, text + i, cmd_text.cursize);
		}

		Cmd_ExecuteString(line, src_command);
	}
}

/* ============================================================ Cmd ======== */

typedef struct cmd_function_s {
	struct cmd_function_s *next;
	const char           *name;
	xcommand_t            function;
} cmd_function_t;

static cmd_function_t *cmd_functions;

static int   cmd_argc;
static char *cmd_argv[MAX_ARGS];
static char *cmd_args;
static char  cmd_null_string[] = "";

void Cmd_Init(void)
{
	/* stuffcmds / exec / echo / alias / cmd / wait land here once their
	   dependencies (argv, file system, aliases, network) are ported */
}

/* Forget every command and clear the text buffer. The hunk memory itself is
   only reclaimed by a mark reset — this just drops the dangling pointers so a
   self-test can roll the hunk back safely. */
void Cmd_Shutdown(void)
{
	for (int i = 0; i < cmd_argc; i++)
		Z_Free(cmd_argv[i]);
	cmd_argc      = 0;
	cmd_args      = NULL;
	cmd_functions = NULL;
	memset(&cmd_text, 0, sizeof cmd_text);
}

int Cmd_Argc(void)
{
	return cmd_argc;
}

char *Cmd_Argv(int arg)
{
	if ((unsigned) arg >= (unsigned) cmd_argc)
		return cmd_null_string;
	return cmd_argv[arg];
}

char *Cmd_Args(void)
{
	return cmd_args ? cmd_args : cmd_null_string;
}

void Cmd_TokenizeString(const char *text)
{
	for (int i = 0; i < cmd_argc; i++)
		Z_Free(cmd_argv[i]);
	cmd_argc = 0;
	cmd_args = NULL;

	for (;;) {
		while (*text && *text <= ' ' && *text != '\n')
			text++;

		if (*text == '\n') {
			text++;
			break;
		}
		if (!*text)
			return;

		if (cmd_argc == 1)
			cmd_args = (char *) text;

		text = COM_Parse((char *) text);
		if (!text)
			return;

		if (cmd_argc < MAX_ARGS) {
			cmd_argv[cmd_argc] = Z_Malloc((int) strlen(com_token) + 1);
			strcpy(cmd_argv[cmd_argc], com_token);
			cmd_argc++;
		}
	}
}

void Cmd_AddCommand(const char *cmd_name, xcommand_t function)
{
	if (Cvar_VariableString(cmd_name)[0]) {
		printf("Cmd_AddCommand: %s already defined as a var\n", cmd_name);
		return;
	}
	for (cmd_function_t *cmd = cmd_functions; cmd; cmd = cmd->next) {
		if (!strcmp(cmd_name, cmd->name)) {
			printf("Cmd_AddCommand: %s already defined\n", cmd_name);
			return;
		}
	}

	cmd_function_t *cmd = Hunk_Alloc(sizeof(cmd_function_t));
	cmd->name     = cmd_name;
	cmd->function = function;
	cmd->next     = cmd_functions;
	cmd_functions = cmd;
}

qboolean Cmd_Exists(const char *cmd_name)
{
	for (cmd_function_t *cmd = cmd_functions; cmd; cmd = cmd->next)
		if (!strcmp(cmd_name, cmd->name))
			return true;
	return false;
}

char *Cmd_CompleteCommand(const char *partial)
{
	int len = (int) strlen(partial);
	if (!len)
		return NULL;

	for (cmd_function_t *cmd = cmd_functions; cmd; cmd = cmd->next)
		if (!strncmp(partial, cmd->name, len))
			return (char *) cmd->name;
	return NULL;
}

void Cmd_ExecuteString(const char *text, cmd_source_t src)
{
	cmd_source = src;
	Cmd_TokenizeString(text);

	if (!cmd_argc)
		return;

	for (cmd_function_t *cmd = cmd_functions; cmd; cmd = cmd->next) {
		if (!Q_strcasecmp(cmd_argv[0], cmd->name)) {
			cmd->function();
			return;
		}
	}

	if (!Cvar_Command())
		printf("Unknown command \"%s\"\n", Cmd_Argv(0));
}
