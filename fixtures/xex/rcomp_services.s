# Original XAM allocation and file-metadata oracle. No game bytes or HLE doubles.
# The packaged sidecar boot-data.bin is the four original bytes 01 02 03 04 used
# by rcomp_boot.s. Return 0x71 after all checks, else 0xE500|check.
#_ XEX_ENTRY services_entry
#_ XEX_IMPORT imp_XamAlloc xam.xex 0x01EA
#_ XEX_IMPORT imp_XamFree xam.xex 0x01EC
#_ XEX_IMPORT imp_RtlFreeAnsiString xboxkrnl.exe 0x0127
#_ XEX_IMPORT imp_RtlMultiByteToUnicodeN xboxkrnl.exe 0x0133
#_ XEX_IMPORT imp_RtlNtStatusToDosError xboxkrnl.exe 0x0135
#_ XEX_IMPORT imp_RtlUnicodeStringToAnsiString xboxkrnl.exe 0x0142
#_ XEX_IMPORT imp_RtlUnicodeToMultiByteN xboxkrnl.exe 0x0143
#_ XEX_IMPORT imp_RtlUpcaseUnicodeChar xboxkrnl.exe 0x0149
#_ XEX_IMPORT imp_ObDereferenceObject xboxkrnl.exe 0x0105
#_ XEX_IMPORT imp_ObReferenceObject xboxkrnl.exe 0x010F
#_ XEX_IMPORT imp_ObReferenceObjectByHandle xboxkrnl.exe 0x0110
#_ XEX_IMPORT imp_NtOpenFile xboxkrnl.exe 0x00DF
#_ XEX_IMPORT imp_NtReadFile xboxkrnl.exe 0x00F0
#_ XEX_IMPORT imp_NtQueryFullAttributesFile xboxkrnl.exe 0x00E7
#_ XEX_IMPORT imp_NtQueryInformationFile xboxkrnl.exe 0x00E8
#_ XEX_IMPORT imp_NtClose xboxkrnl.exe 0x00CF

    .macro ADDRESS reg, symbol
    lis \reg, \symbol@ha
    addi \reg, \reg, \symbol@l
    .endm
    .macro KEY reg, hi, lo
    lis \reg, \hi
    ori \reg, \reg, \lo
    .endm
    .macro GUARDS before, after
    KEY r0, 0x1122, 0x3344
    stw r0, \before(r1)
    KEY r0, 0x5566, 0x7788
    stw r0, \after(r1)
    .endm
    .macro CHECK_GUARDS before, after
    lwz r0, \before(r1)
    KEY r11, 0x1122, 0x3344
    cmpw cr0, r0, r11
    bne cr0, .Lfail
    lwz r0, \after(r1)
    KEY r11, 0x5566, 0x7788
    cmpw cr0, r0, r11
    bne cr0, .Lfail
    .endm

    .text
    .globl services_entry
