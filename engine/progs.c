/*
 * q1psx — progs.dat loader. Ported from WinQuake pr_edict.c (PR_LoadProgs).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sys.h"
#include "zone.h"
#include "qcommon.h"
#include "crc.h"
#include "pak.h"
#include "progs.h"

dprograms_t   *progs;
dfunction_t   *pr_functions;
char          *pr_strings;
ddef_t        *pr_globaldefs;
ddef_t        *pr_fielddefs;
dstatement_t  *pr_statements;
globalvars_t  *pr_global_struct;
vec_t         *pr_globals;

int            pr_edict_size;
unsigned short pr_crc;

int PR_FindFunction(const char *name)
{
	for (int i = 1; i < progs->numfunctions; i++)
		if (!strcmp(pr_strings + pr_functions[i].s_name, name))
			return i;
	return -1;
}

void PR_LoadProgs(const pak_t *pak)
{
	int idx = pak_find(pak, "progs.dat");
	if (idx < 0)
		Sys_Error("PR_LoadProgs: progs.dat not in pak");

	int size   = pak->files[idx].filelen;
	int padded = (size + 2047) & ~2047;

	progs = Hunk_AllocName(padded, "progs");
	if (pak_read_lump(pak, idx, progs, padded) < 0)
		Sys_Error("PR_LoadProgs: read failed");

	/* whole-file CRC (used later for the client/server progs match) */
	pr_crc = CRC_Block((const byte *) progs, size);

	/* byte-swap the header */
	for (int i = 0; i < (int) sizeof(*progs) / 4; i++)
		((int *) progs)[i] = LittleLong(((int *) progs)[i]);

	if (progs->version != PROG_VERSION)
		Sys_Error("progs.dat has wrong version (%d, want %d)",
		          progs->version, PROG_VERSION);
	if (progs->crc != PROGHEADER_CRC)
		Sys_Error("progs.dat progdefs out of date (crc %d, want %d)",
		          progs->crc, PROGHEADER_CRC);

	pr_functions     = (dfunction_t *) ((byte *) progs + progs->ofs_functions);
	pr_strings       = (char *) progs + progs->ofs_strings;
	pr_globaldefs    = (ddef_t *) ((byte *) progs + progs->ofs_globaldefs);
	pr_fielddefs     = (ddef_t *) ((byte *) progs + progs->ofs_fielddefs);
	pr_statements    = (dstatement_t *) ((byte *) progs + progs->ofs_statements);
	pr_global_struct = (globalvars_t *) ((byte *) progs + progs->ofs_globals);
	pr_globals       = (vec_t *) pr_global_struct;

	pr_edict_size = progs->entityfields * 4 + (int) sizeof(edict_t) - (int) sizeof(entvars_t);

	/* byte-swap the lumps */
	for (int i = 0; i < progs->numstatements; i++) {
		pr_statements[i].op = LittleShort(pr_statements[i].op);
		pr_statements[i].a  = LittleShort(pr_statements[i].a);
		pr_statements[i].b  = LittleShort(pr_statements[i].b);
		pr_statements[i].c  = LittleShort(pr_statements[i].c);
	}

	for (int i = 0; i < progs->numfunctions; i++) {
		pr_functions[i].first_statement = LittleLong(pr_functions[i].first_statement);
		pr_functions[i].parm_start      = LittleLong(pr_functions[i].parm_start);
		pr_functions[i].s_name          = LittleLong(pr_functions[i].s_name);
		pr_functions[i].s_file          = LittleLong(pr_functions[i].s_file);
		pr_functions[i].numparms        = LittleLong(pr_functions[i].numparms);
		pr_functions[i].locals          = LittleLong(pr_functions[i].locals);
	}

	for (int i = 0; i < progs->numglobaldefs; i++) {
		pr_globaldefs[i].type   = LittleShort(pr_globaldefs[i].type);
		pr_globaldefs[i].ofs    = LittleShort(pr_globaldefs[i].ofs);
		pr_globaldefs[i].s_name = LittleLong(pr_globaldefs[i].s_name);
	}

	for (int i = 0; i < progs->numfielddefs; i++) {
		pr_fielddefs[i].type = LittleShort(pr_fielddefs[i].type);
		if (pr_fielddefs[i].type & DEF_SAVEGLOBAL)
			Sys_Error("PR_LoadProgs: fielddef has DEF_SAVEGLOBAL");
		pr_fielddefs[i].ofs    = LittleShort(pr_fielddefs[i].ofs);
		pr_fielddefs[i].s_name = LittleLong(pr_fielddefs[i].s_name);
	}

	/* globals: swap the raw 32-bit words only (still IEEE-float bit patterns —
	   Phase 7 converts the ev_float / ev_vector slots to 20.12) */
	for (int i = 0; i < progs->numglobals; i++)
		((int *) pr_globals)[i] = LittleLong(((int *) pr_globals)[i]);

	printf("PR_LoadProgs: progs.dat v%d, progdefs crc %d, file crc %u (%d KiB)\n",
	       progs->version, progs->crc, pr_crc, size / 1024);
	printf("  statements=%d functions=%d globaldefs=%d fielddefs=%d strings=%dB globals=%d entityfields=%d\n",
	       progs->numstatements, progs->numfunctions, progs->numglobaldefs,
	       progs->numfielddefs, progs->numstrings, progs->numglobals, progs->entityfields);
	printf("  edict size = %d bytes\n", pr_edict_size);
}
