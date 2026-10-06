# Original PPC proof of virtual kernel fields and real title termination.
# Input r3 points to a runner-owned guest results block; +0 selects phase0/1.
# Phase0: 18 checks, returns0x72 or0xE600|check. Phase1: terminates with code0.
#_ XEX_ENTRY kernel_boot_entry
#_ XEX_LIBRARY_VERSION xboxkrnl.exe 0x201A1B00 0x201A1B00
#_ XEX_LIBRARY_VERSION xam.xex 0x201A1B00 0x201A1B00
#_ XEX_IMPORT_RECORD var_version xboxkrnl.exe 0x0158
#_ XEX_IMPORT_RECORD var_timestamp xboxkrnl.exe 0x00AD
#_ XEX_IMPORT_RECORD var_thread_type xboxkrnl.exe 0x001B
#_ XEX_IMPORT imp_ObReferenceObjectByHandle xboxkrnl.exe 0x0110
#_ XEX_IMPORT imp_ObDereferenceObject xboxkrnl.exe 0x0105
#_ XEX_IMPORT imp_ExCreateThread xboxkrnl.exe 0x000D
#_ XEX_IMPORT imp_NtResumeThread xboxkrnl.exe 0x00F5
#_ XEX_IMPORT imp_NtWaitForSingleObjectEx xboxkrnl.exe 0x00FD
#_ XEX_IMPORT imp_NtClose xboxkrnl.exe 0x00CF
#_ XEX_IMPORT imp_ExGetXConfigSetting xboxkrnl.exe 0x0010
#_ XEX_IMPORT imp_ExRegisterTitleTerminateNotification xboxkrnl.exe 0x0015
#_ XEX_IMPORT imp_XamLoaderTerminateTitle xam.xex 0x01A9

    .macro ADDRESS reg, symbol
    lis \reg, \symbol@ha
    addi \reg, \reg, \symbol@l
    .endm
    .macro WORD reg, hi, lo
    lis \reg, \hi
    ori \reg, \reg, \lo
    .endm
    .macro STEP number
    li r29, \number
    .endm
    .text
    .globl kernel_boot_entry
kernel_boot_entry:
    mflr r12
    stw r12, -8(r1)
    std r31, -16(r1)
    std r30, -24(r1)
    std r29, -32(r1)
    std r28, -40(r1)
    std r27, -48(r1)
    std r26, -56(r1)
    stwu r1, -320(r1)
    mr r26, r3
    lwz r0, 0(r26)
    cmpwi cr0, r0, 1
    beq cr0, .Lterminate

    ADDRESS r11, var_version
    lwz r27, 0(r11)
    STEP 1
    lhz r0, 0(r27)
    cmpwi cr0, r0, 2
    bne cr0, .Lfail
    STEP 2
    lhz r0, 2(r27)
    cmpwi cr0, r0, 0
    bne cr0, .Lfail
    STEP 3
    lhz r0, 4(r27)
    cmpwi cr0, r0, 6683
    bne cr0, .Lfail
    STEP 4
    lhz r0, 6(r27)
    cmpwi cr0, r0, 0
    bne cr0, .Lfail

    STEP 5
    lwz r27, 0x100(r13)
    cmpwi cr0, r27, 0
    beq cr0, .Lfail
    lwz r31, 0x14C(r27)
    cmpwi cr0, r31, 0
    beq cr0, .Lfail
    stw r31, 4(r26)
    STEP 6
    ADDRESS r11, var_thread_type
    lwz r30, 0(r11)
    li r3, -2
    mr r4, r30
    addi r5, r1, 0x80
    bl imp_ObReferenceObjectByHandle
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    lwz r0, 0x80(r1)
    cmpw cr0, r0, r27
    bne cr0, .Lfail
    STEP 7
    WORD r0, 0xA1B2, 0xC3D4
    stw r0, 0x160(r27)
    lwz r11, 0x160(r27)
    cmpw cr0, r0, r11
    bne cr0, .Lfail

    STEP 8
    addi r3, r1, 0x84
    lis r4, 1
    addi r5, r26, 8
    li r6, 0
    ADDRESS r7, kernel_boot_worker
    mr r8, r26
    li r9, 1
    bl imp_ExCreateThread
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    lwz r28, 0x84(r1)
    STEP 9
    mr r3, r28
    mr r4, r30
    addi r5, r1, 0x88
    bl imp_ObReferenceObjectByHandle
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    lwz r27, 0x88(r1)
    lwz r0, 0x14C(r27)
    lwz r11, 8(r26)
    cmpw cr0, r0, r11
    bne cr0, .Lfail
    cmpw cr0, r0, r31
    beq cr0, .Lfail
    STEP 10
    lwz r0, 0x160(r27)
    cmpwi cr0, r0, 0
    bne cr0, .Lfail
    li r0, 0x456
    stw r0, 0x160(r27)
    STEP 11
    mr r3, r28
    addi r4, r1, 0x8C
    bl imp_NtResumeThread
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    lwz r0, 0x8C(r1)
    cmpwi cr0, r0, 1
    bne cr0, .Lfail
    STEP 12
    mr r3, r28
    li r4, 0
    li r5, 0
    ADDRESS r6, wait_limit
    bl imp_NtWaitForSingleObjectEx
    stw r3, 32(r26)
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    lwz r0, 12(r26)
    stw r0, 36(r26)
    WORD r11, 0xBEEF, 0xC0DE
    cmpw cr0, r0, r11
    bne cr0, .Lfail
    STEP 13
    lwz r11, 0x100(r13)
    lwz r0, 0x160(r11)
    WORD r11, 0xA1B2, 0xC3D4
    cmpw cr0, r0, r11
    bne cr0, .Lfail
    STEP 14
    mr r3, r28
    bl imp_NtClose
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    lwz r0, 0x160(r27)
    WORD r11, 0xBEEF, 0xC0DE
    cmpw cr0, r0, r11
    bne cr0, .Lfail
    STEP 15
    mr r3, r27
    bl imp_ObDereferenceObject
    lwz r3, 0x80(r1)
    bl imp_ObDereferenceObject

    STEP 16
    li r3, 2
    li r4, 2
    addi r5, r1, 0x90
    li r6, 4
    addi r7, r1, 0x94
    bl imp_ExGetXConfigSetting
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    lwz r0, 0x90(r1)
    WORD r11, 0x0040, 0x0400
    cmpw cr0, r0, r11
    bne cr0, .Lfail
    lhz r0, 0x94(r1)
    cmpwi cr0, r0, 4
    bne cr0, .Lfail
    STEP 17
    li r3, 3
    li r4, 10
    addi r5, r1, 0x90
    li r6, 4
    addi r7, r1, 0x94
    bl imp_ExGetXConfigSetting
    cmpwi cr0, r3, 0
    bne cr0, .Lfail
    lwz r0, 0x90(r1)
    lis r11, 1
    cmpw cr0, r0, r11
    bne cr0, .Lfail
    STEP 18
    ADDRESS r11, var_timestamp
    lwz r11, 0(r11)
    lwz r0, 0x10(r11)