services_entry:
    mflr r12
    stw r12, -8(r1)
    std r28, -40(r1)
    std r29, -32(r1)
    std r30, -24(r1)
    std r31, -16(r1)
    stwu r1, -512(r1)
    li r29, 1

    # Check1: XamAlloc publishes a writable guest allocation through out_ptr.
    GUARDS 0x7C, 0x84
    KEY r0, 0xDEAD, 0xBEEF
    stw r0, 0x80(r1)
    li r3, 0
    li r4, 64
    addi r5, r1, 0x80
    bl imp_XamAlloc
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    lwz r31, 0x80(r1)
    cmpwi cr0, r31, 0
    beq cr0, .Lfail
    CHECK_GUARDS 0x7C, 0x84
    KEY r0, 0xA1B2, 0xC3D4
    stw r0, 0(r31)
    lwz r11, 0(r31)
    cmpw cr0, r0, r11
    bne cr0, .Lfail

    # Check2: XamFree releases the exact allocation.
    addi r29, r29, 1
    mr r3, r31
    bl imp_XamFree

    # Check3: CP1252 bytes A, EURO, e-acute, Y-diaeresis convert to exact
    # UTF-16BE code units. The expected vector comes from the independent
    # Windows ntdll oracle used to generate the runtime's pinned tables.
    addi r29, r29, 1
    GUARDS 0x14C, 0x158
    li r0, 0
    stw r0, 0x15C(r1)
    addi r3, r1, 0x150
    li r4, 8
    addi r5, r1, 0x15C
    ADDRESS r6, multibyte_phrase
    li r7, 4
    bl imp_RtlMultiByteToUnicodeN
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    lwz r0, 0x150(r1)
    KEY r11, 0x0041, 0x20AC
    cmpw cr0, r0, r11
    bne cr0, .Lfail
    lwz r0, 0x154(r1)
    KEY r11, 0x00E9, 0x0178
    cmpw cr0, r0, r11
    bne cr0, .Lfail
    lwz r0, 0x15C(r1)
    cmpwi cr0, r0, 8
    bne cr0, .Lfail
    CHECK_GUARDS 0x14C, 0x158

    # Check4: the inverse conversion returns the original four CP1252 bytes.
    addi r29, r29, 1
    GUARDS 0x16C, 0x174
    li r0, 0
    stw r0, 0x178(r1)
    addi r3, r1, 0x170
    li r4, 4
    addi r5, r1, 0x178
    addi r6, r1, 0x150
    li r7, 8
    bl imp_RtlUnicodeToMultiByteN
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    lwz r0, 0x170(r1)
    KEY r11, 0x4180, 0xE99F
    cmpw cr0, r0, r11
    bne cr0, .Lfail
    lwz r0, 0x178(r1)
    cmpwi cr0, r0, 4
    bne cr0, .Lfail
    CHECK_GUARDS 0x16C, 0x174

    # Check5: independent ntdll uppercase oracle U+00E9 -> U+00C9.
    addi r29, r29, 1
    li r3, 0xE9
    bl imp_RtlUpcaseUnicodeChar
    cmpwi cr0, r3, 0xC9
    bne cr0, .Lfail

    # Check6: STATUS_INVALID_HANDLE maps to ERROR_INVALID_HANDLE (6).
    addi r29, r29, 1
    KEY r3, 0xC000, 0x0008
    bl imp_RtlNtStatusToDosError
    cmpwi cr0, r3, 6
    bne cr0, .Lfail

    # Check7: allocate an ANSI_STRING from a counted Unicode descriptor, verify
    # payload and ownership descriptor, then release it and require zero reset.
    addi r29, r29, 1
    GUARDS 0x18C, 0x198
    li r0, 0
    stw r0, 0x190(r1)
    stw r0, 0x194(r1)
    addi r3, r1, 0x190
    ADDRESS r4, unicode_descriptor
    li r5, 1
    bl imp_RtlUnicodeStringToAnsiString
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    lhz r0, 0x190(r1)
    cmpwi cr0, r0, 4
    bne cr0, .Lfail
    lhz r0, 0x192(r1)
    cmpwi cr0, r0, 5
    bne cr0, .Lfail
    lwz r28, 0x194(r1)
    cmpwi cr0, r28, 0
    beq cr0, .Lfail
    lwz r0, 0(r28)
    KEY r11, 0x4180, 0xE99F
    cmpw cr0, r0, r11
    bne cr0, .Lfail
    lbz r0, 4(r28)
    cmpwi cr0, r0, 0
    bne cr0, .Lfail
    addi r3, r1, 0x190
    bl imp_RtlFreeAnsiString
    lwz r0, 0x190(r1)
    cmpwi cr0, r0, 0
    bne cr0, .Lfail
    lwz r0, 0x194(r1)
    cmpwi cr0, r0, 0
    bne cr0, .Lfail
    CHECK_GUARDS 0x18C, 0x198

    # Check8: path metadata for the packaged four-byte file is deterministic
    # apart from host timestamps. Guard words prove the 56-byte structure does
    # not spill; EOF/allocation/attributes are independently known.
    addi r29, r29, 1
    GUARDS 0x9C, 0xD8
    li r0, 0x5A
    addi r10, r1, 0xA0
    li r11, 56
    mtctr r11
