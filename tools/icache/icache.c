/*
 * icache — the instruction cache's layout (host tool, docs/porting-notes.md §81.5).
 *
 *   icache sim <ref.elf> <ref.ldmap> TRACE...
 *   icache opt <ref.elf> <ref.ldmap> <out.ld> [-n iters] [-s seed] [-k start.ld] [-t share] [-c]
 *              [-m other.ldmap] TRACE...
 *
 *   TRACE = <trace>:<elf>:<frame>[:<weight>]  (scripts/icache.sh writes them)
 *
 * The console's instruction cache: 4 KB, direct mapped, 256 lines of 16
 * bytes; a miss fills its line from the missed word to the line's end
 * (psx-spx), a cycle a word in DuckStation's model (on the machine, more).
 * The data have none: the scratchpad stands for it.
 *
 * A trace (PCSX-Redux's interpreter, a Lua breakpoint on every fetch) is a
 * list of frames, each a list of runs, "<hex address> <count>": the
 * instructions fetched in a row. Its addresses are its own build's; each is
 * moved onto the reference link (the release) by the nearest symbol that
 * both have once, then cut by the reference's .text input sections (its GNU
 * ld map). The frames numbered <frame> are measured, the others warm the
 * cache; a weight counts a trace that many times.
 *
 * sim: the words the cache fills in the measured frames, as linked.
 * opt: a hot list (the sections that run, in an order) is placed first in
 * .text (engine/q1psx.ld: .text starts at 0x80010000, a 4 KB boundary), the
 * others after it in their own order, as the link does; each candidate is
 * simulated, a local search (one section moved or two swapped) keeps the
 * best. The hot sections: those with more than <share> of the measured
 * fetches (default 0.0001) from the asm objects and the SDK's archives
 * (the C's too with -c: its sections differ between the bench's and the
 * release's builds and move with every edit, so by default they stay after
 * the renderer's chains), and a start list's (-k: a former icache.ld, its
 * order first). -m: another link's map (the bench's): a section it lacks or
 * holds at another size stays out of the hot list, so the hot code lies
 * alike in both links (the bench measures what the release runs). The hot list goes to out.ld as input section patterns
 * (rewritten as the search goes, a minute apart at most).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NM      "mipsel-none-elf-nm"
#define OBJDUMP "mipsel-none-elf-objdump"
#define TEXT0   0x10000u                  /* .text's start, physical */
#define NONE    0xffffu

static void die(const char *m, const char *a) { fprintf(stderr, "icache: %s %s\n", m, a ? a : ""); exit(2); }
static void *xrealloc(void *p, size_t n) { p = realloc(p, n); if (!p) die("out of memory", 0); return p; }

/* ------------------------------------------------------------- symbols -- */

typedef struct { uint32_t addr; char *name; } sym_t;
typedef struct { sym_t *s; int n; } symtab_t;

static int sym_cmp(const void *a, const void *b)
{
	const sym_t *x = a, *y = b;
	if (x->addr != y->addr) return x->addr < y->addr ? -1 : 1;
	return strcmp(x->name, y->name);
}
static int name_cmp(const void *a, const void *b) { return strcmp((*(const sym_t *const *) a)->name, (*(const sym_t *const *) b)->name); }

static symtab_t read_syms(const char *elf)
{
	char cmd[1024], line[1024];
	snprintf(cmd, sizeof cmd, NM " -n '%s'", elf);
	FILE *f = popen(cmd, "r");
	if (!f) die("cannot run", cmd);
	symtab_t t = { 0, 0 };
	int cap = 0;
	while (fgets(line, sizeof line, f)) {
		char a[64], ty[8], name[512];
		if (sscanf(line, "%63s %7s %511s", a, ty, name) != 3) continue;
		if (!strchr("tTwW", ty[0])) continue;
		if (t.n == cap) t.s = xrealloc(t.s, (cap = cap ? cap * 2 : 4096) * sizeof *t.s);
		t.s[t.n].addr = (uint32_t) strtoull(a, 0, 16) & 0x1fffffffu;
		t.s[t.n].name = strdup(name);
		t.n++;
	}
	pclose(f);
	qsort(t.s, t.n, sizeof *t.s, sym_cmp);
	return t;
}

