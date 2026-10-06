# Atomic increment with a lwarx/stwcx. retry loop (original).
# r3 = guest address of a 32-bit counter, r4 = iterations.
# Run concurrently by tests/sync/sync_runner.cpp on T host threads, each with
# its own PPCContext over the same guest memory. Oracle: counter = T * r4
# exactly (architectural guarantee of lwarx/stwcx.), and r5 = number of
# stwcx. failures observed by this thread (informational, not checked).
test_atomic_add_loop:
  #_ REGISTER_IN r3 0x10001000
  #_ REGISTER_IN r4 1000
  li r5, 0
  mtctr r4
.Lnext:
  lwarx r6, 0, r3
  addi r6, r6, 1
  stwcx. r6, 0, r3
  beq cr0, .Lstored
  addi r5, r5, 1
  b .Lnext
.Lstored:
  bdnz .Lnext
  sync
  blr
  #_ MEMORY_OUT 10001000 000003E8
