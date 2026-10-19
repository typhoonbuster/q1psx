/*
 * hazcheck — find MIPS I / R3000 pipeline hazards in a disassembly (host tool).
 *
 *   mipsel-none-elf-objdump -d -z --no-show-raw-insn q1psx.elf | hazcheck
 *
 * The engine's renderer is hand-written with `.set noreorder`, so the
 * assembler does not protect us. Four rules are checked:
 *
 *   load delay   the instruction right after a load (lw/lh/lhu/lb/lbu/lwl/lwr)
 *                or a coprocessor move (mfc2/cfc2/mfc0) must not read its
 *                destination register: the R3000 still sees the OLD value.
 *   load delay   the same across a branch: a load in a branch's or a jump's
 *   (branch      delay slot runs before the target's first instruction, which
 *    target)     must not read the load's destination either (static targets:
 *                b, beq... j, jal; not jr / jalr). The rule above sees only
 *                the way the branch falls through.
 *   GTE          a GTE register write (lwc2/mtc2/ctc2) must be at least two
 *                instructions before the next GTE command.
 *   HI/LO        mfhi/mflo must not be followed within two instructions by an
 *                instruction that writes HI/LO.
 *
 * Why it exists: PCSX-Redux's dynarec runs load-delay mistakes "correctly"
 * (it hands the new value over); its interpreter and real hardware do not.
 * A `lw $ra` right before `jr $ra` made r_render return into the middle of
 * its own walk on hardware while looking fine under the dynarec; a `lw` in
 * a delay slot whose target read it made the brush models' cut clear RAM
 * under the interpreter (docs/porting-notes.md §49).
 *
 * Exit status 1 if anything is found (the build fails), 0 otherwise.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXLINES (1 << 20)

typedef struct { char addr[16], op[16], args[96]; } insn;

static insn ins[MAXLINES];
static int  n;

/* the instructions by address, for the branches' targets */
typedef struct { unsigned long addr; int idx; } at_t;
static at_t by_addr[MAXLINES];

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

static int cmp_addr(const void *a, const void *b)
{
	unsigned long x = ((const at_t *) a)->addr, y = ((const at_t *) b)->addr;
	return x < y ? -1 : x > y;
}

/* the instruction at address `a`, -1 if the listing has none */
static int at(unsigned long a)
{
	int lo = 0, hi = n - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (by_addr[mid].addr == a)
			return by_addr[mid].idx;
		if (by_addr[mid].addr < a)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return -1;
}

/* a branch's or a jump's static target: its last operand, the address
   objdump prints before " <label>"; -1 if it has none */
static int target(const insn *i)
{
	static const char *const branches[] = {
		"b", "beq", "bne", "beqz", "bnez", "bltz", "bgez", "blez", "bgtz",
		"bltzal", "bgezal", "bal", "j", "jal", NULL
	};
	if (!is_one_of(i->op, branches))
		return -1;
	const char *p = strrchr(i->args, ',');
	p = p ? p + 1 : i->args;
	char *end;
	unsigned long a = strtoul(p, &end, 16);
	return end == p ? -1 : at(a);
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
		by_addr[n].addr = strtoul(i->addr, NULL, 16);
		by_addr[n].idx  = n;
		n++;
	}
	qsort(by_addr, (size_t) n, sizeof *by_addr, cmp_addr);

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
		const insn *d = &ins[k + 1];             /* the delay slot */
		int t = is_one_of(d->op, loads) ? target(a) : -1;
		if (t >= 0) {
			char dst[16];
			sscanf(d->args, "%15[^,]", dst);
			if (strcmp(dst, "zero") && reads(&ins[t], dst)) {
				printf("load delay (branch target): %s %s %s  [delay slot: %s %s %s]  ->  %s %s %s\n",
				       a->addr, a->op, a->args, d->addr, d->op, d->args, ins[t].addr, ins[t].op,
				       ins[t].args);
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
