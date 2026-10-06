# Original instruction fixtures. No game bytes or relocated branches.
.text
# Original VX128 encoding (Xenia pin 95a5c3e, ppc_emit_altivec.cc).
# The public stock PPC assembler lacks -mvmx128, so construct only this
# documented encoding. VD is the implicit fourth operand (selection mask).
.macro vsel128_original vd, va, vb
    .long (5 << 26) | 848 | ((\vd & 31) << 21) | ((\vd & 96) >> 3) | ((\va & 31) << 16) | (\va & 32) | ((\va & 64) << 4) | ((\vb & 31) << 11) | ((\vb & 96) >> 5)
.endm
.space 4
.global estimate
estimate:
    frsqrte f1, f2
    blr
.global estimate_record
estimate_record:
    frsqrte. f1, f2
    blr
.global estimate_alias
estimate_alias:
    frsqrte f2, f2
    blr
.global select_standard
select_standard:
    vsel v1, v2, v3, v4
    blr
.global select_high
select_high:
    vsel128_original 127, 64, 95
    blr
.global select_alias_a
select_alias_a:
    vsel128_original 64, 64, 95
    blr
.global select_alias_b
select_alias_b:
    vsel128_original 95, 64, 95
    blr
.global cache_zero_base
cache_zero_base:
    dcbst 0, r4
    blr
.global cache_indexed
cache_indexed:
    dcbst r3, r4
    blr
.global cache_publish
cache_publish:
    stw r3, 0(r4)
    dcbst 0, r4
    sync
    blr
.global barrier_sync
barrier_sync:
    sync
    blr
.global barrier_light
barrier_light:
    lwsync
    blr
.global barrier_io
barrier_io:
    eieio
    blr
.global read_fpscr
read_fpscr:
    mffs f1
    blr
.global read_fpscr_record
read_fpscr_record:
    mffs. f1
    blr
.global write_fpscr
write_fpscr:
    mtfsf 255, f2
    blr
.global write_rounding_only
write_rounding_only:
    mtfsf 1, f2
    blr
.global write_fpscr_partial_record
write_fpscr_partial_record:
    mtfsf. 0x42, f2
    blr
