# Synthetic XEX "title" (original code, no game content), packaged by
# cpu/tools/mkxex.py and recompiled by XenonRecomp in XEX/TOML mode.
#
# _start(r3 = mode):
#   1. s = fn_table_sum()              direct call; sums the 8 words of k_table (.rdata) = 36
#   2. s = g_ops[1](s)                 indirect call through a function pointer table (.data) = 108
#   3. p = NtAllocateVirtualMemory(64 KiB); *p = s; s = *p + 1; NtFreeVirtualMemory(p)   = 109
#   4. if mode == 1: call imp_xbdm_0012 (import from a module no export table knows)
#   return s (0x6D), or 0xFFFF...FFn on failure of step n
# Variable imports (import slots filled by the runtime loader, see
# runtime/include/rcomp/runtime/xex_loader.h):
#   mode 2: return *(u32*)slot_krnl_version   first word of XboxKrnlVersion as
#           registered by the runner (a TESTDOUBLE_ variable in tests/xex)
#   mode 3: return slot_hw_info               raw slot of XboxHardwareInfo, never
#           registered: the loader's poison address (0x1560 for ordinal 0x156)
#   mode 4: return slot_ntalloc               slot of a *function* export: poison 0x0CC0
# Jump tables (the four patterns XenonAnalyse recognises), each switch
# returns 10*t+k for k = 0..3 and 0x63 by default; the driver packs
# f(0)..f(4) one byte each into r3:
#   mode 5: absolute table        0x0A0B0C0D63
#   mode 6: computed (byte << 2)  0x1415161763
#   mode 7: byte offsets          0x1E1F202163
#   mode 8: 16-bit offsets        0x28292A2B63
# Register save/restore helpers (__savegprlr_14 / __restgprlr_14, as in
# compiler output) used by a prologue/epilogue:
#   mode 9: 5*100 + 6*10 + 7 = 567 (0x237) if r29-r31 of the caller survive,
#           0xBAD otherwise
#
#_ XEX_ENTRY _start
#_ XEX_IMPORT imp_NtAllocateVirtualMemory xboxkrnl.exe 0x00CC
#_ XEX_IMPORT imp_NtFreeVirtualMemory xboxkrnl.exe 0x00DC
#_ XEX_IMPORT imp_xbdm_0012 xbdm.xex 0x0012
#_ XEX_IMPORT_RECORD slot_krnl_version xboxkrnl.exe 0x0158
#_ XEX_IMPORT_RECORD slot_hw_info xboxkrnl.exe 0x0156
#_ XEX_IMPORT_RECORD slot_ntalloc xboxkrnl.exe 0x00CC

    .text
    .globl _start
_start:
    cmpwi cr0, r3, 5
    bge cr0, .Lhigh_modes
    cmpwi cr0, r3, 2
    beq cr0, .Lmode2
    cmpwi cr0, r3, 3
    beq cr0, .Lmode3
    cmpwi cr0, r3, 4
    beq cr0, .Lmode4
    mflr r12
    stw r12, -8(r1)
    std r30, -24(r1)
    std r31, -16(r1)
    stwu r1, -128(r1)
    mr r31, r1
    mr r30, r3
    # 1. direct call
    bl fn_table_sum
    stw r3, 0x60(r31)
    # 2. indirect call: g_ops[1](s)
    lis r11, g_ops@ha
    addi r11, r11, g_ops@l
    lwz r12, 4(r11)
    mtctr r12
    lwz r3, 0x60(r31)
    bctrl
    stw r3, 0x60(r31)
    # 3. allocate, round-trip, free
    li r0, 0
    stw r0, 0x50(r31)
    lis r0, 1
    stw r0, 0x54(r31)
    addi r3, r31, 0x50
    addi r4, r31, 0x54
    li r5, 0x3000
    li r6, 4
    li r7, 0
    bl imp_NtAllocateVirtualMemory
    li r9, 3
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    lwz r8, 0x50(r31)
    lwz r3, 0x60(r31)
    stw r3, 0(r8)
    lwz r3, 0(r8)
    addi r3, r3, 1
    stw r3, 0x60(r31)
    li r0, 0
    stw r0, 0x54(r31)
    addi r3, r31, 0x50
    addi r4, r31, 0x54
    lis r5, 0
    ori r5, r5, 0x8000
    li r6, 0
    bl imp_NtFreeVirtualMemory
    li r9, 4
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    # 4. optional unresolved import
    cmpwi cr0, r30, 1
    bne cr0, .Ldone
    li r3, 0x77
    bl imp_xbdm_0012
.Ldone:
    lwz r3, 0x60(r31)
    b .Lret
.Lfail:
    li r3, -16
    or r3, r3, r9
.Lret:
    addi r1, r1, 128
    lwz r12, -8(r1)
    mtlr r12
    ld r30, -24(r1)
    ld r31, -16(r1)
    blr

