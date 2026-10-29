# q1psx — mdl_expand: a streamed model's frame (formats/qstream.h: one word
# a vertex, x | z << 8 | y << 16 | normal << 24) into the renderer's qw_vert
# pool (engine/mdl.h). C ABI:
#
#   void mdl_expand(qw_vert *dst, const uint32_t *src, int n);
#   void mdl_expand_shift(qw_vert *dst, const uint32_t *src, int n, int shift);
#   void mdl_lod_copy(qw_vert *dst, const uint16_t *par, int n, const qw_vert *pool);
#   void mdl_frame_avg(uint32_t *dst, const uint32_t *a, const uint32_t *b, int n);
#
# Word w gives x | y << 16 = w & 0x00ff00ff and z | normal << 16 =
# (w >> 8) & 0x00ff00ff: two ANDs and a shift, two stores. Two vertices a
# turn (n odd: the last one apart). The shift's variant (a big model: its
# steps doubled or more, its matrix within the GTE's range) shifts x, y and
# z left, the normal kept. mdl_lod_copy (a level of detail's band, formats/
# qstream.h QSTM_LOD): n collapsed vertices from dst on, each its parent's
# two words (par[i] the parent's index in the pool, lower than its own: in
# order, the parent's are final). mdl_frame_avg (a frame its neighbours
# make, formats/qstream.h QST_LERP): n words, each byte the floor of a's
# and b's average ((a & b) + ((a ^ b) & 0xfefefefe) / 2: no carry between
# the bytes), the normal's byte a's. Clobbers t0-t7.

	.set noreorder
	.set noat

	.section .text.mdl_expand, "ax", @progbits
	.global mdl_expand
	.type mdl_expand, @function
mdl_expand:
	lui     $t0, 0x00ff
	addiu   $at, $a2, -2
	bltz    $at, 2f                      # fewer than two
	ori     $t0, $t0, 0x00ff             # (0x00ff00ff)
1:	lw      $t1, 0($a1)
	lw      $t3, 4($a1)
	srl     $t2, $t1, 8
	and     $t1, $t1, $t0                # x | y << 16
	and     $t2, $t2, $t0                # z | normal << 16
	srl     $t4, $t3, 8
	and     $t3, $t3, $t0
	and     $t4, $t4, $t0
	sw      $t1, 0($a0)
	sw      $t2, 4($a0)
	sw      $t3, 8($a0)
	sw      $t4, 12($a0)
	addiu   $a2, $a2, -2
	addiu   $at, $a2, -2
	addiu   $a1, $a1, 8
	bgez    $at, 1b
	addiu   $a0, $a0, 16
2:	blez    $a2, 3f                      # the last one, n odd
	lw      $t1, 0($a1)                  # (delay; n even: a harmless read)
	nop
	srl     $t2, $t1, 8
	and     $t1, $t1, $t0
	and     $t2, $t2, $t0
	sw      $t1, 0($a0)
	sw      $t2, 4($a0)
3:	jr      $ra
	nop

	.section .text.mdl_expand_shift, "ax", @progbits
	.global mdl_expand_shift
	.type mdl_expand_shift, @function
mdl_expand_shift:
	lui     $t0, 0x00ff
	ori     $t0, $t0, 0x00ff             # x | y << 16's mask
	blez    $a2, 2f
	lui     $t5, 0x00ff                  # the normal's (0x00ff0000)
1:	lw      $t1, 0($a1)
	addiu   $a1, $a1, 4
	srl     $t2, $t1, 8
	and     $t1, $t1, $t0
	sllv    $t1, $t1, $a3                # x, y shifted (each in its half)
	andi    $t3, $t2, 0xff
	sllv    $t3, $t3, $a3                # z shifted
	and     $t2, $t2, $t5                # normal << 16
	or      $t2, $t2, $t3
	sw      $t1, 0($a0)
	sw      $t2, 4($a0)
	addiu   $a2, $a2, -1
	bgtz    $a2, 1b
	addiu   $a0, $a0, 8
2:	jr      $ra
	nop

	.section .text.mdl_frame_avg, "ax", @progbits
	.global mdl_frame_avg
	.type mdl_frame_avg, @function
mdl_frame_avg:
	lui     $t0, 0xfefe
	ori     $t0, $t0, 0xfefe             # each byte's low bit out
	lui     $t1, 0x00ff
	blez    $a3, 2f
	ori     $t1, $t1, 0xffff             # x, z, y (0x00ffffff)
	lui     $t7, 0xff00                  # the normal's byte
1:	lw      $t2, 0($a1)
	lw      $t3, 0($a2)
	addiu   $a1, $a1, 4
	xor     $t4, $t2, $t3
	and     $t4, $t4, $t0
	srl     $t4, $t4, 1
	and     $t5, $t2, $t3
	addu    $t5, $t5, $t4                # each byte's floor average
	and     $t5, $t5, $t1
	and     $t6, $t2, $t7                # a's normal
	or      $t5, $t5, $t6
	addiu   $a2, $a2, 4
	sw      $t5, 0($a0)
	addiu   $a3, $a3, -1
	bgtz    $a3, 1b
	addiu   $a0, $a0, 4
2:	jr      $ra
	nop
	.size mdl_frame_avg, . - mdl_frame_avg

	.section .text.mdl_lod_copy, "ax", @progbits
	.global mdl_lod_copy
	.type mdl_lod_copy, @function
mdl_lod_copy:
	lhu     $t0, 0($a1)
	addiu   $a2, $a2, -1
1:	addiu   $a1, $a1, 2
	sll     $t0, $t0, 3
	addu    $t0, $t0, $a3                # the parent's qw_vert
	lw      $t1, 0($t0)
	lw      $t2, 4($t0)
	lhu     $t0, 0($a1)                  # (the next one's parent, ahead)
	sw      $t1, 0($a0)
	sw      $t2, 4($a0)
	addiu   $a2, $a2, -1
	bgez    $a2, 1b
	addiu   $a0, $a0, 8
	jr      $ra
	nop
	.size mdl_lod_copy, . - mdl_lod_copy
