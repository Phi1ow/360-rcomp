# M3 mini guest program (original). Runs under the guest runtime: guest stack
# (r1/r13 from rcomp::rt::create_guest_thread), xboxkrnl HLE, sandboxed VFS
# with device "game" mounted on fixtures/ppc/m3/data.
#
# 1. NtAllocateVirtualMemory 64 KiB   2. NtOpenFile game:\m3.bin
# 3. NtReadFile 256 bytes into the allocation   4. h = h*31 + b over them
# 5. NtClose   6. NtFreeVirtualMemory   7. return h (r3), bytes read (r4)
# Any failing call returns r3 = 0xFFFFFFFFFFFFFFF0 | step, r4 = NTSTATUS.
#
# Imports: `#_ IMPORT` marks a helper that stands for the import thunk a XEX
# loader would bind; tests/cpu/gen_harness.py overrides the helper's weak
# symbol with a call to the runtime's __imp__<name> (no XEX here).
#
# Data (MEMORY_IN): 0x10001000 ANSI_STRING{Length=12, Max=12, Buffer=0x10001020}
#                   0x10001008 OBJECT_ATTRIBUTES{Root=0, ObjectName=0x10001000, Attributes=0}
#                   0x10001020 "game:\m3.bin"
test_m3_main:
  #_ PROGRAM stack=0x10000 mount=game:data
  #_ MEMORY_IN 10001000 000C000C 10001020 00000000 10001000 00000000
  #_ MEMORY_IN 10001020 67616d65 3a5c6d33 2e62696e
  mflr r12
  stw r12, -8(r1)
  std r30, -24(r1)
  std r31, -16(r1)
  stwu r1, -128(r1)
  mr r31, r1
  # 1. NtAllocateVirtualMemory(&base, &size, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE, 0)
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
  # 2. NtOpenFile(&handle, FILE_READ_DATA|SYNCHRONIZE, &oa, &iosb, FILE_SYNCHRONOUS_IO_NONALERT)
  addi r3, r31, 0x58
  lis r4, 0x10
  ori r4, r4, 1
  lis r5, 0x1000
  ori r5, r5, 0x1008
  addi r6, r31, 0x60
  li r7, 0x20
  bl fn_imp_NtOpenFile
  li r9, 2
  cmpwi cr0, r3, 0
  bne cr0, .Lfail
  # 3. NtReadFile(handle, 0, 0, 0, &iosb, base, 256, NULL)
  lwz r3, 0x58(r31)
  li r4, 0
  li r5, 0
  li r6, 0
  addi r7, r31, 0x60
  lwz r8, 0x50(r31)
  li r9, 256
  li r10, 0
  bl fn_imp_NtReadFile
  li r9, 3
  cmpwi cr0, r3, 0
  bne cr0, .Lfail
  # 4. h = h * 31 + b over IoStatusBlock.Information bytes
  lwz r5, 0x64(r31)
  lwz r6, 0x50(r31)
  li r30, 0
  cmpwi cr0, r5, 0
  beq cr0, .Lhashed
  mtctr r5
.Lhash:
  lbz r8, 0(r6)
  mulli r30, r30, 31
  add r30, r30, r8
  addi r6, r6, 1
  bdnz .Lhash
.Lhashed:
  # 5. NtClose(handle)
  lwz r3, 0x58(r31)
  bl fn_imp_NtClose
  li r9, 5
  cmpwi cr0, r3, 0
  bne cr0, .Lfail
  # 6. NtFreeVirtualMemory(&base, &size=0, MEM_RELEASE, 0)
  li r0, 0
  stw r0, 0x54(r31)
  addi r3, r31, 0x50
  addi r4, r31, 0x54
  lis r5, 0
  ori r5, r5, 0x8000
  li r6, 0
  bl fn_imp_NtFreeVirtualMemory
  li r9, 6
  cmpwi cr0, r3, 0
  bne cr0, .Lfail
  # 7. result
  clrldi r3, r30, 32
  lwz r4, 0x64(r31)
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
  #_ REGISTER_OUT r3 0x66E7B380
  #_ REGISTER_OUT r4 256

fn_imp_NtAllocateVirtualMemory:
  #_ IMPORT xboxkrnl.exe NtAllocateVirtualMemory
  blr

fn_imp_NtOpenFile:
  #_ IMPORT xboxkrnl.exe NtOpenFile
  blr

fn_imp_NtReadFile:
  #_ IMPORT xboxkrnl.exe NtReadFile
  blr

fn_imp_NtClose:
  #_ IMPORT xboxkrnl.exe NtClose
  blr

fn_imp_NtFreeVirtualMemory:
  #_ IMPORT xboxkrnl.exe NtFreeVirtualMemory
  blr
