# Original PPC and data. Expected worker results: 0x1459AFF4, 0x1459C105.
# Main exits 0x6D; failures 0xE001..E00F. Each guest wait is bounded to 5s.
#_ XEX_ENTRY boot_entry
#_ XEX_TLS boot_tls_template 4 16 4
#_ XEX_IMPORT imp_NtAllocateVirtualMemory xboxkrnl.exe 0x00CC
#_ XEX_IMPORT imp_NtFreeVirtualMemory xboxkrnl.exe 0x00DC
#_ XEX_IMPORT imp_NtOpenFile xboxkrnl.exe 0x00DF
#_ XEX_IMPORT imp_NtReadFile xboxkrnl.exe 0x00F0
#_ XEX_IMPORT imp_NtClose xboxkrnl.exe 0x00CF
#_ XEX_IMPORT imp_ExCreateThread xboxkrnl.exe 0x000D
#_ XEX_IMPORT imp_NtResumeThread xboxkrnl.exe 0x00F5
#_ XEX_IMPORT imp_NtWaitForSingleObjectEx xboxkrnl.exe 0x00FD
#_ XEX_IMPORT imp_ExTerminateThread xboxkrnl.exe 0x0019
#_ XEX_IMPORT_RECORD slot_gpu_clock xboxkrnl.exe 0x01C0

    .macro CHECK_TLS fail
    lwz r11, 0(r13)
    lwz r4, 0(r11)
    lis r5, 0x1357
    ori r5, r5, 0x9BDF
    cmpw cr0, r4, r5
    bne cr0, \fail
    addi r12, r11, 4
    li r0, 7
    mtctr r0
.Ltls_zero_\@:
    lwz r0, 0(r12)
    cmpwi cr0, r0, 0
    bne cr0, \fail
    addi r12, r12, 4
    bdnz .Ltls_zero_\@
    .endm

    .text
    .globl boot_entry
boot_entry:
    mflr r12
    stw r12, -8(r1)
    std r29, -32(r1)
    std r30, -24(r1)
    std r31, -16(r1)
    stwu r1, -256(r1)
    li r29, 1
    lis r11, slot_gpu_clock@ha
    lwz r11, slot_gpu_clock@l(r11)
    lwz r0, 0(r11)
    cmpwi cr0, r0, 500
    bne cr0, .Lboot_fail
    li r29, 2
    CHECK_TLS .Lboot_fail
    mr r31, r11
    lis r0, 0xCAFE
    ori r0, r0, 0xBABE
    stw r0, 0(r31)
    li r29, 3
    li r0, 0
    stw r0, 0x80(r1)
    lis r0, 1
    stw r0, 0x84(r1)
    addi r3, r1, 0x80
    addi r4, r1, 0x84
    li r5, 0x3000
    li r6, 4
    li r7, 0
    bl imp_NtAllocateVirtualMemory
    cmpwi cr0, r3, 0
    bne cr0, .Lboot_fail
    lwz r30, 0x80(r1)
    li r29, 4
    addi r3, r1, 0x88
    li r4, 1
    lis r5, boot_attributes@ha
    addi r5, r5, boot_attributes@l
    addi r6, r1, 0x90
    li r7, 0
    bl imp_NtOpenFile
    cmpwi cr0, r3, 0
    bne cr0, .Lboot_fail
    li r29, 5
    lwz r3, 0x88(r1)
    li r4, 0
    li r5, 0
    li r6, 0
    addi r7, r1, 0x90
    mr r8, r30
    li r9, 4
    li r10, 0
    bl imp_NtReadFile
    cmpwi cr0, r3, 0
    bne cr0, .Lboot_fail
    lwz r0, 0x94(r1)
    cmpwi cr0, r0, 4
    bne cr0, .Lboot_fail
    lwz r0, 0(r30)
    lis r11, 0x0102
    ori r11, r11, 0x0304
    cmpw cr0, r0, r11
    bne cr0, .Lboot_fail
    stw r0, 0x100(r30)
    stw r0, 0x120(r30)
    li r0, 0x1111
    stw r0, 0x104(r30)
    li r0, 0x2222
    stw r0, 0x124(r30)
    li r29, 6
    lwz r3, 0x88(r1)
    bl imp_NtClose
    cmpwi cr0, r3, 0
    bne cr0, .Lboot_fail
    # Both guest TLS allocations coexist before either worker runs.
    li r29, 7
    addi r3, r1, 0x98
    lis r4, 1
    addi r5, r1, 0xA0
    li r6, 0
    lis r7, boot_worker@ha
    addi r7, r7, boot_worker@l
    addi r8, r30, 0x100
    li r9, 1
    bl imp_ExCreateThread
    cmpwi cr0, r3, 0
    bne cr0, .Lboot_fail
    li r29, 8
    addi r3, r1, 0x9C
    lis r4, 1
    addi r5, r1, 0xA4
    li r6, 0
    lis r7, boot_worker@ha
    addi r7, r7, boot_worker@l
    addi r8, r30, 0x120
    li r9, 1
    bl imp_ExCreateThread
    cmpwi cr0, r3, 0
    bne cr0, .Lboot_fail
    li r29, 9
    lwz r3, 0x98(r1)
    li r4, 0
    bl imp_NtResumeThread
    cmpwi cr0, r3, 0
    bne cr0, .Lboot_fail
    lwz r3, 0x9C(r1)
    li r4, 0
    bl imp_NtResumeThread
    cmpwi cr0, r3, 0
    bne cr0, .Lboot_fail
    li r29, 10
    lwz r3, 0x98(r1)
    li r4, 0
    li r5, 0
    lis r6, boot_timeout@ha
    addi r6, r6, boot_timeout@l
    bl imp_NtWaitForSingleObjectEx
    cmpwi cr0, r3, 0
    bne cr0, .Lboot_fail
    lwz r3, 0x9C(r1)
    li r4, 0
    li r5, 0
    lis r6, boot_timeout@ha
    addi r6, r6, boot_timeout@l
    bl imp_NtWaitForSingleObjectEx
    cmpwi cr0, r3, 0
    bne cr0, .Lboot_fail
    li r29, 11
    lwz r0, 0x108(r30)
    lis r11, 0x1459
    ori r11, r11, 0xAFF4
    cmpw cr0, r0, r11
    bne cr0, .Lboot_fail
    lwz r0, 0x128(r30)
    lis r11, 0x1459
    ori r11, r11, 0xC105
    cmpw cr0, r0, r11
    bne cr0, .Lboot_fail
    li r29, 12
    lwz r0, 0x10C(r30)
    lwz r11, 0x12C(r30)
    cmpw cr0, r0, r11
    beq cr0, .Lboot_fail
    cmpw cr0, r0, r31
    beq cr0, .Lboot_fail
    cmpw cr0, r11, r31
    beq cr0, .Lboot_fail
    lwz r0, 0xA0(r1)
    lwz r11, 0xA4(r1)
    cmpw cr0, r0, r11
    beq cr0, .Lboot_fail
    lwz r0, 0(r31)
    lis r11, 0xCAFE
    ori r11, r11, 0xBABE
    cmpw cr0, r0, r11
    bne cr0, .Lboot_fail
    li r29, 13
    lwz r3, 0x98(r1)
    bl imp_NtClose
    cmpwi cr0, r3, 0
    bne cr0, .Lboot_fail
    lwz r3, 0x9C(r1)
    bl imp_NtClose
    cmpwi cr0, r3, 0
    bne cr0, .Lboot_fail
    li r29, 14
    li r0, 0
    stw r0, 0x84(r1)
    addi r3, r1, 0x80
    addi r4, r1, 0x84
    li r5, 0
    ori r5, r5, 0x8000
    li r6, 0
    bl imp_NtFreeVirtualMemory
    cmpwi cr0, r3, 0
    bne cr0, .Lboot_fail
    li r3, 0x6D
    bl imp_ExTerminateThread
    li r29, 15
