# Original module/bootstrap oracle; no game bytes or substituted HLE.
# Success 0x6F; failure 0xE200 | the numbered check. r3 selects launch 0/1.
#_ XEX_ENTRY modules_entry
#_ XEX_IMPORT imp_RtlImageXexHeaderField xboxkrnl.exe 0x012B
#_ XEX_IMPORT imp_XexCheckExecutablePrivilege xboxkrnl.exe 0x0194
#_ XEX_IMPORT imp_XexGetModuleHandle xboxkrnl.exe 0x0195
#_ XEX_IMPORT imp_XexGetProcedureAddress xboxkrnl.exe 0x0197
#_ XEX_IMPORT imp_RtlCompareStringN xboxkrnl.exe 0x011D
#_ XEX_IMPORT_RECORD var_DebugMonitor xboxkrnl.exe 0x0059
#_ XEX_IMPORT_RECORD var_MainModule xboxkrnl.exe 0x0193
#_ XEX_IMPORT_RECORD var_CommandLine xboxkrnl.exe 0x01AE
#_ XEX_IMPORT_RECORD var_CertMonitor xboxkrnl.exe 0x0266
#_ XEX_HEADER_VALUE 0xCAFE0000 0x12345678
#_ XEX_HEADER_VALUE 0xCAFE0101 0xA1B2C3D4
#_ XEX_HEADER_DATA 0xCAFE0202 header_fixed 8
#_ XEX_HEADER_DATA 0xCAFE03FF header_variable 12
#_ XEX_HEADER_VALUE 0xCAFE0400 0
#_ XEX_HEADER_VALUE 0x00030000 0x00200008

    .macro ADDRESS reg, symbol
    lis \reg, \symbol@ha
    addi \reg, \reg, \symbol@l
    .endm
    .macro KEY reg, hi, lo
    lis \reg, \hi
    ori \reg, \reg, \lo
    .endm
    .macro HEADER hi, lo
    mr r3, r31
    KEY r4, \hi, \lo
    bl imp_RtlImageXexHeaderField
    .endm
    .macro MAIN_BY_NAME offset
    lwz r3, \offset(r28)
    addi r4, r1, 0x80
    bl imp_XexGetModuleHandle
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    lwz r0, 0x80(r1)
    cmpw cr0, r0, r30
    bne cr0, .Lfail
    .endm
    .macro RESOLVE selector
    mr r3, r26
    li r4, \selector
    addi r5, r1, 0x80
    bl imp_XexGetProcedureAddress
    .endm
    .macro CALL_COMPARE
    mtctr r3
    ADDRESS r3, compare_a
    li r4, 5
    ADDRESS r5, compare_b
    li r6, 5
    li r7, 1
    bctrl
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    .endm

    .text
    .globl modules_entry
modules_entry:
    mflr r12
    stw r12, -8(r1)
    std r26, -56(r1)
    std r27, -48(r1)
    std r28, -40(r1)
    std r29, -32(r1)
    std r30, -24(r1)
    std r31, -16(r1)
    stwu r1, -256(r1)
    ADDRESS r28, launch_a
    cmpwi cr0, r3, 0
    beq cr0, .Llaunch_ready
    ADDRESS r28, launch_b
