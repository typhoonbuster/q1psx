/*
 * q1psx — the edicts' function table (engine/ent_fns.h): every function an
 * edict's think, think1, use, touch, blocked, th_die or th_pain slot ever
 * holds, by its number.
 */

#include "ents.h"

#define X(f) void f(edict_t *self);
ENT_FN_LIST(X)
#undef X

const ent_fn ent_fns[EF_COUNT] = {
	[EF_NONE] = 0,
#define X(f) [EF_##f] = f,
	ENT_FN_LIST(X)
#undef X
};
