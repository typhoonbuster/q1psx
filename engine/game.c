#include <stdint.h>
#include <string.h>

#include "clip.h"
#include "dlight.h"
#include "game.h"
#include "gdata.h"
#include "prof.h"
#include "q_fixed.h"
#include "render.h"
#include "rlist.h"
#include "snd.h"
#include "vid.h"
#include "world.h"

player_t pl;
int      game_tick_count;

#define SUB CLIP_SUB                            /* 8 position sub-units per world unit */
#define TICKS(sec100) ((sec100) * GAME_HZ / 100)   /* hundredths of a second -> ticks */
#define DEG(d) ((d) * ANG_TURN / 360)

/* ---------------------------------------------------------------- random -- */

static uint32_t rng_state = 0x1234567u;

uint32_t q_rand(void)
{
	rng_state = rng_state * 1664525u + 1013904223u;
	return rng_state >> 16;
}

static int32_t crandom(int32_t scale)           /* -scale .. scale */
{
	return (int32_t) (q_rand() % (uint32_t) (2 * scale + 1)) - scale;
}

/* --------------------------------------------------------------- vectors -- */

/* Quake's AngleVectors for the player's view (no roll), x4096 */
static void view_vectors(int yaw, int pitch, int32_t f[3], int32_t r[3], int32_t u[3])
{
	int32_t sy = r_sin(yaw), cy = r_cos(yaw), sp = r_sin(pitch), cp = r_cos(pitch);
	f[0] = cp * cy >> 12; f[1] = cp * sy >> 12; f[2] = -sp;
	r[0] = sy;            r[1] = -cy;           r[2] = 0;
	u[0] = sp * cy >> 12; u[1] = sp * sy >> 12; u[2] = cp;
}

/* rint(random() x n) + 1: 1 .. n + 1 (the sound picks) */
static int rand_rint(int n)
{
	return (int) (((q_rand() & 0x7fff) * (uint32_t) n + 16383) / 32767) + 1;
}

static int32_t vlen(const int32_t v[3])
{
	return (int32_t) q_isqrt64((uint64_t) ((int64_t) v[0] * v[0] + (int64_t) v[1] * v[1] +
	                                       (int64_t) v[2] * v[2]));
}

/* v x4096 / |v| */
static void vnormalize(const int32_t v[3], int32_t out[3])
{
	int32_t l = vlen(v);
	for (int k = 0; k < 3; k++)
		out[k] = l ? (int32_t) ((int64_t) v[k] * 4096 / l) : 0;
}

/* Quake's vectoangles: yaw and pitch (positive up) of a direction */
static void vectoangles(const int32_t v[3], int *yaw, int *pitch)
{
	if (!v[0] && !v[1]) {
		*yaw = 0;
		*pitch = v[2] > 0 ? DEG(90) : DEG(270);
		return;
	}
	*yaw = r_atan2(v[1], v[0]);
	int32_t fwd = (int32_t) q_isqrt64((uint64_t) ((int64_t) v[0] * v[0] + (int64_t) v[1] * v[1]));
	*pitch = r_atan2(v[2], fwd);
}

/* ------------------------------------------------------------- particles -- */

/* Quake's particle ramps (r_part.c) */
static const uint8_t ramp1[8] = { 0x6f, 0x6d, 0x6b, 0x69, 0x67, 0x65, 0x63, 0x61 };
static const uint8_t ramp2[8] = { 0x6f, 0x6e, 0x6d, 0x6c, 0x6b, 0x6a, 0x68, 0x66 };
static const uint8_t ramp3[8] = { 0x6d, 0x6b, 6, 5, 4, 3, 3, 3 };

enum { PT_STATIC, PT_GRAV, PT_SLOWGRAV, PT_FIRE, PT_EXPLODE, PT_EXPLODE2 };

#define MAX_PARTICLES 256
#define PV_SHIFT 8       /* particle velocity: 1/SUB units per tick x 256 */

typedef struct {
	int32_t org[3];        /* 1/SUB units                                  */
	int32_t vel[3];        /* 1/SUB units per tick, x 256                  */
	int32_t die;           /* tick                                         */
	int32_t ramp;          /* x 256                                        */
	uint8_t type, colour;
	uint16_t next;         /* r_part.c's lists: the next particle + 1, 0 ends */
	const qw_leaf *leaf;
} particle_t;

static particle_t particles[MAX_PARTICLES];
static uint16_t   part_active, part_free;   /* the lists' heads, index + 1 (R_ClearParticles) */

/* u/s -> particle velocity */
static int32_t pvel(int32_t ups)
{
	return (int32_t) (((int64_t) ups * SUB * 256) / GAME_HZ);
}

#define PGRAV ((800 * SUB * 256) / (GAME_HZ * GAME_HZ) / 20)   /* r_part.c: gravity x 0.05 */

/* R_ClearParticles: every particle on the free list */
static void particles_clear(void)
{
	part_active = 0;
	part_free   = 1;
	for (int i = 0; i < MAX_PARTICLES; i++)
		particles[i].next = (uint16_t) (i + 2 <= MAX_PARTICLES ? i + 2 : 0);
}

/* the free list's head onto the active list's (r_part.c: none left, none made) */
static particle_t *particle_new(void)
{
	if (!part_free)
		return 0;
	particle_t *p = &particles[part_free - 1];
	int i = part_free;
	part_free   = p->next;
	p->next     = (uint16_t) part_active;
	part_active = i;
	return p;
}

/* R_RunParticleEffect: `count` sparks of `colour` (its 8-shade ramp) */
static void particle_effect(const int32_t org[3], const qw_leaf *leaf, int colour, int count)
{
	for (int i = 0; i < count; i++) {
		particle_t *p = particle_new();
		if (!p) return;
		p->die    = game_tick_count + TICKS(10) * (int) (q_rand() % 5) + 1;
		p->colour = (uint8_t) ((colour & ~7) + (q_rand() & 7));
		p->type   = PT_SLOWGRAV;
		p->leaf   = leaf;
		for (int k = 0; k < 3; k++) {
			p->org[k] = org[k] + crandom(8) * SUB;
			p->vel[k] = 0;
		}
	}
}

/* R_ParticleExplosion, scaled down from 1024 to 48 */
static void particle_explosion(const int32_t org[3], const qw_leaf *leaf)
{
	for (int i = 0; i < 48; i++) {
		particle_t *p = particle_new();
		if (!p) return;
		p->die    = game_tick_count + TICKS(500);
		p->colour = ramp1[0];
		p->ramp   = (int32_t) (q_rand() & 3) << 8;
		p->type   = (i & 1) ? PT_EXPLODE : PT_EXPLODE2;
		p->leaf   = leaf;
		for (int k = 0; k < 3; k++) {
			p->org[k] = org[k] + crandom(16) * SUB;
			p->vel[k] = pvel(crandom(256));
		}
	}
}

/* R_RocketTrail types 0 (rocket: fire) and 1 (grenade: smoke): one
   particle per tick, not one per 3 units */
static void particle_trail(const int32_t org[3], const qw_leaf *leaf, int smoke)
{
	particle_t *p = particle_new();
	if (!p) return;
	p->die    = game_tick_count + TICKS(50);
	p->ramp   = ((int32_t) (q_rand() & 3) + (smoke ? 2 : 0)) << 8;
	p->colour = ramp3[p->ramp >> 8];
	p->type   = PT_FIRE;
	p->leaf   = leaf;
	for (int k = 0; k < 3; k++) {
		p->org[k] = org[k] + crandom(3) * SUB;
		p->vel[k] = 0;
	}
}

/* R_DrawParticles' run: the active list walked, a dead particle (its time
   or its ramp's end) unlinked onto the free list */
