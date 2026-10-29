/*
 * q1psx — the edicts' functions by number (docs/porting-notes.md §90.5):
 * an edict's think, think1, use, touch, blocked, th_die and th_pain are a
 * byte each, an index into ent_fns[] (0: none), where the QC keeps a
 * function pointer a word each. Every function an edict slot ever holds
 * is in ENT_FN_LIST, in its class file (no longer static: ent_fns.c names
 * them all); EF_<name> is its number. Call one with ENT_CALL (ents.h).
 */

#ifndef Q1PSX_ENT_FNS_H
#define Q1PSX_ENT_FNS_H

#define ENT_FN_LIST(X) \
	X(ai_found_target) \
	X(ammo_touch) \
	X(armor_touch) \
	X(army_die) \
	X(army_pain) \
	X(backpack_touch) \
	X(barrel_explode) \
	X(boss_awake) \
	X(button_blocked) \
	X(button_done) \
	X(button_killed) \
	X(button_return) \
	X(button_touch) \
	X(button_use) \
	X(button_wait) \
	X(changelevel_touch) \
	X(counter_use) \
	X(delay_think) \
	X(demon_die) \
	X(demon_jump_touch) \
	X(demon_pain) \
	X(dog_die) \
	X(dog_jump_touch) \
	X(dog_pain) \
	X(door_blocked) \
	X(door_go_down) \
	X(door_hit_bottom) \
	X(door_hit_top) \
	X(door_killed) \
	X(door_touch) \
	X(door_trigger_touch) \
	X(door_use) \
	X(execute_changelevel) \
	X(fd_secret_done) \
	X(fd_secret_move1) \
	X(fd_secret_move2) \
	X(fd_secret_move3) \
	X(fd_secret_move4) \
	X(fd_secret_move5) \
	X(fd_secret_move6) \
	X(fd_secret_use) \
	X(fire_fly) \
	X(fire_touch) \
	X(flymonster_start_go) \
	X(func_train_find) \
	X(func_wall_use) \
	X(health_touch) \
	X(hurt_on) \
	X(hurt_touch) \
	X(item_megahealth_rot) \
	X(key_touch) \
	X(knight_die) \
	X(knight_pain) \
	X(lightning_fire) \
	X(lightning_use) \
	X(link_doors) \
	X(make_bubbles) \
	X(monster_think) \
	X(monster_use) \
	X(multi_killed) \
	X(multi_touch) \
	X(multi_use) \
	X(multi_wait) \
	X(ogre_die) \
	X(ogre_pain) \
	X(place_item) \
	X(plat_center_touch) \
	X(plat_crush) \
	X(plat_go_down) \
	X(plat_hit_bottom) \
	X(plat_hit_top) \
	X(plat_trigger_use) \
	X(plat_use) \
	X(play_teleport) \
	X(powerup_touch) \
	X(secret_blocked) \
	X(secret_touch) \
	X(sham_die) \
	X(sham_pain) \
	X(shooter_think) \
	X(sigil_touch) \
	X(spikeshooter_use) \
	X(sub_calc_move_done) \
	X(sub_null) \
	X(sub_remove) \
	X(sub_use_targets) \
	X(tdeath_touch) \
	X(teleport_touch) \
	X(teleport_use) \
	X(train_blocked) \
	X(train_next) \
	X(train_use) \
	X(train_wait) \
	X(trigger_monsterjump_touch) \
	X(trigger_onlyregistered_touch) \
	X(trigger_push_touch) \
	X(trigger_skill_touch) \
	X(walkmonster_start_go) \
	X(weapon_touch) \
	X(wiz_die) \
	X(wiz_fast_fire) \
	X(wiz_pain) \
	X(zombie_die) \
	X(zombie_pain)

enum {
	EF_NONE,
#define X(f) EF_##f,
	ENT_FN_LIST(X)
#undef X
	EF_COUNT
};

_Static_assert(EF_COUNT <= 256, "ent_fns.h: an edict slot is a byte");

extern const ent_fn ent_fns[EF_COUNT];   /* [EF_NONE] = 0 */

#endif