/* the names a table holds once, sorted for bsearch (pointers into t) */
static const sym_t **unique_names(const symtab_t *t, int *n)
{
	const sym_t **v = xrealloc(0, (t->n + 1) * sizeof *v);
	for (int i = 0; i < t->n; i++) v[i] = &t->s[i];
	qsort(v, t->n, sizeof *v, name_cmp);
	int k = 0;
	for (int i = 0; i < t->n; ) {
		int j = i + 1;
		while (j < t->n && !strcmp(v[j]->name, v[i]->name)) j++;
		if (j == i + 1) v[k++] = v[i];
		i = j;
	}
	*n = k;
	return v;
}

static const sym_t *find_name(const sym_t **v, int n, const char *name)
{
	int lo = 0, hi = n - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2, c = strcmp(v[mid]->name, name);
		if (!c) return v[mid];
		if (c < 0) lo = mid + 1; else hi = mid - 1;
	}
	return 0;
}

/* ------------------------------------------------------------ sections -- */

typedef struct { char *name, *obj; uint32_t addr, size, align; double heat; } sec_t;
static sec_t *sec;
static int nsec;

static int sec_cmp(const void *a, const void *b)
{
	const sec_t *x = a, *y = b;
	return x->addr < y->addr ? -1 : x->addr > y->addr;
}

/* the reference's .text input sections, from its GNU ld map */
static void read_map(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) die("cannot open", path);
	char line[4096], pending[512] = "";
	int in_text = 0, cap = 0;
	while (fgets(line, sizeof line, f)) {
		if (!strncmp(line, ".text", 5) && (line[5] == ' ' || line[5] == '\n')) { in_text = 1; continue; }
		if (line[0] == '.' ) { in_text = 0; continue; }
		if (!in_text) continue;
		char name[512], obj[2048];
		unsigned long long a, s;
		if (pending[0]) {
			if (sscanf(line, " 0x%llx 0x%llx %2047s", &a, &s, obj) == 3) goto add;
			pending[0] = 0;
		}
		if (sscanf(line, " %511s 0x%llx 0x%llx %2047s", name, &a, &s, obj) == 4 && !strncmp(name, ".text", 5)) {
			strcpy(pending, name);
			goto add;
		}
		if (sscanf(line, " %511s", name) == 1 && !strncmp(name, ".text", 5) && line[0] == ' ' && line[1] == '.')
			strcpy(pending, name);
		continue;
	add:
		if (s) {
			if (nsec == cap) sec = xrealloc(sec, (cap = cap ? cap * 2 : 1024) * sizeof *sec);
			sec[nsec] = (sec_t) { strdup(pending), strdup(obj), (uint32_t) a & 0x1fffffffu, (uint32_t) s, 4, 0 };
			nsec++;
		}
		pending[0] = 0;
	}
	fclose(f);
	qsort(sec, nsec, sizeof *sec, sec_cmp);
}

/* another link's .text input sections (-m: the bench's beside the release's):
   name, object's base name and size, to keep only the sections both links
   have alike in the hot list */
typedef struct { char *name, *obj; uint32_t size; } osec_t;
static osec_t *osec;
static int nosec;

static const char *base_name(const char *o)
{
	const char *b = o;
	for (const char *p = o; *p && *p != '('; p++) if (*p == '/') b = p + 1;
	return b;
}

static void read_other(const char *path)
{
	sec_t *keep = sec;
	int keepn = nsec;
	sec = 0; nsec = 0;
	read_map(path);
	osec = xrealloc(0, (nsec + 1) * sizeof *osec);
	for (int i = 0; i < nsec; i++) osec[i] = (osec_t) { sec[i].name, sec[i].obj, sec[i].size };
	nosec = nsec;
	free(sec);
	sec = keep; nsec = keepn;
}

static int alike(const sec_t *s)
{
	if (!osec) return 1;
	for (int i = 0; i < nosec; i++)
		if (!strcmp(osec[i].name, s->name) && !strcmp(base_name(osec[i].obj), base_name(s->obj)))
			return osec[i].size == s->size;
	return 0;
}