static void particles_tick(void)
{
	uint16_t *link = &part_active;
	for (int i = part_active; i; ) {
		particle_t *p = &particles[i - 1];
		int dead = p->die <= game_tick_count;
		if (!dead) {
			for (int k = 0; k < 3; k++)
				p->org[k] += p->vel[k] >> PV_SHIFT;
			switch (p->type) {
			case PT_FIRE:                                 /* ramp3, rises */
				p->ramp += 256 * 5 / GAME_HZ;
				if (p->ramp >= 6 << 8) { dead = 1; break; }
				p->colour = ramp3[p->ramp >> 8];
				p->vel[2] += PGRAV;
				break;
			case PT_EXPLODE:                              /* ramp1, speeds up, falls */
				p->ramp += 256 * 10 / GAME_HZ;
				if (p->ramp >= 8 << 8) { dead = 1; break; }
				p->colour = ramp1[p->ramp >> 8];
				for (int k = 0; k < 3; k++)
					p->vel[k] += p->vel[k] / 15;
				p->vel[2] -= PGRAV;
				break;
			case PT_EXPLODE2:                             /* ramp2, slows down, falls */
				p->ramp += 256 * 15 / GAME_HZ;
				if (p->ramp >= 8 << 8) { dead = 1; break; }
				p->colour = ramp2[p->ramp >> 8];
				for (int k = 0; k < 3; k++)
					p->vel[k] -= p->vel[k] / GAME_HZ;
				p->vel[2] -= PGRAV;
				break;
			case PT_GRAV:
			case PT_SLOWGRAV:
				p->vel[2] -= PGRAV;
				break;
			default:
				break;
			}
		}
		int next = p->next;
		if (dead) {
			*link     = (uint16_t) next;
			p->next   = (uint16_t) part_free;
			part_free = i;
		} else {
			link = &p->next;
		}
		i = next;
	}
}

/* ----------------------------------------------------------- projectiles -- */

enum { PJ_FREE, PJ_SPIKE, PJ_SSPIKE, PJ_GRENADE, PJ_ROCKET, PJ_EXPLOSION, PJ_BEAM, PJ_BUBBLE, PJ_BUBBLER };

#define MAX_PROJ 48

typedef struct {
	int      type;
	int32_t  org[3];       /* 1/SUB units                                  */
	int32_t  vel[3];       /* u/s                                          */
	int32_t  end[3];       /* a beam's far end                             */
	int      yaw, pitch, roll;
	int      grav_acc;
	int      onground;
	int      die;          /* tick                                         */
	int      frame_tick;   /* an explosion: when its frame advances        */
	int      frame;
	const qw_leaf *leaf;
} proj_t;

static proj_t projs[MAX_PROJ];
static int    proj_top;                /* past the highest slot in use: the loops' bound */

static proj_t *proj_new(int type)
{
	for (int i = 0; i < MAX_PROJ; i++)
		if (projs[i].type == PJ_FREE) {
			memset(&projs[i], 0, sizeof projs[i]);
			projs[i].type = type;
			if (i >= proj_top)
				proj_top = i + 1;
			return &projs[i];
		}
	return 0;
}

static const qw_leaf *leaf_at(const int32_t org[3])
{
	int32_t w[3] = { org[0] / SUB, org[1] / SUB, org[2] / SUB };
	return world_point_leaf(w);
}

static void t_radius_damage(const int32_t org[3], int damage, int self_inflicted);

/* TE_EXPLOSION + BecomeExplosion at org */
static void explode_at(const int32_t org[3])
{
	const qw_leaf *leaf = leaf_at(org);
	particle_explosion(org, leaf);
	snd_start(ENT_TEMP, 0, S_R_EXP3, org, 255, ATTN_NORM);
	dlight_t *dl = dl_alloc(0, game_tick_count);     /* TE_EXPLOSION's light */
	for (int k = 0; k < 3; k++) dl->origin[k] = org[k];
	dl->radius = 350;
	dl->die    = game_tick_count + TICKS(50);
	dl->decay  = 300;
	proj_t *e = proj_new(PJ_EXPLOSION);
	if (e) {
		for (int k = 0; k < 3; k++) e->org[k] = org[k];
		e->leaf       = leaf;
		e->frame      = 0;
		e->frame_tick = game_tick_count + TICKS(10);
		e->die        = game_tick_count + 6 * TICKS(10);
	}
}

/* a missile (rocket / nail) or a grenade for this tick: SV_Physics_Toss */
static void proj_move(proj_t *p)
{
	if (p->type == PJ_GRENADE) {
		if (p->onground)
			return;
		p->grav_acc += 800;                           /* sv_gravity per tick, exactly */
		p->vel[2] -= p->grav_acc / GAME_HZ;
		p->grav_acc %= GAME_HZ;
		p->roll += DEG(300) / GAME_HZ;                /* avelocity '300 300 300' */
		p->pitch += DEG(300) / GAME_HZ;
		p->yaw += DEG(300) / GAME_HZ;
	}
	int32_t end[3];
	for (int k = 0; k < 3; k++)
		end[k] = p->org[k] + (int32_t) (((int64_t) p->vel[k] * SUB * 1092) >> 16);   /* x 8/60 */
	trace_t tr;
	clip_trace(0, p->org, end, &tr);
	int hit = tr.fraction < Q_FIX_ONE || tr.startsolid;
	for (int k = 0; k < 3; k++)
		p->org[k] = tr.startsolid ? p->org[k] : tr.endpos[k];
	p->leaf = leaf_at(p->org);
	if (!hit)
		return;

	if (clip_contents(0, p->org) == -6) {             /* CONTENTS_SKY: gone */
		p->type = PJ_FREE;
		return;
	}
	switch (p->type) {
	case PJ_SPIKE:
	case PJ_SSPIKE:
		particle_effect(p->org, p->leaf, 0, p->type == PJ_SPIKE ? 10 : 20);   /* TE_SPIKE */
		if (q_rand() % 5)
			snd_start(ENT_TEMP, 0, S_TINK1, p->org, 255, ATTN_NORM);
		else {
			int rnd = (int) (q_rand() & 3);
			snd_start(ENT_TEMP, 0, rnd == 1 ? S_RIC1 : rnd == 2 ? S_RIC2 : S_RIC3, p->org, 255, ATTN_NORM);
		}
		p->type = PJ_FREE;
		break;
	case PJ_ROCKET: {
		t_radius_damage(p->org, 120, 1);
		int32_t dir[3], org[3];
		vnormalize(p->vel, dir);
		for (int k = 0; k < 3; k++)
			org[k] = p->org[k] - 8 * SUB * dir[k] / 4096;
		p->type = PJ_FREE;
		explode_at(org);
		break;
	}
	case PJ_GRENADE: {
		/* ClipVelocity with overbounce 1.5; rests on a floor when slow */
		int32_t n[3] = { tr.plane_n[0], tr.plane_n[1], tr.plane_n[2] };
		snd_start(ENT_PROJ + (int) (p - projs), CHAN_WEAPON, S_BOUNCE, p->org, 255, ATTN_NORM);
		int32_t dot = (p->vel[0] * n[0] + p->vel[1] * n[1] + p->vel[2] * n[2]) >> 12;
		int32_t backoff = dot * 3 / 2;
		for (int k = 0; k < 3; k++)
			p->vel[k] -= n[k] * backoff >> 12;
		if (n[2] > 2867 && p->vel[2] < 60) {           /* 0.7 x 4096 */
			p->onground = 1;
			p->vel[0] = p->vel[1] = p->vel[2] = 0;
		}
		break;
	}
	default:
		break;
	}
}

/* ------------------------------------------------------------------ bubbles -- */