.Llaunch_ready:
    # 1: import slot -> exported BE32 cell -> real main LDR.
    li r29, 1
    ADDRESS r11, var_MainModule
    lwz r27, 0(r11)
    cmpwi cr0, r27, 0
    beq cr0, .Lfail
    lwz r30, 0(r27)
    cmpwi cr0, r30, 0
    beq cr0, .Lfail
    li r3, 0
    addi r4, r1, 0x80
    bl imp_XexGetModuleHandle
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    lwz r0, 0x80(r1)
    cmpw cr0, r0, r30
    bne cr0, .Lfail

    # 2/3: exact guest path and basename, with documented ASCII case folding.
    li r29, 2
    MAIN_BY_NAME 0
    li r29, 3
    MAIN_BY_NAME 4

    # 4: commandline export points directly at configured ASCII, not a cell.
    li r29, 4
    ADDRESS r11, var_CommandLine
    lwz r3, 0(r11)
    li r4, -1
    lwz r5, 8(r28)
    li r6, -1
    li r7, 0
    bl imp_RtlCompareStringN
    cmpwi cr0, r3, 0
    bne cr0, .Lfail

    # 5: absent monitors have exported cells; neither is a fake callback.
    li r29, 5
    ADDRESS r11, var_DebugMonitor
    lwz r11, 0(r11)
    cmpwi cr0, r11, 0
    beq cr0, .Lfail
    lwz r0, 0(r11)
    cmpwi cr0, r0, 0
    bne cr0, .Lfail
    ADDRESS r11, var_CertMonitor
    lwz r11, 0(r11)
    cmpwi cr0, r11, 0
    beq cr0, .Lfail
    lwz r0, 0(r11)
    cmpwi cr0, r0, 0
    bne cr0, .Lfail

    # 6: names are real UTF-16BE descriptors and image/entry are this fixture.
    li r29, 6
    addi r3, r30, 0x24
    lwz r4, 0(r28)
    bl check_unicode_name
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    addi r3, r30, 0x2C
    lwz r4, 12(r28)
    bl check_unicode_name
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    lwz r0, 0x1C(r30)
    lis r11, 0x8200
    cmpw cr0, r0, r11
    bne cr0, .Lfail
    lwz r0, 0x3C(r30)
    ADDRESS r11, modules_entry
    cmpw cr0, r0, r11
    bne cr0, .Lfail

    # 7: XexHeaderBase points to the actual associated XEX2 header.
    li r29, 7
    lwz r31, 0x58(r30)
    cmpwi cr0, r31, 0
    beq cr0, .Lfail
    lwz r0, 0(r31)
    KEY r11, 0x5845, 0x5832
    cmpw cr0, r0, r11
    bne cr0, .Lfail

    # 8/9: low-byte 00 returns immediate value, including zero.
    li r29, 8
    HEADER 0xCAFE, 0x0000
    KEY r11, 0x1234, 0x5678
    cmpw cr0, r3, r11
    bne cr0, .Lfail
    li r29, 9
    HEADER 0xCAFE, 0x0400
    cmpwi cr0, r3, 0
    bne cr0, .Lfail

    # 10: low-byte 01 returns precisely the table value-word address.
    li r29, 10
    mr r3, r31
    KEY r4, 0xCAFE, 0x0101
    bl find_header_slot
    stw r3, 0x88(r1)
    cmpwi cr0, r3, 0
    beq cr0, .Lfail
    HEADER 0xCAFE, 0x0101
    lwz r11, 0x88(r1)
    cmpw cr0, r3, r11
    bne cr0, .Lfail
    lwz r0, 0(r3)
    KEY r11, 0xA1B2, 0xC3D4
    cmpw cr0, r0, r11
    bne cr0, .Lfail

    # 11: fixed block is header+record offset, with both original words.
    li r29, 11
    mr r3, r31
    KEY r4, 0xCAFE, 0x0202
    bl find_header_slot
    cmpwi cr0, r3, 0
    beq cr0, .Lfail
    lwz r11, 0(r3)
    add r11, r31, r11
    stw r11, 0x88(r1)
    HEADER 0xCAFE, 0x0202
    lwz r11, 0x88(r1)
    cmpw cr0, r3, r11
    bne cr0, .Lfail
    lwz r0, 0(r3)
    KEY r11, 0x1357, 0x9BDF
    cmpw cr0, r0, r11
    bne cr0, .Lfail
    lwz r0, 4(r3)
    KEY r11, 0x2468, 0xACE0
    cmpw cr0, r0, r11
    bne cr0, .Lfail

    # 12/13: variable block includes its own length; absent key returns zero.
    li r29, 12
    HEADER 0xCAFE, 0x03FF
    cmpwi cr0, r3, 0
    beq cr0, .Lfail
    lwz r0, 0(r3)
    cmpwi cr0, r0, 12
    bne cr0, .Lfail
    lwz r0, 4(r3)
    KEY r11, 0x1020, 0x3040
    cmpw cr0, r0, r11
    bne cr0, .Lfail
    lwz r0, 8(r3)
    KEY r11, 0x5060, 0x7080
    cmpw cr0, r0, r11
    bne cr0, .Lfail
    li r29, 13
    HEADER 0xCAFE, 0x0500
    cmpwi cr0, r3, 0
    bne cr0, .Lfail

    # 14: privilege tests actual system_flags 0x00200008, plus out-of-range.
    li r29, 14
    li r3, 3
    bl imp_XexCheckExecutablePrivilege
    cmpwi cr0, r3, 1
    bne cr0, .Lfail
    li r3, 21
    bl imp_XexCheckExecutablePrivilege
    cmpwi cr0, r3, 1
    bne cr0, .Lfail
    li r3, 2
    bl imp_XexCheckExecutablePrivilege
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    li r3, 32
    bl imp_XexCheckExecutablePrivilege
    cmpwi cr0, r3, 0
    bne cr0, .Lfail

    # 15: absent module clears the output and returns Win32 ERROR_NOT_FOUND.
    li r29, 15
    li r0, -1
    stw r0, 0x80(r1)
    ADDRESS r3, absent_name
    addi r4, r1, 0x80
    bl imp_XexGetModuleHandle
    cmpwi cr0, r3, 1168
    bne cr0, .Lfail
    lwz r0, 0x80(r1)
    cmpwi cr0, r0, 0
    bne cr0, .Lfail

    # 16: real kernel module, with a distinct guest module identity.
    li r29, 16
    ADDRESS r3, kernel_name
    addi r4, r1, 0x80
    bl imp_XexGetModuleHandle
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    lwz r26, 0x80(r1)
    cmpwi cr0, r26, 0
    beq cr0, .Lfail
    cmpw cr0, r26, r30
    beq cr0, .Lfail

    # 17/18: ordinal and name resolve to the imported AOT thunk; execute both.
    li r29, 17
    RESOLVE 0x011D
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    lwz r3, 0x80(r1)
    ADDRESS r11, imp_RtlCompareStringN
    cmpw cr0, r3, r11
    bne cr0, .Lfail
    CALL_COMPARE
    li r29, 18
    mr r3, r26
    ADDRESS r4, compare_name
    addi r5, r1, 0x80
    bl imp_XexGetProcedureAddress
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    lwz r3, 0x80(r1)
    ADDRESS r11, imp_RtlCompareStringN
    cmpw cr0, r3, r11
    bne cr0, .Lfail
    CALL_COMPARE

    # 19: resolving a variable returns exported storage, not its BE32 value.
    li r29, 19
    RESOLVE 0x0193
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    lwz r11, 0x80(r1)
    cmpw cr0, r11, r27
    bne cr0, .Lfail
    lwz r0, 0(r11)
    cmpw cr0, r0, r30
    bne cr0, .Lfail

    # 20: missing export clears output; invalid module preserves output.
    li r29, 20
    mr r3, r26
    ADDRESS r4, absent_name
    addi r5, r1, 0x80
    li r0, -1
    stw r0, 0x80(r1)
    bl imp_XexGetProcedureAddress
    KEY r11, 0xC000, 0x0263
    cmpw cr0, r3, r11
    bne cr0, .Lfail
    lwz r0, 0x80(r1)
    cmpwi cr0, r0, 0
    bne cr0, .Lfail
    li r29, 21
    KEY r3, 0xDEAD, 0xBEEF
    li r4, 0x011D
    addi r5, r1, 0x80
    KEY r0, 0x1357, 0x2468
    stw r0, 0x80(r1)
    bl imp_XexGetProcedureAddress
    KEY r11, 0xC000, 0x0008
    cmpw cr0, r3, r11
    bne cr0, .Lfail
    lwz r0, 0x80(r1)
    KEY r11, 0x1357, 0x2468
    cmpw cr0, r0, r11
    bne cr0, .Lfail

    li r3, 0x6F
    b .Lreturn