/* each object's sections' alignments (objdump -h; an archive's member: 4) */
static void read_aligns(const char *mapdir)
{
	for (int i = 0; i < nsec; i++) {
		if (strchr(sec[i].obj, '(')) continue;
		int done = 0;
		for (int j = 0; j < i; j++)
			if (!strcmp(sec[j].obj, sec[i].obj)) { done = 1; break; }
		if (done) continue;
		char cmd[4096], line[1024];
		snprintf(cmd, sizeof cmd, OBJDUMP " -h '%s%s%s' 2>/dev/null", sec[i].obj[0] == '/' ? "" : mapdir,
		         sec[i].obj[0] == '/' ? "" : "/", sec[i].obj);
		FILE *f = popen(cmd, "r");
		if (!f) continue;
		while (fgets(line, sizeof line, f)) {
			int idx; char name[512], sz[32], vma[32], lma[32], off[32], al[32];
			if (sscanf(line, "%d %511s %31s %31s %31s %31s %31s", &idx, name, sz, vma, lma, off, al) != 7) continue;
			if (strncmp(al, "2**", 3)) continue;
			uint32_t a = 1u << atoi(al + 3);
			for (int j = i; j < nsec; j++)
				if (!strcmp(sec[j].obj, sec[i].obj) && !strcmp(sec[j].name, name))
					sec[j].align = a < 4 ? 4 : a;
		}
		pclose(f);
	}
}

static int sec_at(uint32_t a)
{
	int lo = 0, hi = nsec - 1, r = -1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (sec[mid].addr <= a) { r = mid; lo = mid + 1; } else hi = mid - 1;
	}
	return r >= 0 && a < sec[r].addr + sec[r].size ? r : -1;
}

/* -------------------------------------------------------------- traces -- */

typedef struct { uint32_t addr, count, sec; } rec_t;   /* count 0: a frame's start, addr 1 = measured */
static rec_t *rec;
static long nrec, caprec;
static uint64_t measured;

static void push(uint32_t a, uint32_t c, uint32_t s)
{
	if (nrec == caprec) rec = xrealloc(rec, (caprec = caprec ? caprec * 2 : 1 << 20) * sizeof *rec);
	rec[nrec++] = (rec_t) { a, c, s };
}

static void read_trace(const char *spec, const symtab_t *ref, const sym_t **refu, int nrefu)
{
	char buf[4096];
	snprintf(buf, sizeof buf, "%s", spec);
	char *tr = strtok(buf, ":"), *elf = strtok(0, ":"), *fr = strtok(0, ":"), *w = strtok(0, ":");
	if (!tr || !elf || !fr) die("a trace is <trace>:<elf>:<frame>[:<weight>]:", spec);
	int frame = atoi(fr), weight = w ? atoi(w) : 1;
	(void) ref;
	/* the anchors: the trace build's symbols the reference has once too */
	symtab_t t = read_syms(elf);
	int ntu;
	const sym_t **tu = unique_names(&t, &ntu);
	uint32_t *aa = xrealloc(0, (ntu + 1) * sizeof *aa), *ra = xrealloc(0, (ntu + 1) * sizeof *ra);
	int na = 0;
	for (int i = 0; i < t.n; i++) {                       /* (in address order) */
		const sym_t *u = find_name(tu, ntu, t.s[i].name);
		if (u != &t.s[i]) continue;
		const sym_t *r = find_name(refu, nrefu, t.s[i].name);
		if (!r) continue;
		aa[na] = t.s[i].addr;
		ra[na] = r->addr;
		na++;
	}
	FILE *f = fopen(tr, "r");
	if (!f) die("cannot open", tr);
	long first = nrec;
	int measure = 0;
	char line[256];
	while (fgets(line, sizeof line, f)) {
		if (line[0] == 'F') {
			int v, fn;
			if (sscanf(line, "F %d %d", &v, &fn) == 2) measure = fn == frame;
			push(measure, 0, NONE);
			continue;
		}
		if (!strncmp(line, "END", 3)) break;
		unsigned long long a64; unsigned c;
		if (sscanf(line, "%llx %u", &a64, &c) != 2) continue;
		uint32_t a = (uint32_t) a64 & 0x1fffffffu;
		while (c > 0) {
			/* onto the reference: the nearest anchor at or before */
			int lo = 0, hi = na - 1, k = -1;
			while (lo <= hi) {
				int mid = (lo + hi) / 2;
				if (aa[mid] <= a) { k = mid; lo = mid + 1; } else hi = mid - 1;
			}
			uint32_t r = k >= 0 && a < 0x200000u ? ra[k] + (a - aa[k]) : a;
			int s = sec_at(r);
			uint32_t n = c;
			if (s >= 0) {
				uint32_t left = (sec[s].addr + sec[s].size - r) / 4;
				if (left < n) n = left ? left : 1;
			} else if (k + 1 < na && a + 4 * c > aa[k + 1]) {
				n = (aa[k + 1] - a) / 4;                       /* (to the next anchor) */
				if (!n) n = 1;
			}
			push(r, n, s >= 0 ? (uint32_t) s : NONE);
			if (measure) {
				measured += n;
				if (s >= 0) sec[s].heat += n;
			}
			a += 4 * n;
			c -= n;
		}
	}
	fclose(f);
	long last = nrec;
	for (int i = 1; i < weight; i++)
		for (long j = first; j < last; j++)
			push(rec[j].addr, rec[j].count, rec[j].sec);
	measured += (uint64_t) (weight - 1) * 0;   /* (the shares print per copy) */
	free(aa); free(ra); free(tu);
}

