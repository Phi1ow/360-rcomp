# Synthetic graphics XEX "title" (original code, no game content), packaged
# by cpu/tools/mkxex.py and recompiled by XenonRecomp in XEX/TOML mode.
#
# It drives the GPU the way a title's Direct3D does, only through kernel
# exports and GPU memory:
#   1. ring (64 KiB, 64 KiB pages, write-combined), read-pointer write-back
#      word and front buffer (256x128x4) from MmAllocatePhysicalMemoryEx
#   2. VdInitializeRingBuffer(MmGetPhysicalAddress(ring), 13),
#      VdEnableRingBufferRPtrWriteBack(wb, 6),
#      VdSetGraphicsInterruptCallback(irq_callback, &g_vblanks)
#   3. the CPU draws the front buffer: texel (x, y) = A 0xFF, R x, G y, B 0x40
#      (D3DFMT_A8R8G8B8, stored big-endian)
#   4. PM4: ME_INIT, then VdSwap fills the 64 dwords after it
#   5. the write pointer (67 dwords) is stored into CP_RB_WPTR in the GPU
#      register window (0x7FC80000 + 4 * 0x1C5), as D3D does
#   6. waits (KeDelayExecutionThread, 1 ms steps, up to 5 s) until the GPU
#      wrote the read pointer back = 67, then until irq_callback counted 3
#      vblanks (the callback is recompiled guest code run by the kernel)
# _start returns 0, or the number of the failed step.
#
#_ XEX_ENTRY _start
#_ XEX_IMPORT imp_MmAllocatePhysicalMemoryEx xboxkrnl.exe 0x00BA
#_ XEX_IMPORT imp_MmGetPhysicalAddress xboxkrnl.exe 0x00BE
#_ XEX_IMPORT imp_VdInitializeRingBuffer xboxkrnl.exe 0x01C3
#_ XEX_IMPORT imp_VdEnableRingBufferRPtrWriteBack xboxkrnl.exe 0x01B6
#_ XEX_IMPORT imp_VdSetGraphicsInterruptCallback xboxkrnl.exe 0x01D5
#_ XEX_IMPORT imp_VdSwap xboxkrnl.exe 0x025B
#_ XEX_IMPORT imp_KeDelayExecutionThread xboxkrnl.exe 0x005A

    .text
    .globl _start
_start:
    mflr r12
    stw r12, -8(r1)
    std r26, -56(r1)
    std r27, -48(r1)
    std r28, -40(r1)
    std r29, -32(r1)
    std r30, -24(r1)
    std r31, -16(r1)
    stwu r1, -256(r1)
    mr r28, r1

    # 1. physical allocations: r31 = ring, r30 = write-back word, r29 = front buffer
    li r3, 0
    lis r4, 1                 # 64 KiB
    lis r5, 0x2000
    ori r5, r5, 0x404         # MEM_LARGE_PAGES | PAGE_WRITECOMBINE | PAGE_READWRITE
    li r6, 0
    li r7, -1
    li r8, 0
    bl imp_MmAllocatePhysicalMemoryEx
    li r27, 1
    cmpwi cr0, r3, 0
    beq cr0, .Lfail
    mr r31, r3
    li r3, 0
    li r4, 0x1000
    li r5, 0x404
    li r6, 0
    li r7, -1
    li r8, 0
    bl imp_MmAllocatePhysicalMemoryEx
    cmpwi cr0, r3, 0
    beq cr0, .Lfail
    mr r30, r3
    li r3, 0
    lis r4, 2                 # 256 * 128 * 4 = 0x20000
    lis r5, 0x2000
    ori r5, r5, 0x404
    li r6, 0
    li r7, -1
    li r8, 0x1000
    bl imp_MmAllocatePhysicalMemoryEx
    cmpwi cr0, r3, 0
    beq cr0, .Lfail
    mr r29, r3

    # 2. ring buffer, read-pointer write-back, interrupt callback
    li r27, 2
    mr r3, r31
    bl imp_MmGetPhysicalAddress
    li r4, 13                 # 1 << (13 + 3) = 64 KiB
    bl imp_VdInitializeRingBuffer
    mr r3, r30
    li r4, 6
    bl imp_VdEnableRingBufferRPtrWriteBack
    lis r3, irq_callback@ha
    addi r3, r3, irq_callback@l
    lis r4, g_vblanks@ha
    addi r4, r4, g_vblanks@l
    bl imp_VdSetGraphicsInterruptCallback

    # 3. front buffer: word i = 0xFF000040 | (x << 16) | (y << 8), x = i & 255, y = i >> 8
    li r5, 0
    lis r6, 0xFF00
    ori r6, r6, 0x40
