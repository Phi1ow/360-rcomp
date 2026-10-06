# Original regression for generator patch 0021. No game bytes.
# Every global .text label becomes a .pdata function (cpu/tools/mkxex.py);
# the .L labels are labels inside those functions.
#_ XEX_ENTRY _start
#_ XEX_FUNCTION_END fn_sw_unreachable fn_sw_unreachable_end
#_ XEX_FUNCTION_END fn_data_owner fn_data_owner_end

# VMX128 encodings of the pinned decoder (thirdparty/disasm/ppc-dis.c). The
# stock assembler lacks VMX128. VX128(op, xop): vD128, vA128, vB128 split as in
# patch0019_original.s. VX128_1(4, xop): vD128, rA, rB. VX128_5(4, 16): vsldoi128
# with SH in bits 6-9.
.macro vx128_original op, xop, vd, va, vb
    .long (\op << 26) | \xop | ((\vd & 31) << 21) | ((\vd & 96) >> 3) | ((\va & 31) << 16) | (\va & 32) | ((\va & 64) << 4) | ((\vb & 31) << 11) | ((\vb & 96) >> 5)
.endm
.macro vor128_original vd, va, vb
    vx128_original 5, 720, \vd, \va, \vb
.endm
.macro vrlw128_original vd, va, vb
    vx128_original 6, 80, \vd, \va, \vb
.endm
.macro vx128_1_original xop, vd, ra, rb
    .long (4 << 26) | \xop | ((\vd & 31) << 21) | ((\vd & 96) >> 3) | (\ra << 16) | (\rb << 11)
.endm
.macro vsldoi128_original vd, va, vb, sh
    .long (4 << 26) | 0x10 | ((\vd & 31) << 21) | ((\vd & 96) >> 3) | ((\va & 31) << 16) | (\va & 32) | ((\va & 64) << 4) | ((\vb & 31) << 11) | ((\vb & 96) >> 5) | (\sh << 6)
.endm

.text
.globl _start
_start:
    li r3,0
    blr

# ---- load/store with update: r3 = base, r5 = index; rA = EA afterwards -----
.globl fn_lhzu
fn_lhzu:
    lhzu r4,6(r3)
    blr
.globl fn_lhau
fn_lhau:
    lhau r4,-2(r3)
    blr
.globl fn_lfsu
fn_lfsu:
    lfsu f1,8(r3)
    blr
.globl fn_lfdu
fn_lfdu:
    lfdu f1,16(r3)
    blr
.globl fn_sthu
fn_sthu:
    sthu r4,4(r3)
    blr
.globl fn_stfsu
fn_stfsu:
    stfsu f1,12(r3)
    blr
.globl fn_stfdu
fn_stfdu:
    stfdu f1,-24(r3)
    blr
.globl fn_lbzux
fn_lbzux:
    lbzux r4,r3,r5
    blr
.globl fn_lhzux
fn_lhzux:
    lhzux r4,r3,r5
    blr
.globl fn_lhaux
fn_lhaux:
    lhaux r4,r3,r5
    blr
.globl fn_lwzux
fn_lwzux:
    lwzux r5,r3,r5          # rT == rB: EA uses the old r5
    blr
.globl fn_lwaux
fn_lwaux:
    lwaux r4,r3,r5
    blr
.globl fn_ldux
fn_ldux:
    ldux r4,r3,r5
    blr
.globl fn_lfsux
fn_lfsux:
    lfsux f1,r3,r5
    blr
.globl fn_lfdux
fn_lfdux:
    lfdux f1,r3,r5
    blr
.globl fn_stbux
fn_stbux:
    stbux r4,r3,r5
    blr
.globl fn_sthux
fn_sthux:
    sthux r3,r3,r5          # rS == rA: the old r3 is stored
    blr
.globl fn_stdux
fn_stdux:
    stdux r4,r3,r5
    blr
.globl fn_stfsux
fn_stfsux:
    stfsux f1,r3,r5
    blr
.globl fn_stfdux
fn_stfdux:
    stfdux f1,r3,r5
    blr
.globl fn_lhbrx
fn_lhbrx:
    lhbrx r4,r3,r5
    lhbrx r6,0,r7           # rA = 0: EA = r7
    blr

# ---- bdzf / bdzt ------------------------------------------------------------
# CTR = r3. Each pass: i += 1, cr7.lt = (i < r4); bdzf leaves the loop when the
# decremented CTR is 0 and cr7.lt is false. r3 = i, r4 = CTR at exit.
.globl fn_bdzf
fn_bdzf:
    mtctr r3
    li r5,0