/* ---------------------------------------------------------- simulation -- */

static uint32_t *newbase;

static uint64_t simulate(void)
{
	static int32_t tag[256], first[256];
	for (int i = 0; i < 256; i++) { tag[i] = -1; first[i] = 4; }
	uint64_t words = 0;
	int measure = 0;
	for (long i = 0; i < nrec; i++) {
		uint32_t a = rec[i].addr, c = rec[i].count, s = rec[i].sec;
		if (!c) { measure = a & 1; continue; }
		if (s != NONE) a = a - sec[s].addr + newbase[s];
		uint32_t e = a + 4 * c;
		while (a < e) {
			int line = (a >> 4) & 255, tg = (int) (a >> 12), w = (a >> 2) & 3;
			if (!(tag[line] == tg && first[line] <= w)) {
				if (measure) words += 4 - w;
				first[line] = tag[line] != tg ? w : (first[line] < w ? first[line] : w);
				tag[line] = tg;
			}
			a = (a | 15) + 1;
		}
	}
	return words;
}

/* the link's places (engine/q1psx.ld): the hot list first, the renderer's
   other chains (.text.ic.*: within a branch's reach of the hot ones), then
   the others, each group in its order */
static void place(const int *hot, int nhot)
{
	static char *is_hot;
	is_hot = xrealloc(is_hot, nsec);
	memset(is_hot, 0, nsec);
	uint32_t a = TEXT0;
	for (int i = 0; i < nhot; i++) {
		sec_t *s = &sec[hot[i]];
		a = (a + s->align - 1) & ~(s->align - 1);
		newbase[hot[i]] = a;
		a += s->size;
		is_hot[hot[i]] = 1;
	}
	for (int pass = 0; pass < 2; pass++)              /* the renderer's other chains, then the rest */
		for (int i = 0; i < nsec; i++) {
			if (is_hot[i] || (strncmp(sec[i].name, ".text.ic.", 9) == 0) != (pass == 0)) continue;
			a = (a + sec[i].align - 1) & ~(sec[i].align - 1);
			newbase[i] = a;
			a += sec[i].size;
		}
}

static void write_list(const char *out, const int *hot, int nhot, uint64_t w0, uint64_t best);

static int heat_cmp(const void *a, const void *b)
{
	const sec_t *x = &sec[*(const int *) a], *y = &sec[*(const int *) b];
	double dx = x->heat / (x->size > 4 ? x->size : 4), dy = y->heat / (y->size > 4 ? y->size : 4);
	return dx < dy ? 1 : dx > dy ? -1 : 0;
}