/* misc.qc: a bubble (progs/s_bubble.spr) rising at 15 units/s, MOVETYPE_NOCLIP
   (no collision, its touch never runs); every half second (bubble_bob) a
   random drift, at the 4th think it turns to frame 1 (bubble_split: the
   split copy keeps the place, the original goes, its water level never
   being set for a noclip entity), at the 20th it goes. frame = the sprite
   frame, grav_acc = cnt, frame_tick = the next think, end = the sub-unit
   carry */
static void bubble_spawn(const int32_t org[3], const int32_t vel[3], int frame, int cnt)
{
	proj_t *b = proj_new(PJ_BUBBLE);
	if (!b)
		return;
	for (int k = 0; k < 3; k++) {
		b->org[k] = org[k];
		b->vel[k] = vel[k];
	}
	b->leaf       = leaf_at(b->org);
	b->frame      = frame;
	b->grav_acc   = cnt;
	b->frame_tick = game_tick_count + TICKS(50);
}

static void bubble_tick(proj_t *p)
{
	if (game_tick_count >= p->frame_tick) {           /* bubble_bob */
		p->frame_tick = game_tick_count + TICKS(50);
		if (++p->grav_acc == 4) {                     /* bubble_split */
			p->frame    = 1;
			p->grav_acc = 10;
		}
		if (p->grav_acc == 20) {
			p->type = PJ_FREE;
			return;
		}
		int32_t rnd1 = p->vel[0] - 10 + (int32_t) (q_rand() % 20);
		int32_t rnd2 = p->vel[1] - 10 + (int32_t) (q_rand() % 20);
		int32_t rnd3 = p->vel[2] + 10 + (int32_t) (q_rand() % 10);
		if (rnd1 > 10)  rnd1 = 5;
		if (rnd1 < -10) rnd1 = -5;
		if (rnd2 > 10)  rnd2 = 5;
		if (rnd2 < -10) rnd2 = -5;
		if (rnd3 < 10)  rnd3 = 15;
		if (rnd3 > 30)  rnd3 = 25;
		p->vel[0] = rnd1;
		p->vel[1] = rnd2;
		p->vel[2] = rnd3;
	}
	for (int k = 0; k < 3; k++) {                     /* SV_Physics_Noclip */
		p->end[k] += p->vel[k] * SUB;
		p->org[k] += p->end[k] / GAME_HZ;
		p->end[k] -= p->end[k] / GAME_HZ * GAME_HZ;
	}
	p->leaf = leaf_at(p->org);
}

/* player.qc DeathBubbles: a spawner at the player, one bubble every 0.1 s
   24 units over the player while the player's eyes are under water, roll
   bubbles in all; grav_acc counts them */
static void death_bubbles(int num_bubbles)
{
	proj_t *b = proj_new(PJ_BUBBLER);
	if (!b)
		return;
	b->roll       = num_bubbles;
	b->grav_acc   = 0;
	b->frame_tick = game_tick_count + TICKS(10);
}

static void bubbler_tick(proj_t *p)
{
	if (game_tick_count < p->frame_tick)
		return;
	if (pl.pm.waterlevel != 3) {
		p->type = PJ_FREE;
		return;
	}
	int32_t org[3] = { pl.pm.pos[0], pl.pm.pos[1], pl.pm.pos[2] + 24 * SUB };
	static const int32_t up[3] = { 0, 0, 15 };
	bubble_spawn(org, up, 0, 0);
	p->frame_tick = game_tick_count + TICKS(10);
	if (++p->grav_acc >= p->roll)
		p->type = PJ_FREE;
}

static void projs_tick(void)
{
	int top = 0;
	for (int i = 0; i < proj_top; i++) {
		proj_t *p = &projs[i];
		if (p->type != PJ_FREE)
			top = i + 1;
		switch (p->type) {
		case PJ_FREE:
			break;
		case PJ_EXPLOSION:
			if (game_tick_count >= p->frame_tick) {
				p->frame++;
				p->frame_tick += TICKS(10);
			}
			if (p->frame >= 6 || game_tick_count >= p->die)
				p->type = PJ_FREE;
			break;
		case PJ_BEAM:
			if (game_tick_count >= p->die)
				p->type = PJ_FREE;
			break;
		case PJ_BUBBLE:
			bubble_tick(p);
			break;
		case PJ_BUBBLER:
			bubbler_tick(p);
			break;
		case PJ_GRENADE:
			if (game_tick_count >= p->die) {          /* GrenadeExplode */
				int32_t org[3] = { p->org[0], p->org[1], p->org[2] };
				p->type = PJ_FREE;
				t_radius_damage(org, 120, 1);
				explode_at(org);
				break;
			}
			proj_move(p);
			if (p->type == PJ_GRENADE && !p->onground)
				particle_trail(p->org, p->leaf, 1);
			break;
		default:                                      /* missiles */
			if (game_tick_count >= p->die) {
				p->type = PJ_FREE;
				break;
			}
			proj_move(p);
			if (p->type == PJ_ROCKET)
				particle_trail(p->org, p->leaf, 0);
			break;
		}
	}
	proj_top = top;
}

/* ---------------------------------------------------------------- damage -- */

/* PlayerDie: GibPlayer below -40, else DeathSound (under water: bubbles and
   the drowning death) */
static void player_die(void)
{
	if (pl.health < -40) {
		snd_start(ENT_PLAYER, CHAN_VOICE, (q_rand() & 0x7fff) < 0x4000 ? S_GIB : S_UDEATH,
		          pl.pm.pos, 255, ATTN_NONE);
	} else if (pl.pm.waterlevel == 3) {
		death_bubbles(20);
		snd_start(ENT_PLAYER, CHAN_VOICE, S_H2ODEATH, pl.pm.pos, 255, ATTN_NONE);
	} else {
		snd_start(ENT_PLAYER, CHAN_VOICE, S_DEATH1 + rand_rint(4) - 1, pl.pm.pos, 255, ATTN_NONE);
	}
	pl.dead       = 1;
	pl.death_tick = game_tick_count;
	pl.weaponframe = 0;
	pl.anim       = 0;
	pl.viewheight = -8 * SUB;                      /* the view drops to the floor */
	pl.pm.vel[0] = pl.pm.vel[1] = pl.pm.vel[2] = 0;
}

/* T_Damage on the player: armor absorbs its share, the rest is health;
   the kick and the pain face (V_ParseDamage); momentum from the inflictor */