.Lfail:
    ori r3, r29, 0xE200
.Lreturn:
    addi r1, r1, 256
    lwz r12, -8(r1)
    mtlr r12
    ld r26, -56(r1)
    ld r27, -48(r1)
    ld r28, -40(r1)
    ld r29, -32(r1)
    ld r30, -24(r1)
    ld r31, -16(r1)
    blr

    # Independent table walk yields an address, never the expected HLE value.
    .globl find_header_slot
find_header_slot:
    lwz r5, 20(r3)
    addi r6, r3, 24
.Lheader_scan:
    cmpwi cr0, r5, 0
    beq cr0, .Lheader_absent
    lwz r7, 0(r6)
    cmpw cr0, r7, r4
    beq cr0, .Lheader_found
    addi r6, r6, 8
    addi r5, r5, -1
    b .Lheader_scan
.Lheader_found:
    addi r3, r6, 4
    blr
.Lheader_absent:
    li r3, 0
    blr

    # Compare descriptor UTF-16BE against our own short ASCII literal,
    # including actual NUL and byte-counted Length/MaximumLength.
    .globl check_unicode_name
check_unicode_name:
    lhz r5, 0(r3)
    lhz r6, 2(r3)
    lwz r7, 4(r3)
    li r8, 0
.Lunicode_scan:
    lbz r9, 0(r4)
    lhz r10, 0(r7)
    cmpw cr0, r9, r10
    bne cr0, .Lunicode_bad
    cmpwi cr0, r9, 0
    beq cr0, .Lunicode_end
    addi r8, r8, 2
    addi r4, r4, 1
    addi r7, r7, 2
    b .Lunicode_scan
