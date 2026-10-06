# Original calendar and suspended-thread oracle. No game data or HLE doubles.
# Windows ntdll observations: rcomp_calendar_oracle.json. Xbox/PS5 execution
# remains separate evidence. Return 0x70 after 45 checks, else 0xE400|check.
#_ XEX_ENTRY calendar_threads_entry
#_ XEX_IMPORT imp_RtlTimeFieldsToTime xboxkrnl.exe 0x013F
#_ XEX_IMPORT imp_RtlTimeToTimeFields xboxkrnl.exe 0x0140
#_ XEX_IMPORT imp_ExCreateThread xboxkrnl.exe 0x000D
#_ XEX_IMPORT imp_NtResumeThread xboxkrnl.exe 0x00F5
#_ XEX_IMPORT imp_NtWaitForSingleObjectEx xboxkrnl.exe 0x00FD
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
    .globl calendar_threads_entry
calendar_threads_entry:
    mflr r12
    stw r12, -8(r1)
    std r28, -40(r1)
    std r29, -32(r1)
    std r30, -24(r1)
    std r31, -16(r1)
    stwu r1, -256(r1)
    li r29, 1
    ADDRESS r31, calendar_success
    li r30, 7
.Ldate:
    # Checks1..14: full known ticks and all eight inverse fields, with guards.
    GUARDS 0x7C, 0x88
    mr r3, r31
    addi r4, r1, 0x80
    bl imp_RtlTimeFieldsToTime
    cmpwi cr0, r3, 1
    bne cr0, .Lfail
    lwz r0, 0x80(r1)
    lwz r11, 16(r31)
    cmpw cr0, r0, r11
    bne cr0, .Lfail
    lwz r0, 0x84(r1)
    lwz r11, 20(r31)
    cmpw cr0, r0, r11
    bne cr0, .Lfail
    CHECK_GUARDS 0x7C, 0x88
    addi r29, r29, 1
    GUARDS 0x8C, 0xA0
    addi r3, r31, 16
    addi r4, r1, 0x90
    bl imp_RtlTimeToTimeFields
    addi r10, r31, 24
    addi r11, r1, 0x90
    li r9, 4
    mtctr r9
.Lroundtrip_words:
    lwz r0, 0(r10)
    lwz r9, 0(r11)
    cmpw cr0, r0, r9
    bne cr0, .Lfail
    addi r10, r10, 4
    addi r11, r11, 4
    bdnz .Lroundtrip_words
    CHECK_GUARDS 0x8C, 0xA0
    addi r29, r29, 1
    addi r31, r31, 40
    addi r30, r30, -1
    cmpwi cr0, r30, 0
    bne cr0, .Ldate

    # Checks15..30: invalid calendar fields return FALSE without output writes.
    ADDRESS r31, calendar_invalid
    li r30, 16
.Linvalid_date:
    GUARDS 0x7C, 0x88
    KEY r0, 0x0123, 0x4567
    stw r0, 0x80(r1)
    KEY r0, 0x89AB, 0xCDEF
    stw r0, 0x84(r1)
    mr r3, r31
    addi r4, r1, 0x80
    bl imp_RtlTimeFieldsToTime
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    lwz r0, 0x80(r1)
    KEY r11, 0x0123, 0x4567
    cmpw cr0, r0, r11
    bne cr0, .Lfail
    lwz r0, 0x84(r1)
    KEY r11, 0x89AB, 0xCDEF
    cmpw cr0, r0, r11
    bne cr0, .Lfail
    CHECK_GUARDS 0x7C, 0x88
    addi r29, r29, 1
    addi r31, r31, 16
    addi r30, r30, -1
    cmpwi cr0, r30, 0
    bne cr0, .Linvalid_date

    # Checks31..39: 100ns fractions, ms/second/day edges, and signed INT64_MAX.
    ADDRESS r31, calendar_inverse
    li r30, 9