/* a pattern for the input section (q1psx.ld: the object's name, or an
   archive's member) */
static void pattern(FILE *f, const sec_t *s)
{
	const char *o = s->obj, *par = strchr(o, '(');
	if (par) {
		const char *base = o;
		for (const char *p = o; p < par; p++) if (*p == '/') base = p + 1;
		fprintf(f, "\t*%.*s:%.*s(%s)\n", (int) (par - base), base, (int) (strlen(par) - 2), par + 1, s->name);
	} else {
		const char *base = strrchr(o, '/');
		fprintf(f, "\t*/%s(%s)\n", base ? base + 1 : o, s->name);
	}
}

/* a hot list to start from (a former icache.ld): its sections, in order */
static int read_start(const char *path, int *hot)
{
	FILE *f = fopen(path, "r");
	if (!f) die("cannot open", path);
	char line[1024];
	int n = 0;
	while (fgets(line, sizeof line, f)) {
		/* a star, a slash and the object, or a star, the archive and its member,
		   then the section in brackets (pattern() writes them) */
		char *star = strchr(line, '*'), *o = strchr(line, '('), *c = o ? strchr(o, ')') : 0;
		if (!star || !o || !c || star > o) continue;
		*o = *c = 0;
		const char *obj = star + 1, *name = o + 1;
		if (*obj == '/') obj++;
		char *colon = strchr(obj, ':');
		if (colon) *colon = 0;                          /* (the archive's name will do) */
		for (int i = 0; i < nsec; i++) {
			if (strcmp(sec[i].name, name) || !strstr(sec[i].obj, obj)) continue;
			int dup = 0;
			for (int j = 0; j < n; j++) if (hot[j] == i) dup = 1;
			if (!dup) hot[n++] = i;
			break;
		}
	}
	fclose(f);
	return n;
}