.Lunicode_end:
    cmpw cr0, r8, r5
    bne cr0, .Lunicode_bad
    addi r8, r8, 2
    cmplw cr0, r6, r8
    blt cr0, .Lunicode_bad
    li r3, 0
    blr
.Lunicode_bad:
    li r3, 1
    blr

    .globl imp_RtlImageXexHeaderField
imp_RtlImageXexHeaderField: .long 0, 0, 0, 0
    .globl imp_XexCheckExecutablePrivilege
imp_XexCheckExecutablePrivilege: .long 0, 0, 0, 0
    .globl imp_XexGetModuleHandle
imp_XexGetModuleHandle: .long 0, 0, 0, 0
    .globl imp_XexGetProcedureAddress
imp_XexGetProcedureAddress: .long 0, 0, 0, 0
    .globl imp_RtlCompareStringN
imp_RtlCompareStringN: .long 0, 0, 0, 0

    .section .rodata
path_a: .asciz "game:\\rcomp_modules.xex"
base_a: .asciz "rcomp_modules.xex"
case_a: .asciz "RCOMP_MODULES.XEX"
command_a: .asciz "rcomp_modules.xex --original alpha"
path_b: .asciz "game:\\sub\\rcomp_modules_alt.xex"
base_b: .asciz "rcomp_modules_alt.xex"
case_b: .asciz "RCOMP_MODULES_ALT.XEX"
command_b: .asciz "rcomp_modules_alt.xex --original beta=2"
kernel_name: .asciz "xboxkrnl.exe"
compare_name: .asciz "RtlCompareStringN"
absent_name: .asciz "original_fixture_missing"
compare_a: .asciz "MiXeD"
compare_b: .asciz "mixed"
    .balign 4
launch_a: .long path_a, case_a, command_a, base_a
launch_b: .long path_b, case_b, command_b, base_b
header_fixed: .long 0x13579BDF, 0x2468ACE0
header_variable: .long 12, 0x10203040, 0x50607080
    .data
    .balign 4
var_DebugMonitor: .long 0
var_MainModule: .long 0
var_CommandLine: .long 0
var_CertMonitor: .long 0