.Lpoison_full:
    stb r0, 0(r10)
    addi r10, r10, 1
    bdnz .Lpoison_full
    ADDRESS r3, boot_attributes
    addi r4, r1, 0xA0
    bl imp_NtQueryFullAttributesFile
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    ld r0, 0xC0(r1)       # AllocationSize (+32)
    li r11, 0x200
    cmpd cr0, r0, r11
    bne cr0, .Lfail
    ld r0, 0xC8(r1)       # EndOfFile (+40)
    li r11, 4
    cmpd cr0, r0, r11
    bne cr0, .Lfail
    lwz r0, 0xD0(r1)      # Attributes (+48)
    li r11, 0x81          # READONLY | NORMAL on the read-only game mount
    cmpw cr0, r0, r11
    bne cr0, .Lfail
    CHECK_GUARDS 0x9C, 0xD8

    # Check9: open the same file using the existing production file service.
    addi r29, r29, 1
    li r0, 0
    stw r0, 0x88(r1)
    stw r0, 0x8C(r1)
    stw r0, 0x90(r1)
    addi r3, r1, 0x88
    li r4, 1
    ADDRESS r5, boot_attributes
    addi r6, r1, 0x8C
    li r7, 0
    bl imp_NtOpenFile
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    lwz r30, 0x88(r1)
    cmpwi cr0, r30, 0
    beq cr0, .Lfail

    # Check10: a two-byte read moves the file position to exactly 2.
    addi r29, r29, 1
    mr r3, r30
    li r4, 0
    li r5, 0
    li r6, 0
    addi r7, r1, 0x8C
    addi r8, r1, 0x1D0
    li r9, 2
    li r10, 0
    bl imp_NtReadFile
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    lhz r0, 0x1D0(r1)
    li r11, 0x0102
    cmpw cr0, r0, r11
    bne cr0, .Lfail
    lwz r0, 0x90(r1)
    cmpwi cr0, r0, 2
    bne cr0, .Lfail

    # Check11: FilePositionInformation (class14) reports the live offset and
    # IO_STATUS_BLOCK.Information is exactly the eight output bytes.
    addi r29, r29, 1
    GUARDS 0xFC, 0x108
    KEY r0, 0xCAFE, 0xBABE
    stw r0, 0x100(r1)
    stw r0, 0x104(r1)
    mr r3, r30
    addi r4, r1, 0x8C
    addi r5, r1, 0x100
    li r6, 8
    li r7, 14
    bl imp_NtQueryInformationFile
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    ld r0, 0x100(r1)
    li r11, 2
    cmpd cr0, r0, r11
    bne cr0, .Lfail
    lwz r0, 0x8C(r1)
    cmpwi cr0, r0, 0
    bne cr0, .Lfail
    lwz r0, 0x90(r1)
    cmpwi cr0, r0, 8
    bne cr0, .Lfail
    CHECK_GUARDS 0xFC, 0x108

    # Check12: FileNetworkOpenInformation (class34) matches the path query.
    addi r29, r29, 1
    GUARDS 0x10C, 0x148
    mr r3, r30
    addi r4, r1, 0x8C
    addi r5, r1, 0x110
    li r6, 56
    li r7, 34
    bl imp_NtQueryInformationFile
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    ld r0, 0x130(r1)
    li r11, 0x200
    cmpd cr0, r0, r11
    bne cr0, .Lfail
    ld r0, 0x138(r1)
    li r11, 4
    cmpd cr0, r0, r11
    bne cr0, .Lfail
    lwz r0, 0x140(r1)
    li r11, 0x81
    cmpw cr0, r0, r11
    bne cr0, .Lfail
    lwz r0, 0x90(r1)
    cmpwi cr0, r0, 56
    bne cr0, .Lfail
    CHECK_GUARDS 0x10C, 0x148

    # Check13: close the file once; a second close must see the stale handle.
    addi r29, r29, 1
    mr r3, r30
    bl imp_NtClose
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    mr r3, r30
    bl imp_NtClose
    KEY r11, 0xC000, 0x0008
    cmpw cr0, r3, r11
    bne cr0, .Lfail

    # Check14: reference the real current GuestThread through the documented
    # pseudo-handle. The returned Body is an opaque identity token only.
    addi r29, r29, 1
    GUARDS 0x19C, 0x1A4
    KEY r0, 0xDEAD, 0xBEEF
    stw r0, 0x1A0(r1)
    li r3, -2                 # NtCurrentThread / 0xFFFFFFFE
    li r4, 0                  # untyped lookup; no OBJECT_TYPE layout assumed
    addi r5, r1, 0x1A0
    bl imp_ObReferenceObjectByHandle
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    lwz r28, 0x1A0(r1)
    cmpwi cr0, r28, 0
    beq cr0, .Lfail
    CHECK_GUARDS 0x19C, 0x1A4

    # Check15: the Body reference acquired above survives independently from
    # handles. Add one reference, then release both guest references exactly.
    addi r29, r29, 1
    mr r3, r28
    bl imp_ObReferenceObject
    mr r3, r28
    bl imp_ObDereferenceObject
    mr r3, r28
    bl imp_ObDereferenceObject

    # Check16: an invalid handle reports STATUS_INVALID_HANDLE and must preserve
    # the caller's output word and adjacent sentinels.
    addi r29, r29, 1
    GUARDS 0x1AC, 0x1B4
    KEY r0, 0xDEAD, 0xBEEF
    stw r0, 0x1B0(r1)
    li r3, 0x1337
    li r4, 0
    addi r5, r1, 0x1B0
    bl imp_ObReferenceObjectByHandle
    KEY r11, 0xC000, 0x0008
    cmpw cr0, r3, r11
    bne cr0, .Lfail
    lwz r0, 0x1B0(r1)
    KEY r11, 0xDEAD, 0xBEEF
    cmpw cr0, r0, r11
    bne cr0, .Lfail
    CHECK_GUARDS 0x1AC, 0x1B4

    li r3, 0x71
    b .Lreturn
