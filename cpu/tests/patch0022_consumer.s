# Original regression for generator patch 0022: a module that imports a function
# of another recompiled module (Provider.dll ordinal 1: thunk and import slot),
# a kernel function and a function of a library no module provides. No game bytes.
#_ XEX_ENTRY consumer_entry
#_ XEX_IMPORT imp_provider_add Provider.dll 0x0001
#_ XEX_IMPORT_RECORD slot_provider_add Provider.dll 0x0001
#_ XEX_IMPORT imp_kernel_frequency xboxkrnl.exe 0x0083
#_ XEX_IMPORT imp_missing Missing.dll 0x0007

.text
.globl consumer_entry
consumer_entry:
    li r3, 2
    blr

# r3 = Provider.dll!1(r3, r4) + 100
.globl consumer_call_add
consumer_call_add:
    mflr r12
    stw r12, -8(r1)
    stwu r1, -32(r1)
    bl imp_provider_add
    addi r1, r1, 32
    lwz r12, -8(r1)
    mtlr r12
    addi r3, r3, 100
    blr

# r3 = KeQueryPerformanceFrequency() + 1
.globl consumer_call_kernel
consumer_call_kernel:
    mflr r12
    stw r12, -8(r1)
    stwu r1, -32(r1)
    bl imp_kernel_frequency
    addi r1, r1, 32
    lwz r12, -8(r1)
    mtlr r12
    addi r3, r3, 1
    blr

# Missing.dll ordinal 7: no module provides it.
.globl consumer_call_missing
consumer_call_missing:
    mflr r12
    stw r12, -8(r1)
    stwu r1, -32(r1)
    bl imp_missing
    addi r1, r1, 32
    lwz r12, -8(r1)
    mtlr r12
    blr

    .globl imp_provider_add
imp_provider_add: .long 0, 0, 0, 0
    .globl imp_kernel_frequency
imp_kernel_frequency: .long 0, 0, 0, 0
    .globl imp_missing
imp_missing: .long 0, 0, 0, 0

    .data
    .balign 4
slot_provider_add: .long 0