.Linverse:
    GUARDS 0x8C, 0xA0
    mr r3, r31
    addi r4, r1, 0x90
    bl imp_RtlTimeToTimeFields
    addi r10, r31, 8
    addi r11, r1, 0x90
    li r9, 4
    mtctr r9
.Linverse_words:
    lwz r0, 0(r10)
    lwz r9, 0(r11)
    cmpw cr0, r0, r9
    bne cr0, .Lfail
    addi r10, r10, 4
    addi r11, r11, 4
    bdnz .Linverse_words
    CHECK_GUARDS 0x8C, 0xA0
    addi r29, r29, 1
    addi r31, r31, 24
    addi r30, r30, -1
    cmpwi cr0, r30, 0
    bne cr0, .Linverse

    # Check40: create a real suspended worker; marker resets on every run.
    ADDRESS r11, worker_result
    li r0, 0
    stw r0, 0(r11)
    addi r3, r1, 0xA0
    lis r4, 1
    li r5, 0
    li r6, 0
    ADDRESS r7, calendar_worker
    li r8, 0
    li r9, 1
    bl imp_ExCreateThread
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    lwz r28, 0xA0(r1)

    # Check41: unmapped PreviousSuspendCount must fail before resuming.
    addi r29, r29, 1
    mr r3, r28
    lis r4, 0x7000
    bl imp_NtResumeThread
    KEY r11, 0xC000, 0x0005
    cmpw cr0, r3, r11
    bne cr0, .Lfail

    # Check42: the thread is still unsignalled and has not written its marker.
    addi r29, r29, 1
    mr r3, r28
    li r4, 0
    li r5, 0
    ADDRESS r6, timeout_poll
    bl imp_NtWaitForSingleObjectEx
    cmpwi cr0, r3, 0x102
    bne cr0, .Lfail
    ADDRESS r11, worker_result
    lwz r0, 0(r11)
    cmpwi cr0, r0, 0
    bne cr0, .Lfail

    # Check43: valid resume observes original suspend count exactly1.
    addi r29, r29, 1
    mr r3, r28
    addi r4, r1, 0xA4
    bl imp_NtResumeThread
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    lwz r0, 0xA4(r1)
    cmpwi cr0, r0, 1
    bne cr0, .Lfail

    # Check44: bounded wait publishes the actual completed worker result.
    addi r29, r29, 1
    mr r3, r28
    li r4, 0
    li r5, 0
    ADDRESS r6, timeout_five_seconds
    bl imp_NtWaitForSingleObjectEx
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    ADDRESS r11, worker_result
    lwz r0, 0(r11)
    KEY r11, 0x5151, 0xABCD
    cmpw cr0, r0, r11
    bne cr0, .Lfail

    # Check45: close once; the now-stale handle must be rejected.
    addi r29, r29, 1
    mr r3, r28
    bl imp_NtClose
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    mr r3, r28
    bl imp_NtClose
    KEY r11, 0xC000, 0x0008
    cmpw cr0, r3, r11
    bne cr0, .Lfail
    li r3, 0x70
    b .Lreturn
.Lfail:
    ori r3, r29, 0xE400
.Lreturn:
    addi r1, r1, 256
    lwz r12, -8(r1)
    mtlr r12
    ld r28, -40(r1)
    ld r29, -32(r1)
    ld r30, -24(r1)
    ld r31, -16(r1)
    blr

    .globl calendar_worker
calendar_worker:
    ADDRESS r11, worker_result
    KEY r0, 0x5151, 0xABCD
    stw r0, 0(r11)
    li r3, 37
    blr

    .globl imp_RtlTimeFieldsToTime
imp_RtlTimeFieldsToTime: .long 0, 0, 0, 0
    .globl imp_RtlTimeToTimeFields
imp_RtlTimeToTimeFields: .long 0, 0, 0, 0
    .globl imp_ExCreateThread
imp_ExCreateThread: .long 0, 0, 0, 0
    .globl imp_NtResumeThread
imp_NtResumeThread: .long 0, 0, 0, 0
    .globl imp_NtWaitForSingleObjectEx
