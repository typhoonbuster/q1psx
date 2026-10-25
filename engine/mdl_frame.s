# q1psx — mdl_frame: the streamed models' frame cache's lookup (engine/mdl.c,
# engine/mdl.h). C ABI:
#
#   const qw_vert *mdl_frame(int a, const qst_model *m, int frame, uint32_t bkey);
#
# The key: a << 16 | bkey | the frame stored (formats/qstream.h
# qst_frame_index: the frame map's entry, QST_NOFRAME the first; bkey a
# level of detail's band << 11, 0 the whole model). Written past the last
# entry (key[32]), it ends the scan: no bound to test. Four keys a turn,
# each compare in the shadow of the next key's load (9 instructions a
# turn). A hit: the entry stamped with this frame's, its pool returned (the
# ring + its ofs). The sentinel found: a miss, a tail jump to
# mdl_expand_new (engine/mdl.c: its bytes, its entry, its expansion; a2
# the frame stored, a3 bkey). The C specification: engine/mdl.c's
# mdl_frame (the C engine builds it). Clobbers t0-t9, a2.

	.set noreorder
	.set noat

.set QM_FLAGS,    6          # qst_model (formats/qstream.h)
.set QM_SIZE,     96         # its frame map: after the header
.set QSTM_FMAP,   4
.set QST_NOFRAME, 0xffff
.set MC_SENTINEL, 128        # mdl_cache (engine/mdl.c mdl_cache_t): key[32]
.set MC_OFS,      144
.set MC_STAMP,    400
.set MC_RING,     528
.set MC_NOW,      532

	.section .text.mdl_frame, "ax", @progbits
	.global mdl_frame
	.type mdl_frame, @function
mdl_frame:
	lhu     $t0, QM_FLAGS($a1)
	lui     $t3, %hi(mdl_cache)
	andi    $t0, $t0, QSTM_FMAP
	sll     $t2, $a0, 16                 # the key's asset
	beqz    $t0, 1f                      # no map: the frame stored is Quake's
	addiu   $t3, $t3, %lo(mdl_cache)
	sll     $t1, $a2, 1
	addu    $t1, $a1, $t1
	lhu     $a2, QM_SIZE($t1)            # the map's: the frame stored
	li      $t0, QST_NOFRAME
	beq     $a2, $t0, 7f                 # a frame no state plays: the first
1:	or      $t2, $t2, $a2                # the key
8:	or      $t2, $t2, $a3                # (its band)
	addiu   $t9, $t3, MC_SENTINEL
	sw      $t2, 0($t9)                  # past the last entry: the scan's end
	lw      $t5, 0($t3)
2:	lw      $t6, 4($t3)
	beq     $t5, $t2, 4f                 # entry +0
	lw      $t7, 8($t3)
	beq     $t6, $t2, 3f                 # +1
	lw      $t8, 12($t3)
	beq     $t7, $t2, 5f                 # +2
	addiu   $t3, $t3, 16
	bne     $t8, $t2, 2b                 # +3
	lw      $t5, 0($t3)                  # (the next turn's +0)
	b       4f
	addiu   $t3, $t3, -4                 # +3: t3 back on its key
5:	b       4f
	addiu   $t3, $t3, -8                 # +2
3:	addiu   $t3, $t3, 4                  # +1
4:	beq     $t3, $t9, 6f                 # the sentinel's: not in the cache
	lw      $t0, MC_NOW - MC_SENTINEL($t9)
	lw      $v0, MC_OFS($t3)
	lw      $t1, MC_RING - MC_SENTINEL($t9)
	sw      $t0, MC_STAMP($t3)           # drawn this frame
	jr      $ra
	addu    $v0, $v0, $t1                # its pool: the ring + its ofs
6:	j       mdl_expand_new               # (a, m, the frame stored, bkey)
	nop
7:	move    $a2, $zero                   # (the delay slot's key had 0xffff)
	b       8b
	sll     $t2, $a0, 16                 # the key: the asset, the first frame
	.size mdl_frame, . - mdl_frame