.Lfail:
    ori r3, r29, 0xE500
.Lreturn:
    addi r1, r1, 512
    lwz r12, -8(r1)
    mtlr r12
    ld r28, -40(r1)
    ld r29, -32(r1)
    ld r30, -24(r1)
    ld r31, -16(r1)
    blr

    .globl imp_XamAlloc
imp_XamAlloc: .long 0, 0, 0, 0
    .globl imp_XamFree
imp_XamFree: .long 0, 0, 0, 0
    .globl imp_RtlFreeAnsiString
imp_RtlFreeAnsiString: .long 0, 0, 0, 0
    .globl imp_RtlMultiByteToUnicodeN
imp_RtlMultiByteToUnicodeN: .long 0, 0, 0, 0
    .globl imp_RtlNtStatusToDosError
imp_RtlNtStatusToDosError: .long 0, 0, 0, 0
    .globl imp_RtlUnicodeStringToAnsiString
imp_RtlUnicodeStringToAnsiString: .long 0, 0, 0, 0
    .globl imp_RtlUnicodeToMultiByteN
imp_RtlUnicodeToMultiByteN: .long 0, 0, 0, 0
    .globl imp_RtlUpcaseUnicodeChar
imp_RtlUpcaseUnicodeChar: .long 0, 0, 0, 0
    .globl imp_ObDereferenceObject
imp_ObDereferenceObject: .long 0, 0, 0, 0
    .globl imp_ObReferenceObject
imp_ObReferenceObject: .long 0, 0, 0, 0
    .globl imp_ObReferenceObjectByHandle
imp_ObReferenceObjectByHandle: .long 0, 0, 0, 0
    .globl imp_NtOpenFile
imp_NtOpenFile: .long 0, 0, 0, 0
    .globl imp_NtReadFile
imp_NtReadFile: .long 0, 0, 0, 0
    .globl imp_NtQueryFullAttributesFile
imp_NtQueryFullAttributesFile: .long 0, 0, 0, 0
    .globl imp_NtQueryInformationFile
imp_NtQueryInformationFile: .long 0, 0, 0, 0
    .globl imp_NtClose
imp_NtClose: .long 0, 0, 0, 0

    .section .rodata
boot_path:
    .ascii "game:/boot-data.bin"
boot_path_end:
    .balign 4
boot_name:
    .short boot_path_end-boot_path, boot_path_end-boot_path
    .long boot_path
boot_attributes:
    .long 0, boot_name, 0
    .balign 4
multibyte_phrase:
    .byte 0x41, 0x80, 0xE9, 0x9F
    .balign 4
unicode_phrase:
    .short 0x0041, 0x20AC, 0x00E9, 0x0178
unicode_descriptor:
    .short 8, 8
    .long unicode_phrase
