# M5 first integrated graphics slice (original). A recompiled guest program
# computes a triangle and a colour and draws them through the identified
# render calls of include/rcomp/gfx.h (HLE, option A), which render with
# Vulkan; the runner reads the presented image back and checks it against
# GFX_EXPECT (oracle written from the data below, not from the program).
#
# Data (MEMORY_IN):
#   0x10001000  6 x int32 pixel coordinates: A(10,6) B(58,20) C(20,54)
#   0x10001020  float 32.0, float 1.0 (NDC = px / 32 - 1 on a 64x64 target)
#   0x10001030  colour bytes 33 99 FF FF (R, G, B, A)
# Steps: NtAllocateVirtualMemory (vertex buffer), gfx_Clear(0x00FF00FF),
# int -> double -> NDC -> single for each coordinate (extsw/std/lfd/fcfid/
# fdiv/fsub/frsp/stfs), colour packed with rlwimi, gfx_DrawTriangles,
# gfx_Present, NtFreeVirtualMemory; r3 = 0 on success, 0xFFFF...FFn on
# failure of step n (r4 = the failing status).
test_m5_triangle:
  #_ PROGRAM stack=0x10000
  #_ GFX_EXPECT size=64x64 clear=00FF00FF tri=10,6,58,20,20,54 colour=3399FFFF frames=1 draws=1
  #_ MEMORY_IN 10001000 0000000A 00000006 0000003A 00000014 00000014 00000036
  #_ MEMORY_IN 10001020 42000000 3F800000
  #_ MEMORY_IN 10001030 3399FFFF
  mflr r12
  stw r12, -8(r1)
  std r30, -24(r1)
  std r31, -16(r1)
  stwu r1, -128(r1)
  mr r31, r1
  # 1. vertex buffer: NtAllocateVirtualMemory(&base, &size, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE, 0)
  li r0, 0
  stw r0, 0x50(r31)
  lis r0, 1
  stw r0, 0x54(r31)
  addi r3, r31, 0x50
  addi r4, r31, 0x54
  li r5, 0x3000
  li r6, 4
  li r7, 0
  bl fn_imp_NtAllocateVirtualMemory
  li r9, 1
  cmpwi cr0, r3, 0
  bne cr0, .Lfail
  # 2. gfx_Clear(0x00FF00FF)
  lis r3, 0x00FF
  ori r3, r3, 0x00FF
  bl fn_gfx_Clear
  li r9, 2
  cmpwi cr0, r3, 0
  bne cr0, .Lfail
  # 3. 6 coordinates -> NDC floats in the buffer
  lis r9, 0x1000
  ori r9, r9, 0x1020
  lfs f2, 0(r9)
  lfs f3, 4(r9)
  lis r7, 0x1000
  ori r7, r7, 0x1000
  lwz r8, 0x50(r31)
  li r0, 6
  mtctr r0
.Lconv:
  lwz r5, 0(r7)
  extsw r5, r5
  std r5, 0x70(r31)
  lfd f1, 0x70(r31)
  fcfid f1, f1
  fdiv f1, f1, f2
  fsub f1, f1, f3
  frsp f1, f1
  stfs f1, 0(r8)
  addi r7, r7, 4
  addi r8, r8, 4
  bdnz .Lconv
  # 4. colour 0xRRGGBBAA from bytes
  lis r9, 0x1000
  ori r9, r9, 0x1030
  lbz r4, 0(r9)
  lbz r5, 1(r9)
  lbz r6, 2(r9)
  lbz r10, 3(r9)
  slwi r30, r4, 24
  rlwimi r30, r5, 16, 8, 15
  rlwimi r30, r6, 8, 16, 23
  or r30, r30, r10
  # 5. gfx_DrawTriangles(buffer, 3, colour)
  lwz r3, 0x50(r31)
  li r4, 3
  mr r5, r30
  bl fn_gfx_DrawTriangles
  li r9, 5
  cmpwi cr0, r3, 0
  bne cr0, .Lfail
  # 6. gfx_Present(0)
  li r3, 0
  bl fn_gfx_Present
  li r9, 6
  cmpwi cr0, r3, 0
  bne cr0, .Lfail
  # 7. NtFreeVirtualMemory(&base, &size=0, MEM_RELEASE, 0)
  li r0, 0
  stw r0, 0x54(r31)
  addi r3, r31, 0x50
  addi r4, r31, 0x54
  lis r5, 0
  ori r5, r5, 0x8000
  li r6, 0
  bl fn_imp_NtFreeVirtualMemory
  li r9, 7
  cmpwi cr0, r3, 0
  bne cr0, .Lfail
  li r3, 0
  li r4, 0
  b .Lret
.Lfail:
  mr r4, r3
  li r3, -16
  or r3, r3, r9
.Lret:
  addi r1, r1, 128
  lwz r12, -8(r1)
  mtlr r12
  ld r30, -24(r1)
  ld r31, -16(r1)
  blr
  #_ REGISTER_OUT r3 0
  #_ REGISTER_OUT r4 0

fn_imp_NtAllocateVirtualMemory:
  #_ IMPORT xboxkrnl.exe NtAllocateVirtualMemory
  blr

fn_imp_NtFreeVirtualMemory:
  #_ IMPORT xboxkrnl.exe NtFreeVirtualMemory
  blr

fn_gfx_Clear:
  #_ HLE gfx_Clear
  blr

fn_gfx_DrawTriangles:
  #_ HLE gfx_DrawTriangles
  blr

fn_gfx_Present:
  #_ HLE gfx_Present
  blr