.Lfill:
    rlwinm r7, r5, 16, 8, 15  # (i & 0xFF) << 16
    rlwinm r8, r5, 0, 17, 23  # (i >> 8 & 0x7F) << 8
    or r7, r7, r8
    or r7, r7, r6
    slwi r9, r5, 2
    stwx r7, r29, r9
    addi r5, r5, 1
    cmplwi cr0, r5, 0x8000
    blt cr0, .Lfill

    # 4. PM4: ME_INIT (3 dwords), then VdSwap's 64 dwords
    lis r0, 0xC001
    ori r0, r0, 0x4800        # type 3, 2 payload dwords, opcode 0x48
    stw r0, 0(r31)
    li r0, 0x3FF
    stw r0, 4(r31)
    li r0, 0
    stw r0, 8(r31)
    # fetch constant of the front buffer (D3D texture header form)
    lis r11, g_fetch@ha
    addi r11, r11, g_fetch@l
    rlwinm r0, r29, 0, 0, 19  # virtual base, 4 KiB aligned
    ori r0, r0, 0x86          # k_8_8_8_8 | 8in32
    stw r0, 4(r11)
    lis r12, g_swap_args@ha
    addi r12, r12, g_swap_args@l
    stw r29, 0(r12)           # *FrontBuffer
    addi r3, r31, 12          # ring space
    mr r4, r11                # fetch constant
    li r5, 0
    li r6, 0
    li r7, 0
    mr r8, r12                # PDWORD FrontBuffer
    addi r9, r12, 4           # PDWORD TextureFormat
    addi r10, r12, 8          # PDWORD ColorSpace
    addi r0, r12, 12
    stw r0, 0x54(r1)          # 9th argument: PDWORD Width
    addi r0, r12, 16
    stw r0, 0x5C(r1)          # 10th argument: PDWORD Height
    li r27, 4
    bl imp_VdSwap

    # 5. kick: CP_RB_WPTR = 67 in the GPU register window
    lis r11, 0x7FC8
    li r0, 67
    sync
    stw r0, 0x714(r11)

    # 6. wait for the read pointer, then for 3 vblank interrupts
    li r27, 6
    li r0, -1
    stw r0, 0x80(r28)         # LARGE_INTEGER -10000 (1 ms, relative)
    li r0, -10000
    stw r0, 0x84(r28)
    li r26, 5000
.Lwait_rptr:
    lwz r0, 0(r30)
    cmpwi cr0, r0, 67
    beq cr0, .Lrptr_ok
    li r3, 1
    li r4, 0
    addi r5, r28, 0x80
    bl imp_KeDelayExecutionThread
    addic. r26, r26, -1
    bne cr0, .Lwait_rptr
    b .Lfail
.Lrptr_ok:
    li r27, 7
    li r26, 5000
    lis r11, g_vblanks@ha
.Lwait_vblank:
    lwz r0, g_vblanks@l(r11)
    cmpwi cr0, r0, 3
    bge cr0, .Lok
    li r3, 1
    li r4, 0
    addi r5, r28, 0x80
    bl imp_KeDelayExecutionThread
    lis r11, g_vblanks@ha
    addic. r26, r26, -1
    bne cr0, .Lwait_vblank
    b .Lfail
.Lok:
    li r27, 0
.Lfail:
    mr r3, r27
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

# Graphics interrupt callback(source, user_data): counts vblanks (source 0)
# in the word user_data points to.
    .globl irq_callback
irq_callback:
    cmpwi cr0, r3, 0
    bnelr cr0
    lwz r5, 0(r4)
    addi r5, r5, 1
    stw r5, 0(r4)
    blr

# Import thunks (16 bytes each; mkxex.py writes the thunk data word).
    .globl imp_MmAllocatePhysicalMemoryEx
imp_MmAllocatePhysicalMemoryEx:
    .long 0, 0, 0, 0
    .globl imp_MmGetPhysicalAddress
imp_MmGetPhysicalAddress:
    .long 0, 0, 0, 0
    .globl imp_VdInitializeRingBuffer
imp_VdInitializeRingBuffer:
    .long 0, 0, 0, 0
    .globl imp_VdEnableRingBufferRPtrWriteBack
imp_VdEnableRingBufferRPtrWriteBack:
    .long 0, 0, 0, 0
    .globl imp_VdSetGraphicsInterruptCallback
imp_VdSetGraphicsInterruptCallback:
    .long 0, 0, 0, 0
    .globl imp_VdSwap
imp_VdSwap:
    .long 0, 0, 0, 0
    .globl imp_KeDelayExecutionThread
imp_KeDelayExecutionThread:
    .long 0, 0, 0, 0

    .data
    .align 2
    .globl g_vblanks
g_vblanks:
    .long 0
# Texture fetch constant of the front buffer: 2D texture, linear, pitch 256
# (8 x 32), k_8_8_8_8, 256 x 128, swizzle ZYXW (Direct3D's A8R8G8B8
# mapping); word 1 (format, endianness, base) is completed at run time.
    .globl g_fetch
g_fetch:
    .long 0x02000002
    .long 0
    .long 0x000FE0FF          # (256 - 1) | (128 - 1) << 13
    .long 0x00000C14          # swizzle (2 | 1 << 3 | 0 << 6 | 3 << 9) << 1
    .long 0
    .long 0x00000200          # 2D
# VdSwap's out parameters: front buffer, texture format, colour space, width, height.
    .globl g_swap_args
g_swap_args:
    .long 0
    .long 6
    .long 0
    .long 256
    .long 128
