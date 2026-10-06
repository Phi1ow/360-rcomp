# Original pool/Rtl integration oracle. No game bytes or HLE doubles.
# Success is 0x6E; failure is 0xE100 | the numbered check below.
#_ XEX_ENTRY pool_rtl_entry
#_ XEX_IMPORT imp_ExAllocatePoolTypeWithTag xboxkrnl.exe 0x000B
#_ XEX_IMPORT imp_ExFreePool xboxkrnl.exe 0x000F
#_ XEX_IMPORT imp_RtlCompareMemoryUlong xboxkrnl.exe 0x011B
#_ XEX_IMPORT imp_RtlCompareStringN xboxkrnl.exe 0x011D
#_ XEX_IMPORT imp_RtlFillMemoryUlong xboxkrnl.exe 0x0126
#_ XEX_IMPORT imp_RtlInitAnsiString xboxkrnl.exe 0x012C
#_ XEX_IMPORT imp_RtlInitUnicodeString xboxkrnl.exe 0x012D

    .macro ADDRESS reg, symbol
    lis \reg, \symbol@ha
    addi \reg, \reg, \symbol@l
    .endm
    .macro PATTERN reg
    lis \reg, 0x1234
    ori \reg, \reg, 0x5678
    .endm
    .macro STRING_ARGS length, insensitive
    ADDRESS r3, text_mixed
    li r4, \length
    ADDRESS r5, text_lower
    li r6, \length
    li r7, \insensitive
    .endm

    .text
    .globl pool_rtl_entry
pool_rtl_entry:
    mflr r12
    stw r12, -8(r1)
    std r29, -32(r1)
    std r30, -24(r1)
    std r31, -16(r1)
    stwu r1, -256(r1)

    # 1: ABI is size, tag, type. Small pool is writable and 16-byte aligned.
    li r29, 1
    li r3, 64
    lis r4, 0x5465
    ori r4, r4, 0x7374
    li r5, 0
    bl imp_ExAllocatePoolTypeWithTag
    cmpwi cr0, r3, 0
    beq cr0, .Lfail
    andi. r0, r3, 15
    bne cr0, .Lfail
    mr r31, r3

    # 2: Fill exactly four big-endian words; preserve adjacent guards.
    li r29, 2
    lis r0, 0xDEAD
    ori r0, r0, 0xBEEF
    stw r0, 0(r31)
    stw r0, 20(r31)
    li r0, 0
    stw r0, 4(r31)
    stw r0, 8(r31)
    stw r0, 12(r31)
    stw r0, 16(r31)
    addi r3, r31, 4
    li r4, 16
    PATTERN r5
    bl imp_RtlFillMemoryUlong
    addi r11, r31, 4
    li r0, 4
    mtctr r0
    PATTERN r5
