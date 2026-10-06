# Original regression for generator patch 0020 (PPC_HOST_PTR) and the
# physical 4 KiB window offset of include/rcomp/ppc_prelude.h. No game bytes.
# Every guest data access form of the generator: scalar loads/stores, vector
# loads/stores (lvx, lvlx/lvrx, lvewx, stvx, stvlx/stvrx, stvewx), dcbz/dcbzl,
# lwarx/stwcx., ldarx/stdcx. and setjmp/longjmp (the TOML names the two
# targets below). r3 is the guest address under test.
#_ XEX_ENTRY _start

# Cell/VMX128 X-form encodings of the pinned decoder (thirdparty/disasm/ppc-dis.c:
# lvlx X(31,519), lvrx X(31,551), stvlx X(31,647), stvrx X(31,679), dcbzl
# XOPL(31,1014,1)); the stock assembler lacks them without -mcell.
.macro xform_original xop, t, a, b
    .long (31 << 26) | (\t << 21) | (\a << 16) | (\b << 11) | (\xop << 1)
.endm
.macro lvlx_original vd, ra, rb
    xform_original 519, \vd, \ra, \rb
.endm
.macro lvrx_original vd, ra, rb
    xform_original 551, \vd, \ra, \rb
.endm
.macro stvlx_original vs, ra, rb
    xform_original 647, \vs, \ra, \rb
.endm
.macro stvrx_original vs, ra, rb
    xform_original 679, \vs, \ra, \rb
.endm
.macro dcbzl_original ra, rb
    .long (31 << 26) | (1 << 21) | (\ra << 16) | (\rb << 11) | (1014 << 1)
.endm

.text
.globl _start
_start:
    li r3,0
    blr

# ---- scalar (PPC_LOAD_* / PPC_STORE_*) -------------------------------------
# stw r4 at r3.
.globl fn_store_word
fn_store_word:
    stw r4,0(r3)
    blr

# r3 = word at r3.
.globl fn_load_word
fn_load_word:
    lwz r3,0(r3)
    blr

# stb r4 at r3+4, sth r4 at r3+6, std r4 at r3+8.
.globl fn_store_mixed
fn_store_mixed:
    stb r4,4(r3)
    sth r4,6(r3)
    std r4,8(r3)
    blr

# r3 = (byte at r3+4) << 16 | halfword at r3+6.
.globl fn_load_mixed
fn_load_mixed:
    lbz r5,4(r3)
    lhz r6,6(r3)
    slwi r5,r5,16
    or r3,r5,r6
    blr

# r3 = doubleword at r3+8.
.globl fn_load_double
fn_load_double:
    ld r3,8(r3)
    blr

# ---- vector (PPC_HOST_PTR in lvx/lvlx/lvrx, PPC_VSTORE128, PPC_STORE_U8/U32) -
# 16 bytes from aligned r3 to aligned r4.
.globl fn_vector_copy
fn_vector_copy:
    lvx v1,0,r3
    stvx v1,0,r4
    blr

# The 16 bytes at unaligned r3 (lvlx + lvrx) to aligned r4.
.globl fn_unaligned_load
fn_unaligned_load:
    li r5,16
    lvlx_original 1, 0, 3
    lvrx_original 2, 3, 5
    vor v3,v1,v2
    stvx v3,0,r4
    blr

# The 16 bytes at aligned r4 to unaligned r3 (stvlx + stvrx).
.globl fn_unaligned_store
fn_unaligned_store:
    lvx v1,0,r4
    li r5,16
    stvlx_original 1, 0, 3
    stvrx_original 1, 3, 5
    blr

# lvewx at r3 (the generator loads the whole aligned line), stored to aligned r4.
.globl fn_load_element
fn_load_element:
    lvewx v1,0,r3
    stvx v1,0,r4
    blr

# The word of aligned r4's line selected by r3 & 0xC, stored at r3 (stvewx).
.globl fn_store_element
fn_store_element:
    lvx v1,0,r4
    stvewx v1,0,r3
    blr

# ---- cache block zero (memset through PPC_HOST_PTR) ------------------------
.globl fn_dcbz
fn_dcbz:
    dcbz 0,r3
    blr

.globl fn_dcbzl
fn_dcbzl:
    dcbzl_original 0, 3
    blr

# ---- reservations ----------------------------------------------------------
# Atomically adds r4 to the word at r3; returns the new value.
.globl fn_atomic_add32
fn_atomic_add32:
1:  lwarx r5,0,r3
    add r5,r5,r4
    stwcx. r5,0,r3
    bne- 1b
    mr r3,r5
    blr

# Atomically adds r4 to the doubleword at r3; returns the new value.
.globl fn_atomic_add64
fn_atomic_add64:
1:  ldarx r5,0,r3
    add r5,r5,r4
    stdcx. r5,0,r3
    bne- 1b
    mr r3,r5
    blr

# ---- setjmp/longjmp (jmp_buf in guest memory at r3) -------------------------
# setjmp(r3); the first return longjmps with 5, the second returns 5.
.globl fn_jump
fn_jump:
    mflr r12
    stw r12,-8(r1)
    stwu r1,-16(r1)
    mr r30,r3
    bl fn_setjmp_target
    cmpwi cr6,r3,0
    bne cr6,.Ljump_done
    mr r3,r30
    li r4,5
    bl fn_longjmp_target
.Ljump_done:
    addi r1,r1,16
    lwz r12,-8(r1)
    mtlr r12
    blr

# Named by setjmp_address / longjmp_address: calls become host setjmp/longjmp.
.globl fn_setjmp_target
fn_setjmp_target:
    li r3,0
    blr

.globl fn_longjmp_target
fn_longjmp_target:
    blr