.Lwait_tick:
    lwz r12, 0x10(r11)
    cmpw cr0, r0, r12
    beq cr0, .Lwait_tick
    li r3, 0x72
    b .Lreturn
.Lfail:
    ori r3, r29, 0xE600
.Lreturn:
    addi r1, r1, 320
    ld r26, -56(r1)
    ld r27, -48(r1)
    ld r28, -40(r1)
    ld r29, -32(r1)
    ld r30, -24(r1)
    ld r31, -16(r1)
    lwz r12, -8(r1)
    mtlr r12
    blr
.Lterminate:
    ADDRESS r11, result_pointer
    stw r26, 0(r11)
    lwz r11, 0x100(r13)
    lwz r0, 0x14C(r11)
    stw r0, 16(r26)
    ADDRESS r3, notification
    li r4, 1
    bl imp_ExRegisterTitleTerminateNotification
    li r3, 0
    bl imp_XamLoaderTerminateTitle
    li r0, 1
    stw r0, 28(r26)
    li r3, 0x7F
    b .Lreturn

    .globl kernel_boot_worker
kernel_boot_worker:
    lwz r11, 0x100(r13)
    lwz r0, 0x14C(r11)
    lwz r12, 8(r3)
    cmpw cr0, r0, r12
    bne cr0, .Lworker_bad
    lwz r0, 0x160(r11)
    cmpwi cr0, r0, 0x456
    bne cr0, .Lworker_bad
    WORD r0, 0xBEEF, 0xC0DE
    stw r0, 0x160(r11)
    stw r0, 12(r3)
    li r3, 0
    blr
.Lworker_bad:
    li r0, -1
    stw r0, 12(r3)
    li r3, 1
    blr

    .globl kernel_boot_notification
kernel_boot_notification:
    mflr r12
    stw r12, -8(r1)
    stwu r1, -96(r1)
    ADDRESS r11, result_pointer
    lwz r11, 0(r11)
    lwz r10, 0x100(r13)
    lwz r0, 0x14C(r10)
    lwz r12, 16(r11)
    cmpw cr0, r0, r12
    bne cr0, .Lnotify_bad
    lwz r0, 20(r11)
    addi r0, r0, 1
    stw r0, 20(r11)
    # A nested request returns to this same callback, never dispatches twice.
    li r3, 0
    bl imp_XamLoaderTerminateTitle
    ADDRESS r11, result_pointer
    lwz r11, 0(r11)
    WORD r0, 0xCAFE, 0x1234
    stw r0, 24(r11)
    b .Lnotify_done
.Lnotify_bad:
    li r0, -1
    stw r0, 24(r11)
.Lnotify_done:
    addi r1, r1, 96
    lwz r12, -8(r1)
    mtlr r12
    blr

    .macro IMPORT name
    .align 2
    .globl \name
\name:
    .space 16
    .endm
    IMPORT imp_ObReferenceObjectByHandle
    IMPORT imp_ObDereferenceObject
    IMPORT imp_ExCreateThread
    IMPORT imp_NtResumeThread
    IMPORT imp_NtWaitForSingleObjectEx
    IMPORT imp_NtClose
    IMPORT imp_ExGetXConfigSetting
    IMPORT imp_ExRegisterTitleTerminateNotification
    IMPORT imp_XamLoaderTerminateTitle

    .section .rdata,"a",@progbits
    .align 3
wait_limit:
    .quad -50000000
    .data
    .align 2
    .globl var_version, var_timestamp, var_thread_type
var_version: .long 0
var_timestamp: .long 0
var_thread_type: .long 0
result_pointer: .long 0
notification:
    .long kernel_boot_notification, 2, 0, 0