.Lhigh_modes:
    cmpwi cr0, r3, 9
    beq cr0, .Lmode9
    b fn_switch_driver
.Lmode9:
    b fn_gpr_check

.Lmode2:
    lis r11, slot_krnl_version@ha
    lwz r11, slot_krnl_version@l(r11)
    lwz r3, 0(r11)
    blr
.Lmode3:
    lis r11, slot_hw_info@ha
    lwz r3, slot_hw_info@l(r11)
    blr
.Lmode4:
    lis r11, slot_ntalloc@ha
    lwz r3, slot_ntalloc@l(r11)
    blr

    .globl fn_switch_driver
fn_switch_driver:
    mflr r12
    stw r12, -8(r1)
    std r30, -24(r1)
    std r31, -16(r1)
    stwu r1, -96(r1)
    mr r31, r3
    li r30, 0
    li r0, 0
    std r0, 0x50(r1)
.Ldrv_loop:
    mr r3, r30
    cmpwi cr0, r31, 5
    bne cr0, .Ldrv_not5
    bl fn_sw_absolute
    b .Ldrv_acc
.Ldrv_not5:
    cmpwi cr0, r31, 6
    bne cr0, .Ldrv_not6
    bl fn_sw_computed
    b .Ldrv_acc
.Ldrv_not6:
    cmpwi cr0, r31, 7
    bne cr0, .Ldrv_not7
    bl fn_sw_byte
    b .Ldrv_acc
.Ldrv_not7:
    bl fn_sw_short
.Ldrv_acc:
    ld r4, 0x50(r1)
    sldi r4, r4, 8
    or r4, r4, r3
    std r4, 0x50(r1)
    addi r30, r30, 1
    cmpwi cr0, r30, 5
    blt cr0, .Ldrv_loop
    ld r3, 0x50(r1)
    addi r1, r1, 96
    lwz r12, -8(r1)
    mtlr r12
    ld r30, -24(r1)
    ld r31, -16(r1)
    blr

# ABSOLUTE: lis, addi, rlwinm, lwzx, mtctr, bctr
    .globl fn_sw_absolute
fn_sw_absolute:
    cmplwi cr6, r3, 3
    bgt cr6, .Labs_def
    lis r11, tbl_abs@ha
    addi r11, r11, tbl_abs@l
    rlwinm r0, r3, 2, 0, 29
    lwzx r0, r11, r0
    mtctr r0
    bctr
.Labs0:
    li r3, 10
    blr
.Labs1:
    li r3, 11
    blr
.Labs2:
    li r3, 12
    blr
.Labs3:
    li r3, 13
    blr
.Labs_def:
    li r3, 0x63
    blr

# COMPUTED: lis, addi, lbzx, rlwinm, lis, addi, add, mtctr (label = base + (byte << 2))
    .globl fn_sw_computed
fn_sw_computed:
    cmplwi cr6, r3, 3
    bgt cr6, .Lcmp_def
    lis r11, tbl_cmp@ha
    addi r11, r11, tbl_cmp@l
    lbzx r0, r11, r3
    rlwinm r0, r0, 2, 0, 29
    lis r12, .Lcmp0@ha
    addi r12, r12, .Lcmp0@l
    add r12, r12, r0
    mtctr r12
    bctr
.Lcmp0:
    li r3, 20
    blr
    li r3, 21
    blr
    li r3, 22
    blr
    li r3, 23
    blr
.Lcmp_def:
    li r3, 0x63
    blr

# BYTE OFFSET: lis, addi, lbzx, lis, addi, add, mtctr (label = base + byte)
    .globl fn_sw_byte
fn_sw_byte:
    cmplwi cr6, r3, 3
    bgt cr6, .Lbyte_def
    lis r11, tbl_byte@ha
    addi r11, r11, tbl_byte@l
    lbzx r0, r11, r3
    lis r12, .Lbyte0@ha
    addi r12, r12, .Lbyte0@l
    add r12, r12, r0
    mtctr r12
    bctr
.Lbyte0:
    li r3, 30
    blr
    li r3, 31
    blr
    li r3, 32
    blr
    li r3, 33
    blr
.Lbyte_def:
    li r3, 0x63
    blr

# SHORT OFFSET: lis, addi, rlwinm, lhzx, lis, addi, add, mtctr (label = base + halfword)
    .globl fn_sw_short
fn_sw_short:
    cmplwi cr6, r3, 3
    bgt cr6, .Lshort_def
    lis r11, tbl_short@ha
    addi r11, r11, tbl_short@l
    rlwinm r0, r3, 1, 0, 30
    lhzx r0, r11, r0
    lis r12, .Lshort0@ha
    addi r12, r12, .Lshort0@l
    add r12, r12, r0
    mtctr r12
    bctr