static void t_damage(int damage, const int32_t inflictor[3])
{
	if (pl.dead)
		return;
	int save = (pl.armortype * damage + 99) / 100;        /* ceil */
	if (save >= pl.armor) {
		save = pl.armor;
		pl.armortype = 0;
		pl.items &= ~(IT_ARMOR1 | IT_ARMOR2 | IT_ARMOR3);
	}
	pl.armor -= save;
	int take = damage - save;
	if (take < 0) take = 0;

	/* the client's reaction: the face, the kick */
	pl.pain_until = game_tick_count + TICKS(20);
	int count = (take + save) / 2;
	if (count < 10) count = 10;
	if (inflictor) {
		int32_t from[3], d[3], f[3], r[3], u[3];
		for (int k = 0; k < 3; k++) d[k] = inflictor[k] - pl.pm.pos[k];
		vnormalize(d, from);
		view_vectors(pl.pm.yaw, pl.pm.pitch, f, r, u);
		int32_t side_r = (from[0] * r[0] + from[1] * r[1] + from[2] * r[2]) >> 12;
		int32_t side_f = (from[0] * f[0] + from[1] * f[1] + from[2] * f[2]) >> 12;
		pl.dmg_roll  = DEG(1) * count * side_r * 6 / 10 / 4096;   /* v_kickroll 0.6 */
		pl.dmg_pitch = DEG(1) * count * side_f * 6 / 10 / 4096;   /* v_kickpitch 0.6 */
		pl.dmg_time  = TICKS(50);                                  /* v_kicktime 0.5 */

		/* momentum: dir from the inflictor, x damage x 8 */
		int32_t dir[3];
		for (int k = 0; k < 3; k++) d[k] = pl.pm.pos[k] - inflictor[k];
		vnormalize(d, dir);
		for (int k = 0; k < 3; k++)
			pl.pm.vel[k] += dir[k] * damage * 8 / 4096;
		pl.pm.onground = 0;
	}
	pl.health -= take;
	if (pl.health <= 0) {
		player_die();
		return;
	}
	/* player_pain -> player_pain1 -> PainSound: not while the weapon
	   animates; the water, slime and lava pains every time (the drowning
	   is paced by WaterMove, the burns by their dmgtime); the rest one
	   every half second */
	if (pl.weaponframe)
		return;
	if (pl.pm.watertype == QW_CONTENTS_WATER && pl.pm.waterlevel == 3) {
		death_bubbles(1);
		snd_start(ENT_PLAYER, CHAN_VOICE, (q_rand() & 0x7fff) > 0x4000 ? S_DROWN1 : S_DROWN2,
		          pl.pm.pos, 255, ATTN_NORM);
		return;
	}
	if (pl.pm.watertype == QW_CONTENTS_SLIME || pl.pm.watertype == QW_CONTENTS_LAVA) {
		snd_start(ENT_PLAYER, CHAN_VOICE, (q_rand() & 0x7fff) > 0x4000 ? S_LBURN1 : S_LBURN2,
		          pl.pm.pos, 255, ATTN_NORM);
		return;
	}
	if (pl.pain_finished > game_tick_count)
		return;
	pl.pain_finished = game_tick_count + TICKS(50);
	snd_start(ENT_PLAYER, CHAN_VOICE, S_PAIN1 + rand_rint(5) - 1, pl.pm.pos, 255, ATTN_NORM);
}

/* T_RadiusDamage on the player (the only thing that takes damage) */
static void t_radius_damage(const int32_t org[3], int damage, int self_inflicted)
{
	if (pl.dead)
		return;
	int32_t centre[3] = { pl.pm.pos[0], pl.pm.pos[1], pl.pm.pos[2] + 4 * SUB };
	int32_t d[3];
	for (int k = 0; k < 3; k++) d[k] = org[k] - centre[k];
	int32_t dist = vlen(d) / SUB;
	if (dist > damage + 40)
		return;
	int points = damage - dist / 2;
	if (self_inflicted)
		points /= 2;
	if (points <= 0)
		return;
	trace_t tr;                                       /* CanDamage: a clear line */
	clip_trace(0, org, centre, &tr);
	if (tr.fraction < Q_FIX_ONE)
		return;
	t_damage(points, org);
}

/* --------------------------------------------------------------- weapons -- */

static const uint32_t weapon_item[W_COUNT] = {
	IT_AXE, IT_SHOTGUN, IT_SUPER_SHOTGUN, IT_NAILGUN, IT_SUPER_NAILGUN,
	IT_GRENADE_LAUNCHER, IT_ROCKET_LAUNCHER, IT_LIGHTNING,
};
static const int weapon_ammo[W_COUNT]  = { -1, AMMO_SHELLS, AMMO_SHELLS, AMMO_NAILS, AMMO_NAILS,
                                           AMMO_ROCKETS, AMMO_ROCKETS, AMMO_CELLS };
static const int weapon_needs[W_COUNT] = { 0, 1, 2, 1, 2, 1, 1, 1 };   /* ammo to select it */

int game_ammo_type(void)
{
	return weapon_ammo[pl.weapon];
}

int game_current_ammo(void)
{
	int a = weapon_ammo[pl.weapon];
	return a < 0 ? 0 : pl.ammo[a];
}

static int has_ammo_for(int w)
{
	int a = weapon_ammo[w];
	return a < 0 || pl.ammo[a] >= weapon_needs[w];
}

static int w_best_weapon(void)
{
	static const int order[] = { W_LIGHTNING, W_SNAILGUN, W_SSHOTGUN, W_NAILGUN, W_SHOTGUN };
	for (int i = 0; i < 5; i++)
		if ((pl.items & weapon_item[order[i]]) && has_ammo_for(order[i]))
			return order[i];
	return W_AXE;
}

/* W_SetCurrentAmmo: out of any firing state */
static void w_set_current(int w)
{
	if (w != pl.weapon)
		pl.weapon_switch_tick = game_tick_count;
	pl.weapon      = w;
	pl.weaponframe = 0;
	pl.anim        = 0;
}

static int w_has_no_ammo(void)
{
	if (pl.weapon == W_AXE || game_current_ammo())
		return 0;
	w_set_current(w_best_weapon());
	return 1;
}

/* the weapon animation chains (player.qc), one think every 0.1 s */
enum { AN_NONE, AN_SHOT, AN_AXE_A, AN_AXE_B, AN_NAIL, AN_LIGHT, AN_ROCKET };

static void w_fire_axe(void);
static void w_fire_spikes(int ox);
static void w_fire_lightning(void);

static void anim_start(int anim, int frame)
{
	pl.anim        = anim;
	pl.weaponframe = frame;
	pl.anim_next   = game_tick_count + TICKS(10);
	if (anim != AN_AXE_A && anim != AN_AXE_B)      /* player_shot1, nail1, light1, rocket1 */
		pl.muzzleflash = 1;
}

static void anim_tick(void)
{
	if (!pl.anim || game_tick_count < pl.anim_next)
		return;
	pl.anim_next += TICKS(10);
	switch (pl.anim) {
	case AN_SHOT:                                  /* player_shot1..6 */
	case AN_ROCKET:                                /* player_rocket1..6 */
		if (++pl.weaponframe > 6) { pl.anim = 0; pl.weaponframe = 0; }
		break;
	case AN_AXE_A:                                 /* 1 2 3(hit) 4 */
	case AN_AXE_B:                                 /* 5 6 7(hit) 8 */
		pl.weaponframe++;
		if (pl.weaponframe == 3 || pl.weaponframe == 7)
			w_fire_axe();
		if (pl.weaponframe == 4 || pl.weaponframe == 8) { pl.anim = 0; pl.weaponframe = 0; }
		break;
	case AN_NAIL:                                  /* player_nail1 / 2 while firing */
		if (!pl.fire) { pl.anim = 0; pl.weaponframe = 0; break; }
		pl.weaponframe = pl.weaponframe % 8 + 1;
		pl.muzzleflash = 1;                            /* player_nail1 / 2 */
		w_fire_spikes((pl.weaponframe & 1) ? 4 : -4);
		pl.attack_finished = game_tick_count + TICKS(20);
		break;
	case AN_LIGHT:                                 /* player_light1 / 2 while firing */
		if (!pl.fire) { pl.anim = 0; pl.weaponframe = 0; break; }
		pl.weaponframe = pl.weaponframe % 4 + 1;
		pl.muzzleflash = 1;                            /* player_light1 / 2 */
		w_fire_lightning();
		pl.attack_finished = game_tick_count + TICKS(20);
		break;
	default:
		pl.anim = 0;
		break;
	}
}

/* ---- the attacks (weapons.qc) ---- */

