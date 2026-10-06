# Original regression for generator patch 0019. No game bytes.
# Every global .text label becomes a .pdata function (cpu/tools/mkxex.py);
# the .L labels are entry points inside those functions.
#_ XEX_ENTRY _start

# VX128 encodings of the pinned decoder (thirdparty/disasm/ppc-dis.c:
# vnor128 = VX128(5, 656), vor128 = VX128(5, 720), vcfpuxws128 =
# VX128_3(6, 624) with UIMM in bits 16-20). The stock assembler lacks VMX128.
.macro vx128_original xop, vd, va, vb
    .long (5 << 26) | \xop | ((\vd & 31) << 21) | ((\vd & 96) >> 3) | ((\va & 31) << 16) | (\va & 32) | ((\va & 64) << 4) | ((\vb & 31) << 11) | ((\vb & 96) >> 5)
.endm
.macro vnor128_original vd, va, vb
    vx128_original 656, \vd, \va, \vb
.endm
.macro vor128_original vd, va, vb
    vx128_original 720, \vd, \va, \vb
.endm
.macro vcfpuxws128_original vd, vb, uimm
    .long (6 << 26) | 624 | ((\vd & 31) << 21) | ((\vd & 96) >> 3) | ((\uimm & 31) << 16) | ((\vb & 31) << 11) | ((\vb & 96) >> 5)
.endm

.text
.globl _start
_start:
    li r3,0
    blr

# ---- entry points inside a function --------------------------------------
# fn_owner: r3 = 7 + 1 = 8. Its tail [.Lowner_tail, end) is closed.
.globl fn_owner
fn_owner:
    li r3,7
.Lowner_tail:
    addi r3,r3,1
    cmpwi cr6,r3,50
    blt cr6,.Lowner_done
    addi r3,r3,100
.Lowner_done:
    blr

# A tail call into the tail: 60 + 1 >= 50, so 161.
.globl fn_tail_jump
fn_tail_jump:
    li r3,60
    b .Lowner_tail

# A call into the tail: (20 + 1) + 1000 = 1021.
.globl fn_tail_call
fn_tail_call:
    mflr r12
    stw r12,-8(r1)
    stwu r1,-16(r1)
    li r3,20
    bl .Lowner_tail
    addi r3,r3,1000
    addi r1,r1,16
    lwz r12,-8(r1)
    mtlr r12
    blr

# A tail that ends in a branch to another tail: closed only once the first
# tail is a function (the generator repeats its search). 1 + 2 + 1 = 4.
.globl fn_chain_owner
fn_chain_owner:
    li r3,1
.Lchain_tail:
    addi r3,r3,2
    b .Lowner_tail

# 10 + 2 + 1 = 13.
.globl fn_chain_jump
fn_chain_jump:
    li r3,10
    b .Lchain_tail

# ---- bdnzt / bdnzf on a CR bit other than eq ------------------------------
# Counts while CTR != 0 and cr7.lt (BI 28, r3 < 5): min(r3_in, 5).
.globl fn_bdnzt
fn_bdnzt:
    mtctr r3
    li r3,0
.Lt_loop:
    addi r3,r3,1
    cmpwi cr7,r3,5
    bdnzt 28,.Lt_loop
    blr

# Counts while CTR != 0 and not cr6.gt (BI 25, r3 <= 4): min(r3_in, 5).
# The upstream translation tested eq and stopped at 4.
.globl fn_bdnzf
fn_bdnzf:
    mtctr r3
    li r3,0
.Lf_loop:
    addi r3,r3,1
    cmpwi cr6,r3,4
    bdnzf 25,.Lf_loop
    blr

# ---- VMX128 ----------------------------------------------------------------
# r3 = 16-byte aligned buffer: A at 0, B at 16; outputs at 32, 48, 64.
.globl fn_vnor
fn_vnor:
    li r4,16
    lvx v1,0,r3
    lvx v2,r3,r4
    vnor128_original 64, 1, 2
    vor128_original 3, 64, 64
    li r4,32
    stvx v3,r3,r4
    vnor v4,v1,v2
    li r4,48
    stvx v4,r3,r4
    vor128_original 70, 1, 1
    vor128_original 100, 2, 2
    vnor128_original 127, 70, 100
    vor128_original 5, 127, 127
    li r4,64
    stvx v5,r3,r4
    blr

# r3 = buffer: four floats at 0; unsigned words for scale 2^3 at 16, 2^0 at
# 32 and 2^31 (from a high source register) at 48.
.globl fn_vcfpuxws
fn_vcfpuxws:
    lvx v1,0,r3
    vcfpuxws128_original 65, 1, 3
    vor128_original 2, 65, 65
    li r4,16
    stvx v2,r3,r4
    vcfpuxws128_original 3, 1, 0
    li r4,32
    stvx v3,r3,r4
    vor128_original 90, 1, 1
    vcfpuxws128_original 4, 90, 31
    li r4,48
    stvx v4,r3,r4
    blr

# ---- 16-bit offset switch whose guard is hoisted ---------------------------
# The cmplwi is 40 instructions before the table load, beyond the 32 the
# analyser searched for this form. Cases 0..3 -> 11, 22, 33, 44; else 55.
.globl fn_hoisted_switch
fn_hoisted_switch:
    cmplwi cr6,r3,3
    bgt cr6,.Lhs_default
    li r5,0
    .rept 40
    addi r5,r5,1
    .endr
    lis r12,hs_table@ha
    addi r12,r12,hs_table@l
    rlwinm r0,r3,1,0,30
    lhzx r0,r12,r0
    lis r12,.Lhs_base@ha
    addi r12,r12,.Lhs_base@l
    add r12,r12,r0
    mtctr r12
    bctr
.Lhs_base:
.Lhs_zero:
    li r3,11
    blr
.Lhs_one:
    li r3,22
    blr
.Lhs_two:
    li r3,33
    blr
.Lhs_three:
    li r3,44
    blr
.Lhs_default:
    li r3,55
    blr

.section .rodata
.balign 4
hs_table:
    .short .Lhs_zero - .Lhs_base, .Lhs_one - .Lhs_base, .Lhs_two - .Lhs_base, .Lhs_three - .Lhs_base