.Lzf_loop:
    addi r5,r5,1
    cmpw cr7,r5,r4
    bdzf 28,.Lzf_out
    cmpwi cr6,r5,40
    blt cr6,.Lzf_loop
.Lzf_out:
    mr r3,r5
    mfctr r4
    blr

# Same with bdzt on cr6.gt (BI 25): leave when CTR reaches 0 and i > r4.
.globl fn_bdzt
fn_bdzt:
    mtctr r3
    li r5,0
.Lzt_loop:
    addi r5,r5,1
    cmpw cr6,r5,r4
    bdzt 25,.Lzt_out
    cmpwi cr7,r5,40
    blt cr7,.Lzt_loop
.Lzt_out:
    mr r3,r5
    mfctr r4
    blr

# A bdzf to another function is a conditional tail call: r3 = 2 when CTR - 1
# == 0 and r4 != 0 (cr6.eq false), else 1.
.globl fn_bdzf_tail
fn_bdzf_tail:
    mtctr r3
    cmpwi cr6,r4,0
    bdzf 26,fn_returns_two
    li r3,1
    blr
.globl fn_returns_two
fn_returns_two:
    li r3,2
    blr

# ---- VMX integer forms --------------------------------------------------------
# r3 = 16-byte aligned buffer: A at 0, B at 16; results from 32, CR at 176.
.globl fn_vmx
fn_vmx:
    li r4,16
    lvx v1,0,r3
    lvx v2,r3,r4
    vaddsws v3,v1,v2
    li r4,32
    stvx v3,r3,r4
    vsubuwm v3,v1,v2
    li r4,48
    stvx v3,r3,r4
    vsububm v3,v1,v2
    li r4,64
    stvx v3,r3,r4
    vadduhs v3,v1,v2
    li r4,80
    stvx v3,r3,r4
    vsubuws v3,v1,v2
    li r4,96
    stvx v3,r3,r4
    vcmpgtuw v3,v1,v2
    li r4,112
    stvx v3,r3,r4
    vrlw v3,v1,v2
    li r4,128
    stvx v3,r3,r4
    vor128_original 70,1,1
    vor128_original 101,2,2
    vrlw128_original 90,70,101
    vor128_original 3,90,90
    li r4,144
    stvx v3,r3,r4
    # vsldoi128 words whose low 11 bits are those of maclhwu (0x318, SH 12)
    # and macchwu (0x118, SH 4): vD128 in v64..v95, vA128/vB128 below v32.
    vsldoi128_original 93,1,2,12
    vsldoi128_original 95,1,2,4
    vor128_original 3,93,93
    li r4,160
    stvx v3,r3,r4
    vor128_original 3,95,95
    li r4,192
    stvx v3,r3,r4
    vcmpgtuw. v3,v1,v2
    mfcr r5
    stw r5,176(r3)
    blr

# "LRU" forms: lvxl / lvxl128 load, stvxl / stvlxl128 store. r3 = buffer.
.globl fn_lru
fn_lru:
    li r4,16
    lvxl v1,r3,r4
    li r4,48
    stvxl v1,r3,r4
    li r4,32
    vx128_1_original 707,80,3,4        # lvxl128 v80,r3,r4
    li r4,69
    vx128_1_original 1795,80,3,4       # stvlxl128 v80,r3,r4 (bytes 69..79)
    blr

# ---- record forms of doubleword results: CR0 compares the 64-bit result -------
.globl fn_rldicl_rc
fn_rldicl_rc:
    rldicl. r4,r3,32,32
    mfcr r5
    sradi. r6,r3,4
    mfcr r7
    rldicr. r8,r3,0,31
    mfcr r9
    blr

# ---- jump tables -------------------------------------------------------------
# (a) absolute table, scheduled lis, rlwinm, addi with a nop: 0..3 -> 10..13, else 99.
.globl fn_sw_sched
fn_sw_sched:
    cmplwi cr6,r3,3
    bgt cr6,.La_default
    lis r12,.La_table@ha
    rlwinm r0,r3,2,0,29
    addi r12,r12,.La_table@l
    nop
    lwzx r0,r12,r0
    mtctr r0
    bctr
.La0:
    li r3,10
    blr
.La1:
    li r3,11
    blr
.La2:
    li r3,12
    blr
.La3:
    li r3,13
    blr
.La_default:
    li r3,99
    blr

