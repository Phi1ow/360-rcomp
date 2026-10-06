# Calls, branches, loops, indirect dispatch.
rcomp_control_origin:
test_call_direct:
  #_ REGISTER_IN r3 5
  bl fn_double_plus1
  addi r3, r3, 100
  blr
  #_ REGISTER_OUT r3 111

fn_double_plus1:
  add r3, r3, r3
  addi r3, r3, 1
  blr

test_call_indirect:
  #_ REGISTER_IN r3 7
  li r12, fn_triple - rcomp_control_origin
  mtctr r12
  bctrl
  addi r3, r3, 1
  blr
  #_ REGISTER_OUT r3 22

fn_triple:
  mulli r3, r3, 3
  blr

test_call_indirect_invalid:
  #_ EXPECT_FATAL indirect_target
  lis r12, 0x1234
  mtctr r12
  bctrl
  blr

test_loop_ctr_sum:
  #_ REGISTER_IN r4 10
  li r3, 0
  li r5, 1
  mtctr r4
.Lsum:
  add r3, r3, r5
  addi r5, r5, 1
  bdnz .Lsum
  blr
  #_ REGISTER_OUT r3 55
  #_ REGISTER_OUT r5 11

test_max_signed:
  #_ REGISTER_IN r3 0xFFFFFFFFFFFFFFFE
  #_ REGISTER_IN r4 3
  cmpw cr0, r3, r4
  bge .Lmax_s_keep
  mr r3, r4
.Lmax_s_keep:
  blr
  #_ REGISTER_OUT r3 3

test_max_unsigned:
  #_ REGISTER_IN r3 0xFFFFFFFFFFFFFFFE
  #_ REGISTER_IN r4 3
  cmplw cr0, r3, r4
  bge .Lmax_u_keep
  mr r3, r4
.Lmax_u_keep:
  blr
  #_ REGISTER_OUT r3 0xFFFFFFFFFFFFFFFE

test_trap_eq_taken:
  #_ EXPECT_FATAL guest_trap
  li r3, 5
  twi 4, r3, 5
  blr

test_trap_eq_not_taken:
  li r3, 6
  twi 4, r3, 5
  blr
  #_ REGISTER_OUT r3 6

test_trap_unsigned_imm_taken:
  #_ EXPECT_FATAL guest_trap
  #_ REGISTER_IN r3 0xFFFFFFF0
  twi 1, r3, -32
  blr

test_trap_signed_imm_not_taken:
  #_ REGISTER_IN r3 0xFFFFFFF0
  twi 8, r3, -8
  blr
  #_ REGISTER_OUT r3 0xFFFFFFF0

test_trap_dword_lt_not_taken:
  #_ REGISTER_IN r3 0x0000000100000000
  #_ REGISTER_IN r4 2
  td 16, r3, r4
  blr
  #_ REGISTER_OUT r3 0x0000000100000000

test_trap_word_lt_taken:
  #_ EXPECT_FATAL guest_trap
  #_ REGISTER_IN r3 0x0000000100000000
  #_ REGISTER_IN r4 2
  tw 16, r3, r4
  blr

# mftb (cpu/patches/xenonrecomp/0007): the timebase advances, and by less than
# one second of 50 MHz ticks over a short loop. The rate itself is checked by
# cpu/tests/test_timebase.cpp. Encoded as the classic mftb opcode (31/371,
# TBR 268) that Xbox 360 compilers emit: GNU as would write "mftb" as
# mfspr rD,268, which XenonRecomp @ddd128b does not translate ("mfsprg").
test_mftb_monotonic:
  .long 0x7C8C42E6  # mftb r4
  li r6, 1000
  mtctr r6
.Lmftb_spin:
  bdnz .Lmftb_spin
  .long 0x7CAC42E6  # mftb r5
  subf r7, r4, r5
  li r3, 0
  cmpdi r7, 0
  ble .Lmftb_done
  lis r8, 0x2FA
  ori r8, r8, 0xF080
  cmpd r7, r8
  bge .Lmftb_done
  li r3, 1
.Lmftb_done:
  blr
  #_ REGISTER_OUT r3 1