static void w_fire_axe(void)
{
	int32_t f[3], r[3], u[3], src[3], end[3];
	view_vectors(pl.pm.yaw, pl.pm.pitch, f, r, u);
	for (int k = 0; k < 3; k++) {
		src[k] = pl.pm.pos[k] + (k == 2 ? 16 * SUB : 0);
		end[k] = src[k] + 64 * SUB * f[k] / 4096;
	}
	trace_t tr;
	clip_trace(0, src, end, &tr);
	if (tr.fraction >= Q_FIX_ONE)
		return;
	int32_t org[3];                                 /* hit wall: TE_GUNSHOT */
	for (int k = 0; k < 3; k++)
		org[k] = tr.endpos[k] - 4 * SUB * f[k] / 4096;
	snd_start(ENT_PLAYER, CHAN_WEAPON, S_AXHIT2, pl.pm.pos, 255, ATTN_NORM);
	particle_effect(org, leaf_at(org), 0, 20);
}

/* FireBullets: `count` pellets from the eye, spread x4096 */
static void fire_bullets(int count, int32_t spread_x, int32_t spread_y)
{
	int32_t f[3], r[3], u[3], src[3];
	view_vectors(pl.pm.yaw, pl.pm.pitch, f, r, u);
	src[0] = pl.pm.pos[0] + 10 * SUB * f[0] / 4096;
	src[1] = pl.pm.pos[1] + 10 * SUB * f[1] / 4096;
	src[2] = pl.pm.pos[2] - 24 * SUB + 56 * SUB * 7 / 10;   /* absmin_z + size_z x 0.7 */
	while (count-- > 0) {
		int32_t dir[3], end[3];
		int32_t cx = crandom(4096) * spread_x >> 12, cy = crandom(4096) * spread_y >> 12;
		for (int k = 0; k < 3; k++) {
			dir[k] = f[k] + (cx * r[k] >> 12) + (cy * u[k] >> 12);
			end[k] = src[k] + (int32_t) ((int64_t) dir[k] * 2048 * SUB / 4096);
		}
		trace_t tr;
		clip_trace(0, src, end, &tr);
		if (tr.fraction >= Q_FIX_ONE)
			continue;
		int32_t org[3];                             /* TraceAttack: TE_GUNSHOT */
		for (int k = 0; k < 3; k++)
			org[k] = tr.endpos[k] - 4 * SUB * dir[k] / 4096;
		particle_effect(org, leaf_at(org), 0, 20);
	}
}

static void w_fire_shotgun(void)
{
	snd_start(ENT_PLAYER, CHAN_WEAPON, S_GUNCOCK, pl.pm.pos, 255, ATTN_NORM);
	pl.punch = -DEG(2);
	pl.ammo[AMMO_SHELLS]--;
	fire_bullets(6, 164, 164);                      /* 0.04 x 4096 */
}

static void w_fire_super_shotgun(void)
{
	if (pl.ammo[AMMO_SHELLS] == 1) {
		w_fire_shotgun();
		return;
	}
	snd_start(ENT_PLAYER, CHAN_WEAPON, S_SHOTGN2, pl.pm.pos, 255, ATTN_NORM);
	pl.punch = -DEG(4);
	pl.ammo[AMMO_SHELLS] -= 2;
	fire_bullets(14, 573, 328);                     /* 0.14, 0.08 x 4096 */
}

static void launch_spike(const int32_t org[3], const int32_t dir[3], int super)
{
	proj_t *p = proj_new(super ? PJ_SSPIKE : PJ_SPIKE);
	if (!p) return;
	for (int k = 0; k < 3; k++) {
		p->org[k] = org[k];
		p->vel[k] = dir[k] * 1000 / 4096;
	}
	vectoangles(p->vel, &p->yaw, &p->pitch);
	p->die  = game_tick_count + TICKS(600);
	p->leaf = leaf_at(p->org);
}

static void w_fire_spikes(int ox)
{
	int32_t f[3], r[3], u[3], org[3];
	view_vectors(pl.pm.yaw, pl.pm.pitch, f, r, u);
	if (pl.ammo[AMMO_NAILS] >= 2 && pl.weapon == W_SNAILGUN) {   /* W_FireSuperSpikes */
		snd_start(ENT_PLAYER, CHAN_WEAPON, S_SPIKE2, pl.pm.pos, 255, ATTN_NORM);
		pl.attack_finished = game_tick_count + TICKS(20);
		pl.ammo[AMMO_NAILS] -= 2;
		for (int k = 0; k < 3; k++)
			org[k] = pl.pm.pos[k] + (k == 2 ? 16 * SUB : 0);
		launch_spike(org, f, 1);
		pl.punch = -DEG(2);
		return;
	}
	if (pl.ammo[AMMO_NAILS] < 1) {
		w_set_current(w_best_weapon());
		return;
	}
	snd_start(ENT_PLAYER, CHAN_WEAPON, S_ROCKET1I, pl.pm.pos, 255, ATTN_NORM);
	pl.attack_finished = game_tick_count + TICKS(20);
	pl.ammo[AMMO_NAILS]--;
	for (int k = 0; k < 3; k++)
		org[k] = pl.pm.pos[k] + (k == 2 ? 16 * SUB : 0) + ox * SUB * r[k] / 4096;
	launch_spike(org, f, 0);
	pl.punch = -DEG(2);
}

static void w_fire_grenade(void)
{
	int32_t f[3], r[3], u[3];
	view_vectors(pl.pm.yaw, pl.pm.pitch, f, r, u);
	pl.ammo[AMMO_ROCKETS]--;
	snd_start(ENT_PLAYER, CHAN_WEAPON, S_GRENADE, pl.pm.pos, 255, ATTN_NORM);
	pl.punch = -DEG(2);
	proj_t *p = proj_new(PJ_GRENADE);
	if (!p) return;
	for (int k = 0; k < 3; k++) {
		p->org[k] = pl.pm.pos[k];
		if (pl.pm.pitch)
			p->vel[k] = f[k] * 600 / 4096 + u[k] * 200 / 4096 + crandom(10) * r[k] / 4096 + crandom(10) * u[k] / 4096;
		else
			p->vel[k] = f[k] * 600 / 4096 + (k == 2 ? 200 : 0);
	}
	vectoangles(p->vel, &p->yaw, &p->pitch);
	p->die  = game_tick_count + TICKS(250);
	p->leaf = leaf_at(p->org);
}

static void w_fire_rocket(void)
{
	int32_t f[3], r[3], u[3];
	view_vectors(pl.pm.yaw, pl.pm.pitch, f, r, u);
	pl.ammo[AMMO_ROCKETS]--;
	snd_start(ENT_PLAYER, CHAN_WEAPON, S_SGUN1, pl.pm.pos, 255, ATTN_NORM);
	pl.punch = -DEG(2);
	proj_t *p = proj_new(PJ_ROCKET);
	if (!p) return;
	for (int k = 0; k < 3; k++) {
		p->org[k] = pl.pm.pos[k] + 8 * SUB * f[k] / 4096 + (k == 2 ? 16 * SUB : 0);
		p->vel[k] = f[k] * 1000 / 4096;
	}
	vectoangles(p->vel, &p->yaw, &p->pitch);
	p->die  = game_tick_count + TICKS(500);
	p->leaf = leaf_at(p->org);
}