# (b) computed table (byte << 2) with nops: 0..4 -> 20, 21, 22, 21, 20; else 98.
.globl fn_sw_computed
fn_sw_computed:
    cmplwi cr6,r3,4
    bgt cr6,.Lb_default
    lis r12,.Lb_bytes@ha
    addi r12,r12,.Lb_bytes@l
    lbzx r0,r12,r3
    rlwinm r0,r0,2,0,29
    lis r12,.Lb_base@ha
    nop
    addi r12,r12,.Lb_base@l
    add r12,r12,r0
    mtctr r12
    bctr
.Lb_base:
.Lb0:
    li r3,20
    blr
.Lb1:
    li r3,21
    blr
.Lb2:
    li r3,22
    blr
.Lb_default:
    li r3,98
    blr

# (c) byte offset table with nops: 0..2 -> 30, 31, 30; else 97.
.globl fn_sw_byte
fn_sw_byte:
    cmplwi cr6,r3,2
    bgt cr6,.Lc_default
    lis r12,.Lc_bytes@ha
    addi r12,r12,.Lc_bytes@l
    lbzx r0,r12,r3
    lis r12,.Lc_base@ha
    nop
    addi r12,r12,.Lc_base@l
    nop
    add r12,r12,r0
    mtctr r12
    bctr
.Lc_base:
.Lc0:
    li r3,30
    blr
.Lc1:
    li r3,31
    blr
.Lc_default:
    li r3,97
    blr

# (d) 16-bit offset table, scheduled, with a nop: 0..2 -> 42, 41, 40; else 96.
.globl fn_sw_short
fn_sw_short:
    cmplwi cr6,r3,2
    bgt cr6,.Ld_default
    lis r12,.Ld_half@ha
    rlwinm r0,r3,1,0,30
    addi r12,r12,.Ld_half@l
    lhzx r0,r12,r0
    lis r12,.Ld_base@ha
    addi r12,r12,.Ld_base@l
    nop
    add r12,r12,r0
    mtctr r12
    bctr
.Ld_base:
.Ld0:
    li r3,40
    blr
.Ld1:
    li r3,41
    blr
.Ld2:
    li r3,42
    blr
.Ld_default:
    li r3,96
    blr

# (e) guard on the lt bit (bge leaves for index >= 3): 0..2 -> 50..52; else 95.
.globl fn_sw_bge
fn_sw_bge:
    cmplwi cr6,r3,3
    bge cr6,.Le_default
    lis r12,.Le_table@ha
    addi r12,r12,.Le_table@l
    rlwinm r0,r3,2,0,29
    lwzx r0,r12,r0
    mtctr r0
    bctr
.Le0:
    li r3,50
    blr
.Le1:
    li r3,51
    blr
.Le2:
    li r3,52
    blr
.Le_default:
    li r3,95
    blr

# (f) the index register is reused before the bctr (switch on the CTR target):
# 0..2 -> 60, 61, 62; else 94.
.globl fn_sw_reuse
fn_sw_reuse:
    cmplwi cr6,r3,2
    bgt cr6,.Lf_default
    lis r10,.Lf_table@ha
    rlwinm r3,r3,2,0,29
    addi r10,r10,.Lf_table@l
    lwzx r11,r3,r10
    li r3,0
    mtctr r11
    bctr
.Lf0:
    addi r3,r3,60
    blr
.Lf1:
    addi r3,r3,61
    blr
.Lf2:
    addi r3,r3,62
    blr
.Lf_default:
    li r3,94
    blr

# (f2) the guard compares the register the index was copied from (mr after
# the compare's operand is set, compare after the copy): 0..2 -> 100..102; else 92.
.globl fn_sw_copy
fn_sw_copy:
    mr r31,r4
    cmplwi cr6,r4,2
    bgt cr6,.Lm_default
    lis r12,.Lm_table@ha
    rlwinm r0,r31,2,0,29
    addi r12,r12,.Lm_table@l
    lwzx r0,r12,r0
    mtctr r0
    bctr
.Lm0:
    li r3,100
    blr
.Lm1:
    li r3,101
    blr
.Lm2:
    li r3,102
    blr
.Lm_default:
    li r3,92
    blr

# (f3) the guard branches over an unconditional branch to the default:
# 0..1 -> 110, 111; else 91.
.globl fn_sw_skip
fn_sw_skip:
    cmplwi cr6,r3,1
    ble cr6,.Ln_code
    b .Ln_default
