/*
 * q1psx — QuakeC progs runtime interface.
 *
 * Ported from id-Software/Quake WinQuake/progs.h. Phase 3.4 covers loading and
 * validating progs.dat only — no interpreter, no float->fixed conversion of the
 * globals yet (that is Phase 7, see docs/porting-notes.md §6).
 */

#pragma once

#include "pr_comp.h"
#include "progdefs.h"
#include "qcommon.h"
#include "pak.h"

typedef union eval_s {
	string_t string;
	vec_t    _float;
	vec_t    vector[3];
	func_t   function;
	int      _int;
	int      edict;
} eval_t;

#define MAX_ENT_LEAFS 16

typedef struct edict_s {
	qboolean  free;
	link_t    area;                    /* linked into a BSP node/leaf */
	int       num_leafs;
	short     leafnums[MAX_ENT_LEAFS];
	vec_t     freetime;                /* sv.time when freed */
	entvars_t v;                       /* system fields from progs */
	/* mod-specific progs fields follow immediately after v */
} edict_t;
#define EDICT_FROM_AREA(l) STRUCT_FROM_LINK(l, edict_t, area)

/* progs.dat, byte-swapped and pointed-up by PR_LoadProgs */
extern dprograms_t   *progs;
extern dfunction_t   *pr_functions;
extern char          *pr_strings;
extern ddef_t        *pr_globaldefs;
extern ddef_t        *pr_fielddefs;
extern dstatement_t  *pr_statements;
extern globalvars_t  *pr_global_struct;
extern vec_t         *pr_globals;      /* == pr_global_struct */

extern int            pr_edict_size;   /* bytes per edict */
extern unsigned short pr_crc;          /* CRC-16 of the whole progs.dat file */

/* Load "progs.dat" from `pak`, byte-swap and validate it. Sys_Error on failure.
   Does NOT execute anything. */
void PR_LoadProgs(const pak_t *pak);

/* Directory index of a function by name, or -1 (validation helper). */
int  PR_FindFunction(const char *name);