static void w_fire_lightning(void)
{
	if (pl.ammo[AMMO_CELLS] < 1) {
		w_set_current(w_best_weapon());
		return;
	}
	if (pl.pm.waterlevel > 1) {                       /* explode if under water */
		int cells = pl.ammo[AMMO_CELLS];
		pl.ammo[AMMO_CELLS] = 0;
		w_set_current(w_best_weapon());
		t_radius_damage(pl.pm.pos, 35 * cells, 1);
		return;
	}
	int32_t f[3], r[3], u[3], org[3], end[3];
	view_vectors(pl.pm.yaw, pl.pm.pitch, f, r, u);
	if (pl.t_width < game_tick_count) {
		snd_start(ENT_PLAYER, CHAN_WEAPON, S_LHIT, pl.pm.pos, 255, ATTN_NORM);
		pl.t_width = game_tick_count + TICKS(60);
	}
	pl.punch = -DEG(2);
	pl.ammo[AMMO_CELLS]--;
	for (int k = 0; k < 3; k++) {
		org[k] = pl.pm.pos[k] + (k == 2 ? 16 * SUB : 0);
		end[k] = org[k] + 600 * SUB * f[k] / 4096;
	}
	trace_t tr;
	clip_trace(0, org, end, &tr);
	/* TE_LIGHTNING2: the beam, 0.2 s; one per player (replaced) */
	proj_t *b = 0;
	for (int i = 0; i < proj_top && !b; i++)
		if (projs[i].type == PJ_BEAM)
			b = &projs[i];
	if (!b)
		b = proj_new(PJ_BEAM);
	if (b) {
		b->type = PJ_BEAM;
		for (int k = 0; k < 3; k++) {
			b->org[k] = org[k];
			b->end[k] = tr.endpos[k];
		}
		b->leaf = leaf_at(org);
		b->die  = game_tick_count + TICKS(20);
	}
	if (tr.fraction < Q_FIX_ONE)                     /* sparks where it hits */
		particle_effect(tr.endpos, leaf_at(tr.endpos), 225, 4);
}

/* W_Attack */
static void w_attack(void)
{
	if (w_has_no_ammo())
		return;
	switch (pl.weapon) {
	case W_AXE:
		snd_start(ENT_PLAYER, CHAN_WEAPON, S_AX1, pl.pm.pos, 255, ATTN_NORM);
		anim_start((q_rand() & 1) ? AN_AXE_A : AN_AXE_B, (q_rand() & 1) ? 1 : 5);
		if (pl.anim == AN_AXE_A) pl.weaponframe = 1; else pl.weaponframe = 5;
		pl.attack_finished = game_tick_count + TICKS(50);
		break;
	case W_SHOTGUN:
		anim_start(AN_SHOT, 1);
		w_fire_shotgun();
		pl.attack_finished = game_tick_count + TICKS(50);
		break;
	case W_SSHOTGUN:
		anim_start(AN_SHOT, 1);
		w_fire_super_shotgun();
		pl.attack_finished = game_tick_count + TICKS(70);
		break;
	case W_NAILGUN:
	case W_SNAILGUN:                                /* player_nail1: fires at once */
		anim_start(AN_NAIL, 1);
		w_fire_spikes(4);
		pl.attack_finished = game_tick_count + TICKS(20);
		break;
	case W_GLAUNCHER:
		anim_start(AN_ROCKET, 1);
		w_fire_grenade();
		pl.attack_finished = game_tick_count + TICKS(60);
		break;
	case W_RLAUNCHER:
		anim_start(AN_ROCKET, 1);
		w_fire_rocket();
		pl.attack_finished = game_tick_count + TICKS(80);
		break;
	case W_LIGHTNING:                               /* player_light1: fires at once */
		anim_start(AN_LIGHT, 1);
		w_fire_lightning();
		pl.attack_finished = game_tick_count + TICKS(20);
		snd_start(ENT_PLAYER, CHAN_AUTO, S_LSTART, pl.pm.pos, 255, ATTN_NORM);
		break;
	}
}

/* W_ChangeWeapon: the weapon, if owned with ammo */
static void w_change(int w)
{
	if (!(pl.items & weapon_item[w]) || !has_ammo_for(w))
		return;
	w_set_current(w);
}

/* CycleWeaponCommand / CycleWeaponReverseCommand */
static void w_cycle(int dir)
{
	for (int n = 1; n < W_COUNT; n++) {
		int w = (pl.weapon + dir * n + W_COUNT) % W_COUNT;
		if ((pl.items & weapon_item[w]) && has_ammo_for(w)) {
			w_set_current(w);
			return;
		}
	}
}

/* the d-pad's groups: the next owned weapon of the group after the current */
static void w_group(int g)
{
	static const int groups[4][3] = {
		{ W_SHOTGUN, W_SSHOTGUN, W_AXE }, { W_NAILGUN, W_SNAILGUN, -1 },
		{ W_GLAUNCHER, W_RLAUNCHER, -1 }, { W_LIGHTNING, -1, -1 },
	};
	int start = 0;
	for (int i = 0; i < 3; i++)
		if (groups[g][i] == pl.weapon)
			start = i + 1;
	for (int n = 0; n < 3; n++) {
		int w = groups[g][(start + n) % 3];
		if (w >= 0 && (pl.items & weapon_item[w]) && has_ammo_for(w)) {
			if (w != pl.weapon)
				w_set_current(w);
			return;
		}
	}
}

/* W_WeaponFrame */
static void w_weapon_frame(const game_cmd_t *cmd)
{
	if (game_tick_count < pl.attack_finished)
		return;
	if (cmd->weapon_next) w_cycle(1);
	if (cmd->weapon_prev) w_cycle(-1);
	if (cmd->weapon_group >= 0) w_group(cmd->weapon_group);
	if (pl.fire)
		w_attack();
}

/* ---------------------------------------------------------------- player -- */

void game_init(void)
{
	memset(&pl, 0, sizeof pl);
	memset(projs, 0, sizeof projs);
	proj_top = 0;
	particles_clear();
	game_tick_count = 0;
}

/* at the spawn point, with everything (CheatCommand) until items exist */
void game_spawn(void)
{
	memset(&pl.pm, 0, sizeof pl.pm);
	pl.pm.pos[0] = world.spawn[0] * SUB;
	pl.pm.pos[1] = world.spawn[1] * SUB;
	pl.pm.pos[2] = (world.spawn[2] + 1) * SUB;     /* Quake drops it to the floor */
	pl.pm.yaw    = world.spawn_yaw * ANG_TURN / 360;
	pl.health    = 100;
	pl.armor     = 200;                             /* mal: full armor too */
	pl.armortype = 80;
	pl.items     = IT_AXE | IT_SHOTGUN | IT_SUPER_SHOTGUN | IT_NAILGUN | IT_SUPER_NAILGUN |
	               IT_GRENADE_LAUNCHER | IT_ROCKET_LAUNCHER | IT_LIGHTNING | IT_ARMOR3 |
	               IT_KEY1 | IT_KEY2;
	pl.ammo[AMMO_SHELLS]  = 100;
	pl.ammo[AMMO_NAILS]   = 200;
	pl.ammo[AMMO_ROCKETS] = 100;
	pl.ammo[AMMO_CELLS]   = 200;
	pl.weapon      = W_SHOTGUN;
	pl.weaponframe = 0;
	pl.anim        = 0;
	pl.attack_finished = game_tick_count;
	pl.punch = pl.dmg_time = pl.dmg_roll = pl.dmg_pitch = 0;
	pl.pain_until  = 0;
	pl.pain_finished = pl.t_width = pl.jump_flag = 0;
	pl.air_finished = game_tick_count + TICKS(1200);  /* PutClientInServer: 12 s of air */
	pl.dmgtime = pl.dmg = pl.inwater = pl.swim_flag = 0;
	pl.dead        = 0;
	pl.viewheight  = PM_EYE_HEIGHT * SUB;
	pl.weapon_switch_tick = game_tick_count - TICKS(200);
	pl.muzzleflash = 0;
	memset(projs, 0, sizeof projs);
	proj_top = 0;
	particles_clear();
	dl_clear();
}

/* client.qc WaterMove: 12 s of air, then the drowning (2, 4, .. 10 a
   second); the lava's and slime's burns; the sounds in and out (the drag
   on the velocity is pmove_step's) */
