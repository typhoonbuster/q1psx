/*
 * hazcheck — find MIPS I / R3000 pipeline hazards in a disassembly (host tool).
 *
 *   mipsel-none-elf-objdump -d -z --no-show-raw-insn q1psx.elf | hazcheck
 *
 * The engine's renderer is hand-written with `.set noreorder`, so the
 * assembler does not protect us. Three rules are checked:
 *
 *   load delay   the instruction right after a load (lw/lh/lhu/lb/lbu/lwl/lwr)
 *                or a coprocessor move (mfc2/cfc2/mfc0) must not read its
 *                destination register: the R3000 still sees the OLD value.
 *   GTE          a GTE register write (lwc2/mtc2/ctc2) must be at least two
 *                instructions before the next GTE command.
 *   HI/LO        mfhi/mflo must not be followed within two instructions by an
 *                instruction that writes HI/LO.
 *
 * Why it exists: PCSX-Redux's dynarec runs load-delay mistakes "correctly"
 * (it hands the new value over); its interpreter and real hardware do not.
 * A `lw $ra` right before `jr $ra` made r_render return into the middle of
 * its own walk on hardware while looking fine under the dynarec.
 *
 * Exit status 1 if anything is found (the build fails), 0 otherwise.
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define MAXLINES (1 << 20)

typedef struct { char addr[16], op[16], args[96]; } insn;

static insn ins[MAXLINES];
static int  n;

static int is_one_of(const char *op, const char *const *set)
{
	for (; *set; set++)
		if (!strcmp(op, *set))
			return 1;
	return 0;
}

/* Does `args` (the operand text of an instruction) read register `reg`?
   The first operand is a destination for most ALU/load ops, so skip it
   unless the op only reads (stores, branches, jr, moves to coprocessors,
   mult/div). */
static int reads(const insn *i, const char *reg)
{
	static const char *const all_read[] = {
		"sw", "sh", "sb", "swl", "swr", "swc2", "beq", "bne", "beqz", "bnez",
		"bltz", "bgez", "blez", "bgtz", "bltzal", "bgezal", "jr", "jalr",
		"mtc2", "ctc2", "mtc0", "mult", "multu", "div", "divu", "mthi", "mtlo", NULL
	};
	char buf[96];
	snprintf(buf, sizeof buf, "%s", i->args);
	char *tok = strtok(buf, " ,()");
	int first = 1, skip_first = !is_one_of(i->op, all_read);
	while (tok) {
		if (!(first && skip_first) && !strcmp(tok, reg))
			return 1;
		first = 0;
		tok = strtok(NULL, " ,()");
	}
	return 0;
}

int main(void)
{
	static char line[512];
	while (fgets(line, sizeof line, stdin) && n < MAXLINES) {
		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		char *colon = strchr(p, ':');
		if (!colon || colon == p) continue;
		int hex = 1;
		for (char *q = p; q < colon; q++) if (!isxdigit((unsigned char) *q)) hex = 0;
		if (!hex) continue;
		insn *i = &ins[n];
		memset(i, 0, sizeof *i);
		snprintf(i->addr, sizeof i->addr, "%.*s", (int) (colon - p), p);
		if (sscanf(colon + 1, " %15s %95[^\n]", i->op, i->args) < 1) continue;
		n++;
	}

	static const char *const loads[] = { "lw", "lh", "lhu", "lb", "lbu", "lwl", "lwr",
	                                     "mfc2", "cfc2", "mfc0", NULL };
	static const char *const gte_writes[] = { "lwc2", "mtc2", "ctc2", NULL };
	static const char *const hilo_writes[] = { "mult", "multu", "div", "divu", "mthi", "mtlo", NULL };
	int found = 0;

	for (int k = 0; k + 1 < n; k++) {
		const insn *a = &ins[k];
		if (is_one_of(a->op, loads)) {
			char dst[16];
			sscanf(a->args, "%15[^,]", dst);
			if (strcmp(dst, "zero") && reads(&ins[k + 1], dst)) {
				printf("load delay: %s %s %s  ->  %s %s %s\n", a->addr, a->op, a->args,
				       ins[k + 1].addr, ins[k + 1].op, ins[k + 1].args);
				found++;
			}
		}
		for (int j = 1; j <= 2 && k + j < n; j++) {
			if (is_one_of(a->op, gte_writes) && !strcmp(ins[k + j].op, "c2")) {
				printf("GTE: %s %s %s  ->  %s c2 %s (%d after)\n", a->addr, a->op, a->args,
				       ins[k + j].addr, ins[k + j].args, j);
				found++;
			}
			if ((!strcmp(a->op, "mfhi") || !strcmp(a->op, "mflo")) &&
			    is_one_of(ins[k + j].op, hilo_writes)) {
				printf("HI/LO: %s %s %s  ->  %s %s (%d after)\n", a->addr, a->op, a->args,
				       ins[k + j].addr, ins[k + j].op, j);
				found++;
			}
		}
	}

	if (found)
		printf("hazcheck: %d hazard(s) in %d instructions\n", found, n);
	return found ? 1 : 0;
}