int main(int argc, char **argv)
{
	if (argc < 5) {
		fprintf(stderr, "usage: icache sim <ref.elf> <ref.ldmap> TRACE...\n"
		                "       icache opt <ref.elf> <ref.ldmap> <out.ld> [-n iters] [-s seed] [-k start.ld] TRACE...\n"
		                "TRACE: <trace>:<elf>:<frame>[:<weight>]\n");
		return 2;
	}
	int opt = !strcmp(argv[1], "opt");
	const char *elf = argv[2], *map = argv[3], *out = opt ? argv[4] : 0, *start = 0;
	long iters = 20000;
	unsigned seed = 1;
	double least = 0.0001;                              /* a hot section's least share of the fetches */
	int with_c = 0;                                     /* the C's sections hot too (-c) */
	const char *other = 0;                              /* another link's map (-m) */
	int ai = opt ? 5 : 4;
	while (ai < argc && argv[ai][0] == '-') {
		if (!strcmp(argv[ai], "-n") && ai + 1 < argc) iters = atol(argv[++ai]);
		else if (!strcmp(argv[ai], "-s") && ai + 1 < argc) seed = (unsigned) atol(argv[++ai]);
		else if (!strcmp(argv[ai], "-k") && ai + 1 < argc) start = argv[++ai];
		else if (!strcmp(argv[ai], "-t") && ai + 1 < argc) least = atof(argv[++ai]);
		else if (!strcmp(argv[ai], "-c")) with_c = 1;
		else if (!strcmp(argv[ai], "-m") && ai + 1 < argc) other = argv[++ai];
		else die("unknown option", argv[ai]);
		ai++;
	}
	if (other) read_other(other);
	read_map(map);
	char mapdir[4096];
	snprintf(mapdir, sizeof mapdir, "%s", map);
	char *sl = strrchr(mapdir, '/');
	if (sl) *sl = 0; else strcpy(mapdir, ".");
	read_aligns(mapdir);
	symtab_t ref = read_syms(elf);
	int nrefu;
	const sym_t **refu = unique_names(&ref, &nrefu);
	for (; ai < argc; ai++)
		read_trace(argv[ai], &ref, refu, nrefu);
	newbase = xrealloc(0, nsec * sizeof *newbase);
	for (int i = 0; i < nsec; i++) newbase[i] = sec[i].addr;
	uint64_t w0 = simulate();
	printf("sections %d; measured: %llu instructions (a copy of each trace), %llu words filled as linked\n", nsec,
	       (unsigned long long) measured, (unsigned long long) w0);
	if (!opt) return 0;

	int *hot = xrealloc(0, nsec * sizeof *hot), nhot = 0;
	if (start) {
		int k = 0;
		nhot = read_start(start, hot);
		for (int i = 0; i < nhot; i++)
			if (alike(&sec[hot[i]])) hot[k++] = hot[i];   /* (the start list's too) */
		nhot = k;
	}
	int *rest = xrealloc(0, nsec * sizeof *rest), nrest = 0;
	for (int i = 0; i < nsec; i++) {
		if (sec[i].heat <= least * (double) measured) continue;
		size_t ol = strlen(sec[i].obj);
		if (!with_c && !(ol > 6 && !strcmp(sec[i].obj + ol - 6, ".s.obj")) && !strchr(sec[i].obj, '('))
			continue;                                 /* (the asm's, the SDK's: the same in every build) */
		if (!alike(&sec[i]))
			continue;                                 /* (not in the other link, or of another size) */
		int in = 0;
		for (int j = 0; j < nhot; j++) if (hot[j] == i) { in = 1; break; }
		if (!in) rest[nrest++] = i;
	}
	qsort(rest, nrest, sizeof *rest, heat_cmp);           /* the hottest a byte first */
	memcpy(hot + nhot, rest, nrest * sizeof *rest);
	nhot += nrest;
	place(hot, nhot);
	uint64_t best = simulate();
	printf("start (%d sections): %llu words\n", nhot, (unsigned long long) best);
	int *cand = xrealloc(0, nhot * sizeof *cand);
	srand(seed);
	time_t saved = time(0);
	for (long it = 0; it < iters && nhot > 1; it++) {
		memcpy(cand, hot, nhot * sizeof *hot);
		int i = rand() % nhot, j = rand() % nhot;
		if (i == j) continue;
		if (rand() % 10 < 7) {                            /* one moved */
			int x = cand[i];
			if (i < j) memmove(cand + i, cand + i + 1, (j - i) * sizeof *cand);
			else memmove(cand + j + 1, cand + j, (i - j) * sizeof *cand);
			cand[j] = x;
		} else {                                          /* two swapped */
			int x = cand[i]; cand[i] = cand[j]; cand[j] = x;
		}
		place(cand, nhot);
		uint64_t w = simulate();
		if (w < best) {
			best = w;
			memcpy(hot, cand, nhot * sizeof *hot);
			printf("  %6ld: %llu words (%.1f %% fewer than as linked)\n", it, (unsigned long long) w,
			       100.0 * (double) (w0 - w) / (double) w0);
			fflush(stdout);
			if (time(0) - saved >= 60) {              /* (the best so far, a minute at most behind) */
				write_list(out, hot, nhot, w0, best);
				saved = time(0);
			}
		}
	}
	write_list(out, hot, nhot, w0, best);
	uint32_t bytes = 0;
	for (int i = 0; i < nhot; i++) bytes += sec[hot[i]].size;
	printf("%s: %d sections, %u bytes; %llu words filled (%.1f %% fewer than as linked)\n", out, nhot, bytes,
	       (unsigned long long) best, 100.0 * (double) (w0 - best) / (double) w0);
	return 0;
}

static void write_list(const char *out, const int *hot, int nhot, uint64_t w0, uint64_t best)
{
	FILE *f = fopen(out, "w");
	if (!f) die("cannot write", out);
	fprintf(f, "/* engine/icache.ld: the instruction cache's hot sections, in their order\n"
	           "   at the start of .text (engine/q1psx.ld). Made by tools/icache\n"
	           "   (scripts/icache.sh, docs/porting-notes.md §81.5): %llu words filled\n"
	           "   as linked before, %llu in this order, over its traces. */\n",
	           (unsigned long long) w0, (unsigned long long) best);
	for (int i = 0; i < nhot; i++) pattern(f, &sec[hot[i]]);
	fclose(f);
}