static void player_water(void)
{
	int now = game_tick_count, wl = pl.pm.waterlevel, wt = pl.pm.watertype;
	if (pl.pm.noclip || pl.health < 0)
		return;
	if (wl != 3) {
		if (pl.air_finished < now)
			snd_start(ENT_PLAYER, CHAN_VOICE, S_GASP2, pl.pm.pos, 255, ATTN_NORM);
		else if (pl.air_finished < now + TICKS(900))
			snd_start(ENT_PLAYER, CHAN_VOICE, S_GASP1, pl.pm.pos, 255, ATTN_NORM);
		pl.air_finished = now + TICKS(1200);
		pl.dmg = 2;
	} else if (pl.air_finished < now) {               /* drown! */
		if (pl.pain_finished < now) {
			pl.dmg += 2;
			if (pl.dmg > 15)
				pl.dmg = 10;
			t_damage(pl.dmg, 0);
			pl.pain_finished = now + TICKS(100);
		}
	}
	if (!wl) {
		if (pl.inwater) {                             /* leave water */
			snd_start(ENT_PLAYER, CHAN_BODY, S_OUTWATER, pl.pm.pos, 255, ATTN_NORM);
			pl.inwater = 0;
		}
		return;
	}
	if (wt == QW_CONTENTS_LAVA) {
		if (pl.dmgtime < now) {                       /* (no envirosuit yet) */
			pl.dmgtime = now + TICKS(20);
			t_damage(10 * wl, 0);
		}
	} else if (wt == QW_CONTENTS_SLIME) {
		if (pl.dmgtime < now) {
			pl.dmgtime = now + TICKS(100);
			t_damage(4 * wl, 0);
		}
	}
	if (!pl.inwater) {                                /* enter water */
		snd_start(ENT_PLAYER, CHAN_BODY, wt == QW_CONTENTS_LAVA ? S_INLAVA : wt == QW_CONTENTS_SLIME ? S_SLIMBRN2 : S_INH2O,
		          pl.pm.pos, 255, ATTN_NORM);
		pl.inwater = 1;
		pl.dmgtime = 0;
	}
}

/* client.qc CheckWaterJump: at the surface, a wall 24 units ahead at the
   waist and nothing at the eyes is a ledge: the push away from the wall
   (movedir) and the jump out go through pmove_step this tick */
static void check_water_jump(pmove_cmd_t *pc)
{
	int32_t sy = r_sin(pl.pm.yaw), cy = r_cos(pl.pm.yaw);
	int32_t start[3] = { pl.pm.pos[0], pl.pm.pos[1], pl.pm.pos[2] + 8 * SUB };
	int32_t end[3]   = { start[0] + 24 * SUB * cy / 4096, start[1] + 24 * SUB * sy / 4096, start[2] };
	trace_t tr;
	clip_trace(0, start, end, &tr);
	if (tr.fraction < Q_FIX_ONE) {                    /* solid at waist */
		start[2] = end[2] = start[2] + (PM_MAXS_Z - 8) * SUB;
		pl.pm.movedir[0] = -50 * tr.plane_n[0] / 4096;
		pl.pm.movedir[1] = -50 * tr.plane_n[1] / 4096;
		clip_trace(0, start, end, &tr);
		if (tr.fraction >= Q_FIX_ONE)                 /* open at eye level */
			pc->waterjump = 1;
	}
}

void game_tick(const game_cmd_t *cmd)
{
	game_tick_count++;
	pl.fire_was = pl.fire;
	pl.fire     = cmd->fire;

	if (pl.dead) {
		/* respawn on fire after a second (the body stays where it fell) */
		if (pl.fire && !pl.fire_was && game_tick_count > pl.death_tick + TICKS(100))
			game_spawn();
	} else {
		pmove_cmd_t pc;
		pc.fwd  = cmd->fwd;
		pc.side = cmd->side;
		pc.up   = 0;
		pc.jump = cmd->jump;
		pc.walk = cmd->walk;
		pc.waterjump = 0;
		if (cmd->centre)
			pl.pm.pitch = 0;
		/* PlayerPreThink: WaterMove, CheckWaterJump, PlayerJump's sounds (the
		   velocities they set go through pmove_step, in Quake's order) */
		player_water();
		if (pl.dead)
			goto dead;
		if (pl.pm.waterlevel == 2)
			check_water_jump(&pc);
		if (pc.jump && !pc.waterjump && !pl.pm.waterjump) {
			if (pl.pm.waterlevel >= 2) {
				if (pl.swim_flag < game_tick_count) {   /* the swimming sound */
					pl.swim_flag = game_tick_count + TICKS(100);
					snd_start(ENT_PLAYER, CHAN_BODY, (q_rand() & 0x7fff) < 0x4000 ? S_WATER1 : S_WATER2,
					          pl.pm.pos, 255, ATTN_NORM);
				}
			} else if (pl.pm.onground && pl.pm.jumpreleased) {
				snd_start(ENT_PLAYER, CHAN_BODY, S_PLYRJMP8, pl.pm.pos, 255, ATTN_NORM);
			}
		}
		pl.jump_flag = pl.pm.vel[2];
		pmove_step(&pl.pm, &pc);
		w_weapon_frame(cmd);
		anim_tick();
		/* PlayerPostThink: landed (in the water: the splash) */
		if (pl.jump_flag < -300 && pl.pm.onground && pl.health > 0) {
			if (pl.pm.watertype == QW_CONTENTS_WATER) {
				snd_start(ENT_PLAYER, CHAN_BODY, S_H2OJUMP, pl.pm.pos, 255, ATTN_NORM);
			} else if (pl.jump_flag < -650) {
				t_damage(5, 0);
				snd_start(ENT_PLAYER, CHAN_VOICE, S_LAND2, pl.pm.pos, 255, ATTN_NORM);
			} else {
				snd_start(ENT_PLAYER, CHAN_VOICE, S_LAND, pl.pm.pos, 255, ATTN_NORM);
			}
			pl.jump_flag = 0;
		}
	dead:;
	}
	projs_tick();
	particles_tick();
	dl_decay(game_tick_count);

	/* the view's kicks decay: punchangle 10 degrees per second */
	if (pl.punch < 0) { pl.punch += DEG(10) / GAME_HZ; if (pl.punch > 0) pl.punch = 0; }
	if (pl.punch > 0) { pl.punch -= DEG(10) / GAME_HZ; if (pl.punch < 0) pl.punch = 0; }
	if (pl.dmg_time > 0)
		pl.dmg_time--;
}

/* ------------------------------------------------------------------ view -- */

/* V_CalcBob, in 1/SUB units: cl_bob 0.02, cycle 0.6 s, up 0.5 */
static int calc_bob(void)
{
	int32_t speed = (int32_t) q_isqrt64((uint64_t) ((int64_t) pl.pm.vel[0] * pl.pm.vel[0] +
	                                               (int64_t) pl.pm.vel[1] * pl.pm.vel[1]));
	int32_t bob = speed * SUB * 2 / 100;                    /* units x 0.02, in 1/SUB */
	int     cycle = game_tick_count % TICKS(60);
	int32_t s = r_sin(cycle * ANG_TURN / TICKS(60));
	bob = bob * 3 / 10 + (bob * 7 / 10 * s >> 12);
	if (bob > 4 * SUB) bob = 4 * SUB;
	if (bob < -7 * SUB) bob = -7 * SUB;
	return bob;
}

/* V_CalcRoll: the lean when moving sideways, cl_rollangle 2 at 200 u/s */
static int calc_roll(void)
{
	int32_t f[3], r[3], u[3];
	view_vectors(pl.pm.yaw, 0, f, r, u);
	int32_t side = (pl.pm.vel[0] * r[0] + pl.pm.vel[1] * r[1]) >> 12;
	int sign = side < 0 ? -1 : 1;
	if (side < 0) side = -side;
	if (side > 200) side = 200;
	return sign * side * DEG(2) / 200;
}