imp_NtWaitForSingleObjectEx: .long 0, 0, 0, 0
    .globl imp_NtClose
imp_NtClose: .long 0, 0, 0, 0

    .section .rodata
    .balign 8
timeout_poll: .quad 0
timeout_five_seconds: .quad -50000000
    .data
    .balign 4
worker_result: .long 0

# Expected values from original Windows ntdll calls; not Xbox/PS5 evidence.
    .section .rodata
    .balign 8
calendar_success:
    .short 1601, 1, 1, 0, 0, 0, 0, 0
    .quad 0x0000000000000000
    .short 1601, 1, 1, 0, 0, 0, 0, 1
    .short 1970, 1, 1, 0, 0, 0, 0, 0
    .quad 0x019db1ded53e8000
    .short 1970, 1, 1, 0, 0, 0, 0, 4
    .short 2000, 2, 29, 12, 34, 56, 789, -123
    .quad 0x01bf82b162c9fc50
    .short 2000, 2, 29, 12, 34, 56, 789, 2
    .short 2400, 2, 29, 0, 0, 0, 0, 0
    .quad 0x037ff5cdb46a4000
    .short 2400, 2, 29, 0, 0, 0, 0, 2
    .short 9999, 12, 31, 23, 59, 59, 999, 0
    .quad 0x24c85a5ed1c018f0
    .short 9999, 12, 31, 23, 59, 59, 999, 5
    .short 10000, 1, 1, 0, 0, 0, 0, 0
    .quad 0x24c85a5ed1c04000
    .short 10000, 1, 1, 0, 0, 0, 0, 6
    .short 30827, 12, 31, 23, 59, 59, 999, 0
    .quad 0x7fff35f4f06c58f0
    .short 30827, 12, 31, 23, 59, 59, 999, 5
calendar_invalid:
    .short 1900, 2, 29, 0, 0, 0, 0, 0
    .short 2100, 2, 29, 0, 0, 0, 0, 0
    .short 1600, 12, 31, 23, 59, 59, 999, 0
    .short -1, 9, 28, 12, 34, 56, 789, 0
    .short 2026, 0, 28, 12, 34, 56, 789, 0
    .short 2026, 13, 28, 12, 34, 56, 789, 0
    .short 2026, 9, 0, 12, 34, 56, 789, 0
    .short 2026, 9, 32, 12, 34, 56, 789, 0
    .short 2026, 9, 28, -1, 34, 56, 789, 0
    .short 2026, 9, 28, 24, 34, 56, 789, 0
    .short 2026, 9, 28, 12, -1, 56, 789, 0
    .short 2026, 9, 28, 12, 60, 56, 789, 0
    .short 2026, 9, 28, 12, 34, -1, 789, 0
    .short 2026, 9, 28, 12, 34, 60, 789, 0
    .short 2026, 9, 28, 12, 34, 56, -1, 0
    .short 2026, 9, 28, 12, 34, 56, 1000, 0
    .balign 8
calendar_inverse:
    .quad 0x0000000000000000
    .short 1601, 1, 1, 0, 0, 0, 0, 1
    .quad 0x0000000000000001
    .short 1601, 1, 1, 0, 0, 0, 0, 1
    .quad 0x000000000000270F
    .short 1601, 1, 1, 0, 0, 0, 0, 1
    .quad 0x0000000000002710
    .short 1601, 1, 1, 0, 0, 0, 1, 1
    .quad 0x000000000098967F
    .short 1601, 1, 1, 0, 0, 0, 999, 1
    .quad 0x0000000000989680
    .short 1601, 1, 1, 0, 0, 1, 0, 1
    .quad 0x000000C92A69BFFF
    .short 1601, 1, 1, 23, 59, 59, 999, 1
    .quad 0x000000C92A69C000
    .short 1601, 1, 2, 0, 0, 0, 0, 2
    .quad 0x7FFFFFFFFFFFFFFF
    .short 30828, 9, 14, 2, 48, 5, 477, 4
