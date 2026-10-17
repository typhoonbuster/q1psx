# q1psx — rl_ent_matrix: an entity's GTE matrices from the view's (engine/
# rlist.c rl_model). C ABI:
#
#   void rl_ent_matrix(r_ent_t *e, const int16_t cols[3][4], const int16_t o[4]);
#
# The GTE's rotation and translation are loaded with the view's
# (r_state.rot, r_state.tr: r_render loads them again). e->tr = tr + (rot x o
# >> 12), o the entity's origin in vertex units (MVMVA with the translation:
# (tr x 4096 + rot . o) >> 12 is exactly tr + (rot . o >> 12)). When cols
# is not 0, e->rot = rot x M with M the model-to-world matrix given by
# columns (each x, y, z, pad: a word and a halfword), column j of the
# product being rot x cols[j] >> 12 (MVMVA, sf 1, no translation): the
# sum's arithmetic shift, as the C computes it; no saturation, the entries
# being within 4096 x 3. rot[9] is zeroed. Clobbers t0-t6.

	.set noreorder
	.set noat

	.include "gtereg.inc"
	.include "inline_s.inc"
	.include "r_defs.inc"

	.section .text.rl_ent_matrix, "ax", @progbits
	.global rl_ent_matrix
	.type rl_ent_matrix, @function
rl_ent_matrix:
	lui     $t0, %hi(r_state)
	addiu   $t0, $t0, %lo(r_state)
	lw      $t1, RS_ROT+0($t0)
	lw      $t2, RS_ROT+4($t0)
	lw      $t3, RS_ROT+8($t0)
	lw      $t4, RS_ROT+12($t0)
	lw      $t5, RS_ROT+16($t0)
	ctc2    $t1, C2_R11R12
	ctc2    $t2, C2_R13R21
	ctc2    $t3, C2_R22R23
	ctc2    $t4, C2_R31R32
	ctc2    $t5, C2_R33
	lw      $t1, RS_TR+0($t0)
	lw      $t2, RS_TR+4($t0)
	lw      $t3, RS_TR+8($t0)
	ctc2    $t1, C2_TRX
	ctc2    $t2, C2_TRY
	ctc2    $t3, C2_TRZ
	lwc2    C2_VXY0, 0($a2)              # o (x | y << 16; z from a halfword,
	lh      $t4, 4($a2)                  # sign-extended: see r_world.s)
	beqz    $a1, 2f                      # no columns: the translation only
	mtc2    $t4, C2_VZ0
	lwc2    C2_VXY1, 0($a1)              # columns 0, 1 in V1, V2
	lh      $t5, 4($a1)
	lwc2    C2_VXY2, 8($a1)
	lh      $t6, 12($a1)
	mtc2    $t5, C2_VZ1
	mtc2    $t6, C2_VZ2
	nop
	nop
	MVMVA   1, 0, 1, 3, 0                # IR = rot x column 0 >> 12
	lh      $t4, 20($a1)                 # (column 2's z meanwhile)
	nop
	nop
	mfc2    $t1, C2_IR1
	mfc2    $t2, C2_IR2
	mfc2    $t3, C2_IR3
	MVMVA   1, 0, 2, 3, 0                # column 1
	lwc2    C2_VXY0, 16($a1)             # column 2 in V0 (not this command's
	mtc2    $t4, C2_VZ0                  # input; o is read back below)
	sh      $t1, RE_ROT+0($a0)           # rot[0], [3], [6]
	sh      $t2, RE_ROT+6($a0)
	sh      $t3, RE_ROT+12($a0)
	mfc2    $t1, C2_IR1
	mfc2    $t2, C2_IR2
	mfc2    $t3, C2_IR3
	MVMVA   1, 0, 0, 3, 0                # column 2
	sh      $t1, RE_ROT+2($a0)           # rot[1], [4], [7]
	sh      $t2, RE_ROT+8($a0)
	sh      $t3, RE_ROT+14($a0)
	lh      $t4, 4($a2)                  # (o's z meanwhile)
	mfc2    $t1, C2_IR1
	mfc2    $t2, C2_IR2
	mfc2    $t3, C2_IR3
	lwc2    C2_VXY0, 0($a2)              # o again
	mtc2    $t4, C2_VZ0
	sh      $t1, RE_ROT+4($a0)           # rot[2], [5], [8]
	sh      $t2, RE_ROT+10($a0)
	sh      $t3, RE_ROT+16($a0)
	sh      $zero, RE_ROT+18($a0)        # rot[9]
1:	MVMVA   1, 0, 0, 0, 0                # MAC = (tr x 4096 + rot . o) >> 12
	nop
	nop
	mfc2    $t1, C2_MAC1
	mfc2    $t2, C2_MAC2
	mfc2    $t3, C2_MAC3
	sw      $t1, RE_TR+0($a0)
	sw      $t2, RE_TR+4($a0)
	jr      $ra
	sw      $t3, RE_TR+8($a0)
2:	nop                                  # (two apart from the V0 load)
	b       1b
	nop