.Lshort0:
    li r3, 40
    blr
    li r3, 41
    blr
    li r3, 42
    blr
    li r3, 43
    blr
.Lshort_def:
    li r3, 0x63
    blr

# Caller of fn_nonvolatile: r29-r31 must come back unchanged.
    .globl fn_gpr_check
fn_gpr_check:
    mflr r12
    stw r12, -8(r1)
    stwu r1, -0x60(r1)
    li r29, 0x29
    li r30, 0x30
    li r31, 0x31
    bl fn_nonvolatile
    cmpwi cr0, r29, 0x29
    bne cr0, .Lgpr_bad
    cmpwi cr0, r30, 0x30
    bne cr0, .Lgpr_bad
    cmpwi cr0, r31, 0x31
    beq cr0, .Lgpr_ok
.Lgpr_bad:
    li r3, 0xBAD
.Lgpr_ok:
    addi r1, r1, 0x60
    lwz r12, -8(r1)
    mtlr r12
    blr

# Prologue/epilogue through the helpers, as compiler output does.
    .globl fn_nonvolatile
fn_nonvolatile:
    mflr r12
    bl .Lsavegpr_29
    stwu r1, -0x70(r1)
    li r29, 5
    li r30, 6
    li r31, 7
    mulli r3, r29, 100
    mulli r4, r30, 10
    add r3, r3, r4
    add r3, r3, r31
    addi r1, r1, 0x70
    b .Lrestgpr_29

# __savegprlr_N = __savegprlr_14 + 4*(N-14): std rN, -(8*(32-N)+8)(r1) ... ; stw r12, -8(r1); blr
    .globl __savegprlr_14
__savegprlr_14:
    std r14, -0x98(r1)
    std r15, -0x90(r1)
    std r16, -0x88(r1)
    std r17, -0x80(r1)
    std r18, -0x78(r1)
    std r19, -0x70(r1)
    std r20, -0x68(r1)
    std r21, -0x60(r1)
    std r22, -0x58(r1)
    std r23, -0x50(r1)
    std r24, -0x48(r1)
    std r25, -0x40(r1)
    std r26, -0x38(r1)
    std r27, -0x30(r1)
    std r28, -0x28(r1)
.Lsavegpr_29:
    std r29, -0x20(r1)
    std r30, -0x18(r1)
    std r31, -0x10(r1)
    stw r12, -0x8(r1)
    blr

    .globl __restgprlr_14
__restgprlr_14:
    ld r14, -0x98(r1)
    ld r15, -0x90(r1)
    ld r16, -0x88(r1)
    ld r17, -0x80(r1)
    ld r18, -0x78(r1)
    ld r19, -0x70(r1)
    ld r20, -0x68(r1)
    ld r21, -0x60(r1)
    ld r22, -0x58(r1)
    ld r23, -0x50(r1)
    ld r24, -0x48(r1)
    ld r25, -0x40(r1)
    ld r26, -0x38(r1)
    ld r27, -0x30(r1)
    ld r28, -0x28(r1)
.Lrestgpr_29:
    ld r29, -0x20(r1)
    ld r30, -0x18(r1)
    ld r31, -0x10(r1)
    lwz r12, -0x8(r1)
    mtlr r12
    blr

    .globl fn_table_sum
fn_table_sum:
    lis r11, k_table@ha
    addi r11, r11, k_table@l
    li r3, 0
    li r0, 8
    mtctr r0
.Lsum:
    lwz r10, 0(r11)
    add r3, r3, r10
    addi r11, r11, 4
    bdnz .Lsum
    blr

    .globl fn_times2
fn_times2:
    add r3, r3, r3
    blr

    .globl fn_times3
fn_times3:
    mulli r3, r3, 3
    blr

# Import thunks (16 bytes each; mkxex.py writes the thunk data word).
    .globl imp_NtAllocateVirtualMemory
imp_NtAllocateVirtualMemory:
    .long 0, 0, 0, 0
    .globl imp_NtFreeVirtualMemory
imp_NtFreeVirtualMemory:
    .long 0, 0, 0, 0
    .globl imp_xbdm_0012
imp_xbdm_0012:
    .long 0, 0, 0, 0

    .section .rodata
k_table:
    .long 1, 2, 3, 4, 5, 6, 7, 8
tbl_abs:
    .long .Labs0, .Labs1, .Labs2, .Labs3
tbl_cmp:
    .byte 0, 2, 4, 6
tbl_byte:
    .byte 0, 8, 16, 24
    .balign 2
tbl_short:
    .short 0, 8, 16, 24

    .data
g_ops:
    .long fn_times2, fn_times3
# Import slots (mkxex.py writes the record word: type 0 << 24 | ordinal).
slot_krnl_version:
    .long 0
slot_hw_info:
    .long 0
slot_ntalloc:
    .long 0