.Ln_code:
    lis r12,.Ln_table@ha
    rlwinm r0,r3,2,0,29
    addi r12,r12,.Ln_table@l
    lwzx r0,r12,r0
    mtctr r0
    bctr
.Ln0:
    li r3,110
    blr
.Ln1:
    li r3,111
    blr
.Ln_default:
    li r3,91
    blr

# (g) inline table with no guard (index bounded by its caller) followed by code
# that is not its first case: 0..2 -> 71, 70, 77.
.globl fn_sw_inline_gap
fn_sw_inline_gap:
    lis r12,.Lg_table@ha
    addi r12,r12,.Lg_table@l
    rlwinm r0,r3,2,0,29
    lwzx r0,r12,r0
    mtctr r0
    bctr
.Lg_table:
    .long .Lg1, .Lg0, .Lg2
.Lg_other:
    li r3,77
    blr
.Lg0:
    li r3,70
    blr
.Lg1:
    li r3,71
    blr
.Lg2:
    b .Lg_other

# (h) an unrelated cmplwi of another register right before an unguarded inline
# table (r4 is clamped, the index r3 is bounded by its caller): 0..1 -> 80, 81.
.globl fn_sw_other_compare
fn_sw_other_compare:
    cmplwi cr6,r4,1024
    ble cr6,.Lh_keep
    li r4,1024
.Lh_keep:
    lis r12,.Lh_table@ha
    addi r12,r12,.Lh_table@l
    rlwinm r0,r3,2,0,29
    lwzx r0,r12,r0
    mtctr r0
    bctr
.Lh_table:
    .long .Lh0, .Lh1
.Lh0:
    li r3,80
    blr
.Lh1:
    li r3,81
    blr

# (i) cases the compiler marked unreachable: index 1 points past the end of the
# function at a zero word, index 2 at the start of the next function.
# 0 -> 90, 2 -> 2 (fn_returns_two_again), 1 -> program exception at the zero word.
.globl fn_sw_unreachable
fn_sw_unreachable:
    cmplwi cr6,r3,2
    bgt cr6,.Li_default
    lis r12,.Li_table@ha
    addi r12,r12,.Li_table@l
    rlwinm r0,r3,2,0,29
    lwzx r0,r12,r0
    mtctr r0
    bctr
.Li0:
    li r3,90
    blr
.Li_default:
    li r3,93
    blr
fn_sw_unreachable_end:
    .long 0
.globl fn_returns_two_again
fn_returns_two_again:
    li r3,2
    blr

# (j) a guarded table of function addresses is a call through a function
# pointer, not a switch: 0 -> 2, 1 -> 2 (fn_returns_two, fn_returns_two_again).
.globl fn_dispatch
fn_dispatch:
    cmplwi cr6,r3,1
    bgtlr cr6
    lis r10,.Lk_table@ha
    rlwinm r11,r3,2,0,29
    addi r10,r10,.Lk_table@l
    lwzx r11,r11,r10
    mtctr r11
    bctr

# ---- data after a function that no table, call or branch names --------------
# The scan for unlisted code must not turn it into functions: no "Unable to
# decode" diagnostic. 0x48000000 is `b .` (decodes), the next word does not.
.globl fn_data_owner
fn_data_owner:
    li r3,5
    blr
fn_data_owner_end:
    .long 0x00485645, 0x48000000, 0x00000001, 0xE1BF4000, 0x00589D48
.globl fn_after_data
fn_after_data:
    li r3,6
    blr

.section .rodata
.balign 4
.La_table:
    .long .La0, .La1, .La2, .La3
.Lb_bytes:
    .byte (.Lb0 - .Lb_base) / 4, (.Lb1 - .Lb_base) / 4, (.Lb2 - .Lb_base) / 4, (.Lb1 - .Lb_base) / 4, (.Lb0 - .Lb_base) / 4
.Lc_bytes:
    .byte .Lc0 - .Lc_base, .Lc1 - .Lc_base, .Lc0 - .Lc_base
.balign 2
.Ld_half:
    .short .Ld2 - .Ld_base, .Ld1 - .Ld_base, .Ld0 - .Ld_base
.balign 4
.Le_table:
    .long .Le0, .Le1, .Le2
.Lf_table:
    .long .Lf0, .Lf1, .Lf2
.Lm_table:
    .long .Lm0, .Lm1, .Lm2
.Ln_table:
    .long .Ln0, .Ln1
.Li_table:
    .long .Li0, fn_sw_unreachable_end, fn_returns_two_again
.Lk_table:
    .long fn_returns_two, fn_returns_two_again