.Lboot_fail:
    ori r3, r29, 0xE000
    addi r1, r1, 256
    lwz r12, -8(r1)
    mtlr r12
    ld r29, -32(r1)
    ld r30, -24(r1)
    ld r31, -16(r1)
    blr

    .globl boot_worker
boot_worker:
    mr r10, r3
    CHECK_TLS .Lworker_bad
    stw r11, 12(r10)
    lwz r5, 4(r10)
    stw r5, 0(r11)
    lwz r6, 0(r10)
    add r4, r4, r5
    add r4, r4, r6
    stw r4, 8(r10)
    li r3, 0
    b imp_ExTerminateThread
.Lworker_bad:
    li r0, 0xBAD
    stw r0, 8(r10)
    li r3, 1
    b imp_ExTerminateThread

    .globl imp_NtAllocateVirtualMemory
imp_NtAllocateVirtualMemory: .long 0, 0, 0, 0
    .globl imp_NtFreeVirtualMemory
imp_NtFreeVirtualMemory: .long 0, 0, 0, 0
    .globl imp_NtOpenFile
imp_NtOpenFile: .long 0, 0, 0, 0
    .globl imp_NtReadFile
imp_NtReadFile: .long 0, 0, 0, 0
    .globl imp_NtClose
imp_NtClose: .long 0, 0, 0, 0
    .globl imp_ExCreateThread
imp_ExCreateThread: .long 0, 0, 0, 0
    .globl imp_NtResumeThread
imp_NtResumeThread: .long 0, 0, 0, 0
    .globl imp_NtWaitForSingleObjectEx
imp_NtWaitForSingleObjectEx: .long 0, 0, 0, 0
    .globl imp_ExTerminateThread
imp_ExTerminateThread: .long 0, 0, 0, 0

    .section .rodata
    .balign 8
boot_timeout:
    .quad -50000000
boot_tls_template:
    .long 0x13579BDF
boot_path:
    .ascii "game:/boot-data.bin"
boot_path_end:
    .balign 4
boot_name:
    .short boot_path_end-boot_path, boot_path_end-boot_path
    .long boot_path
boot_attributes:
    .long 0, boot_name, 0
    .data
slot_gpu_clock:
    .long 0