.Lfilled_words:
    lwz r0, 0(r11)
    cmpw cr0, r0, r5
    bne cr0, .Lfail
    addi r11, r11, 4
    bdnz .Lfilled_words
    lis r5, 0xDEAD
    ori r5, r5, 0xBEEF
    lwz r0, 0(r31)
    cmpw cr0, r0, r5
    bne cr0, .Lfail
    lwz r0, 20(r31)
    cmpw cr0, r0, r5
    bne cr0, .Lfail
    lbz r0, 4(r31)
    cmpwi cr0, r0, 0x12
    bne cr0, .Lfail

    # 3/4: Compare returns prefix length in bytes, stopping at first mismatch.
    li r29, 3
    addi r3, r31, 4
    li r4, 16
    PATTERN r5
    bl imp_RtlCompareMemoryUlong
    cmpwi cr0, r3, 16
    bne cr0, .Lfail
    li r29, 4
    li r0, 0
    stw r0, 8(r31)
    addi r3, r31, 4
    li r4, 16
    PATTERN r5
    bl imp_RtlCompareMemoryUlong
    cmpwi cr0, r3, 4
    bne cr0, .Lfail

    # 5/6: Empty compare/fill do not access a null source/destination.
    li r29, 5
    li r3, 0
    li r4, 0
    PATTERN r5
    bl imp_RtlCompareMemoryUlong
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    li r29, 6
    li r3, 0
    li r4, 0
    PATTERN r5
    bl imp_RtlFillMemoryUlong

    # 7/8: Page-sized allocation is page aligned; both pools are freed.
    li r29, 7
    li r3, 4096
    lis r4, 0x5465
    ori r4, r4, 0x7374
    li r5, 0
    bl imp_ExAllocatePoolTypeWithTag
    cmpwi cr0, r3, 0
    beq cr0, .Lfail
    andi. r0, r3, 4095
    bne cr0, .Lfail
    mr r30, r3
    li r0, 0x5555
    stw r0, 4092(r30)
    lwz r11, 4092(r30)
    cmpw cr0, r0, r11
    bne cr0, .Lfail
    bl imp_ExFreePool
    li r29, 8
    mr r3, r31
    bl imp_ExFreePool

    # 9: ANSI descriptor uses byte lengths, includes terminator only in Max.
    li r29, 9
    addi r3, r1, 0x80
    ADDRESS r4, text_mixed
    bl imp_RtlInitAnsiString
    lhz r0, 0x80(r1)
    cmpwi cr0, r0, 5
    bne cr0, .Lfail
    lhz r0, 0x82(r1)
    cmpwi cr0, r0, 6
    bne cr0, .Lfail
    lwz r0, 0x84(r1)
    ADDRESS r11, text_mixed
    cmpw cr0, r0, r11
    bne cr0, .Lfail

    # 10: Unicode descriptor lengths are bytes, original UTF-16BE is aliased.
    li r29, 10
    addi r3, r1, 0x88
    ADDRESS r4, text_unicode
    bl imp_RtlInitUnicodeString
    lhz r0, 0x88(r1)
    cmpwi cr0, r0, 6
    bne cr0, .Lfail
    lhz r0, 0x8A(r1)
    cmpwi cr0, r0, 8
    bne cr0, .Lfail
    lwz r0, 0x8C(r1)
    ADDRESS r11, text_unicode
    cmpw cr0, r0, r11
    bne cr0, .Lfail
    lhz r0, 2(r11)
    cmpwi cr0, r0, 0x3A9
    bne cr0, .Lfail

    # 11/12: Null sources reset all descriptor fields, including the pointer.
    li r29, 11
    addi r3, r1, 0x80
    li r4, 0
    bl imp_RtlInitAnsiString
    lwz r0, 0x80(r1)
    cmpwi cr0, r0, 0
    bne cr0, .Lfail
    lwz r0, 0x84(r1)
    cmpwi cr0, r0, 0
    bne cr0, .Lfail
    li r29, 12
    addi r3, r1, 0x88
    li r4, 0
    bl imp_RtlInitUnicodeString
    lwz r0, 0x88(r1)
    cmpwi cr0, r0, 0
    bne cr0, .Lfail
    lwz r0, 0x8C(r1)
    cmpwi cr0, r0, 0
    bne cr0, .Lfail

    # 13/14: Explicit count bounds comparison before a differing suffix.
    li r29, 13
    ADDRESS r3, text_prefix_a
    li r4, 3
    ADDRESS r5, text_prefix_b
    li r6, 3
    li r7, 0
    bl imp_RtlCompareStringN
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    li r29, 14
    ADDRESS r3, text_prefix_a
    li r4, 4
    ADDRESS r5, text_prefix_b
    li r6, 4
    li r7, 0
    bl imp_RtlCompareStringN
    cmpwi cr0, r3, 0
    bge cr0, .Lfail

    # 15/16: ASCII case fold is optional, and return sign is observed.
    li r29, 15
    STRING_ARGS 5, 1
    bl imp_RtlCompareStringN
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    li r29, 16
    STRING_ARGS 5, 0
    bl imp_RtlCompareStringN
    cmpwi cr0, r3, 0
    bge cr0, .Lfail

    # 17: Zero counted lengths do not access null pointers.
    li r29, 17
    li r3, 0
    li r4, 0
    li r5, 0
    li r6, 0
    li r7, 0
    bl imp_RtlCompareStringN
    cmpwi cr0, r3, 0
    bne cr0, .Lfail

    # 18: With an equal prefix the shorter counted string sorts first.
    li r29, 18
    ADDRESS r3, text_prefix_a
    li r4, 3
    ADDRESS r5, text_prefix_b
    li r6, 4
    li r7, 0
    bl imp_RtlCompareStringN
    cmpwi cr0, r3, -1
    bne cr0, .Lfail

    # 19: A counted comparison includes bytes after an embedded NUL.
    li r29, 19
    ADDRESS r3, text_nul_a
    li r4, 3
    ADDRESS r5, text_nul_b
    li r6, 3
    li r7, 0
    bl imp_RtlCompareStringN
    cmpwi cr0, r3, -1
    bne cr0, .Lfail

    # 20: -1, not 0xFFFF, requests the NUL-terminated source length.
    li r29, 20
    STRING_ARGS -1, 1
    bl imp_RtlCompareStringN
    cmpwi cr0, r3, 0
    bne cr0, .Lfail

    # 21/22: Xbox byte uppercasing includes Latin-1 and maps FF to '?'.
    li r29, 21
    ADDRESS r3, text_latin_lower
    li r4, 3
    ADDRESS r5, text_latin_upper
    li r6, 3
    li r7, 1
    bl imp_RtlCompareStringN
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    li r29, 22
    ADDRESS r3, text_ff
    li r4, 1
    ADDRESS r5, text_question
    li r6, 1
    li r7, 1
    bl imp_RtlCompareStringN
    cmpwi cr0, r3, 0
    bne cr0, .Lfail

    li r3, 0x6E
    b .Lreturn
.Lfail:
    ori r3, r29, 0xE100
.Lreturn:
    addi r1, r1, 256
    lwz r12, -8(r1)
    mtlr r12
    ld r29, -32(r1)
    ld r30, -24(r1)
    ld r31, -16(r1)
    blr

    .globl imp_ExAllocatePoolTypeWithTag
imp_ExAllocatePoolTypeWithTag: .long 0, 0, 0, 0
    .globl imp_ExFreePool
imp_ExFreePool: .long 0, 0, 0, 0
    .globl imp_RtlCompareMemoryUlong
imp_RtlCompareMemoryUlong: .long 0, 0, 0, 0
    .globl imp_RtlCompareStringN
imp_RtlCompareStringN: .long 0, 0, 0, 0
    .globl imp_RtlFillMemoryUlong
imp_RtlFillMemoryUlong: .long 0, 0, 0, 0
    .globl imp_RtlInitAnsiString
imp_RtlInitAnsiString: .long 0, 0, 0, 0
    .globl imp_RtlInitUnicodeString
imp_RtlInitUnicodeString: .long 0, 0, 0, 0

    .section .rodata
text_mixed: .asciz "MiXeD"
text_lower: .asciz "mixed"
text_prefix_a: .asciz "abcX"
text_prefix_b: .asciz "abcY"
text_nul_a: .byte 0x61, 0, 0x58
text_nul_b: .byte 0x61, 0, 0x59
text_latin_lower: .byte 0xE0, 0xF6, 0xFE
text_latin_upper: .byte 0xC0, 0xD6, 0xDE
text_ff: .byte 0xFF
text_question: .byte 0x3F
    .balign 2
text_unicode: .short 0x0041, 0x03A9, 0x0062, 0
    .data
    .long 0