void game_view(int32_t pos[3], int *yaw, int *pitch, int *roll)
{
	int bob = pl.dead ? 0 : calc_bob();
	pos[0] = pl.pm.pos[0];
	pos[1] = pl.pm.pos[1];
	pos[2] = pl.pm.pos[2] + pl.viewheight + bob;
	*yaw   = pl.pm.yaw;
	*pitch = pl.pm.pitch + pl.punch;
	*roll  = pl.dead ? DEG(80) : calc_roll();
	if (pl.dmg_time > 0) {
		*roll  += pl.dmg_roll * pl.dmg_time / TICKS(50);
		*pitch += pl.dmg_pitch * pl.dmg_time / TICKS(50);
	}
}

/* ---------------------------------------------------------------- render -- */

static const int weapon_model[W_COUNT] = { QGM_V_AXE, QGM_V_SHOT, QGM_V_SHOT2, QGM_V_NAIL,
                                           QGM_V_NAIL2, QGM_V_ROCK, QGM_V_ROCK2, QGM_V_LIGHT };

void game_render(void)
{
	rl_begin();
	r_state.viewmodel = 0;
	if (!gdata.ok)
		return;

	/* the dynamic lights (CL_RelinkEntities): the muzzle flash, 16 up and
	   18 forward, for 0.1 s; each rocket's, for the frame; then the world
	   lit (R_PushDlights) */
	if (pl.muzzleflash && !pl.dead) {
		int32_t f[3], r[3], u[3];
		view_vectors(pl.pm.yaw, 0, f, r, u);
		dlight_t *dl = dl_alloc(ENT_PLAYER, game_tick_count);
		for (int k = 0; k < 3; k++)
			dl->origin[k] = pl.pm.pos[k] + (k == 2 ? 16 * SUB : 0) + 18 * SUB * f[k] / 4096;
		dl->radius   = 200 + (int) (q_rand() & 31);
		dl->minlight = 32;
		dl->die      = game_tick_count + TICKS(10);
	}
	pl.muzzleflash = 0;
	for (int i = 0; i < proj_top; i++)
		if (projs[i].type == PJ_ROCKET) {
			dlight_t *dl = dl_alloc(ENT_PROJ + i, game_tick_count);
			for (int k = 0; k < 3; k++) dl->origin[k] = projs[i].org[k];
			dl->radius = 200;
			dl->die    = game_tick_count + 1;
		}
	uint32_t t0 = prof_now();
	dl_push(game_tick_count);
	uint32_t t1 = prof_now();
	prof_add(PROF_DLIGHT, t0, t1);

	/* the view model: at the eye, bobbing (V_CalcRefdef), lit by the eye's
	   leaf; a little lower with the full-screen viewport than with the
	   status bar's (Quake's viewsize fudge) */
	if (!pl.dead) {
		int32_t f[3], r[3], u[3], org[3];
		int bob = calc_bob();
		view_vectors(pl.pm.yaw, pl.pm.pitch, f, r, u);
		for (int k = 0; k < 3; k++)
			org[k] = pl.pm.pos[k] + f[k] * bob * 4 / 10 / 4096;
		org[2] += pl.viewheight + bob + (r_viewport_rows() < 240 ? 2 * SUB : 0);
		const qw_leaf *leaf = leaf_at(org);
		int light = leaf ? leaf->light : 128;
		if (light < 24) light = 24;                   /* always some light on the gun */
		light += dl_add_at(org, game_tick_count) * 3 / 2;   /* (x the cook's light gain) */
		r_ent_t *e = rl_model(&gdata.models[weapon_model[pl.weapon]], pl.weaponframe, org,
		                      pl.pm.yaw, -pl.pm.pitch, 0, light);
		rl_view_model(e);                /* no subdivision; clipped at 5 units, like Quake */
		r_state.viewmodel = (uint32_t) e;
	}

	/* projectiles and effects, in their leaves */
	for (int i = 0; i < proj_top; i++) {
		proj_t *p = &projs[i];
		if (p->type == PJ_FREE || !p->leaf)
			continue;
		int light = p->leaf->light + dl_add_at(p->org, game_tick_count) * 3 / 2;
		switch (p->type) {
		case PJ_SPIKE:
			rl_add_model(p->leaf, rl_model(&gdata.models[QGM_SPIKE], 0, p->org, p->yaw, p->pitch, 0, light));
			break;
		case PJ_SSPIKE:
			rl_add_model(p->leaf, rl_model(&gdata.models[QGM_S_SPIKE], 0, p->org, p->yaw, p->pitch, 0, light));
			break;
		case PJ_GRENADE:
			rl_add_model(p->leaf, rl_model(&gdata.models[QGM_GRENADE], 0, p->org, p->yaw, p->pitch, p->roll, light));
			break;
		case PJ_ROCKET:
			rl_add_model(p->leaf, rl_model(&gdata.models[QGM_MISSILE], 0, p->org, p->yaw, p->pitch, 0, light));
			break;
		case PJ_EXPLOSION:
			rl_add_model(p->leaf, rl_model(&gdata.models[QGM_S_EXPLOD], p->frame, p->org, 0, 0, 0, 128));
			break;
		case PJ_BUBBLE:
			rl_add_model(p->leaf, rl_model(&gdata.models[QGM_S_BUBBLE], p->frame, p->org, 0, 0, 0, light));
			break;
		case PJ_BUBBLER:
			break;
		case PJ_BEAM: {
			/* CL_UpdateTEnts: a bolt every 60 units (the cooked bolt is
			   twice Quake's 30), each rolled at random */
			int32_t d[3], dir[3];
			for (int k = 0; k < 3; k++) d[k] = p->end[k] - p->org[k];
			int yaw, pitch;
			vectoangles(d, &yaw, &pitch);
			vnormalize(d, dir);
			int32_t len = vlen(d), at = 0, org[3];
			for (int k = 0; k < 3; k++) org[k] = p->org[k];
			while (at < len) {
				const qw_leaf *leaf = leaf_at(org);
				rl_add_model(leaf, rl_model(&gdata.models[QGM_BOLT], 0, org, yaw, pitch,
				                            (int) (q_rand() & 4095), 128));
				for (int k = 0; k < 3; k++)
					org[k] += 60 * SUB * dir[k] / 4096;
				at += 60 * SUB;
			}
			break;
		}
		default:
			break;
		}
	}
	for (int i = part_active; i; i = particles[i - 1].next) {   /* (R_DrawParticles' order) */
		const particle_t *p = &particles[i - 1];
		rl_add_particle(p->leaf, p->org, p->colour);
	}
}

/* V_SetContentsColor: cshift_water, cshift_slime (solid too), cshift_lava
   by the view point's contents, a 50 % blend (Quake's 128 / 255 for the
   water; the slime's and lava's 150 are shown at 50 % too) */
void game_view_tint(void)
{
	if (!r_state.view_leaf)
		return;
	const qw_leaf *l = (const qw_leaf *) (r_state.view_leaf | 0x80000000u);
	switch (l->contents) {
	case QW_CONTENTS_WATER: vid_tint(130, 80, 50); break;
	case QW_CONTENTS_SOLID:
	case QW_CONTENTS_SLIME: vid_tint(0, 25, 5);    break;
	case QW_CONTENTS_LAVA:  vid_tint(255, 80, 0);  break;
	default: break;
	}
}

void game_render_done(void)
{
	uint32_t t0 = prof_now();
	rl_end();
	dl_restore();                          /* the world's light bytes back */
	prof_add(PROF_DLIGHT, t0, prof_now());
}
