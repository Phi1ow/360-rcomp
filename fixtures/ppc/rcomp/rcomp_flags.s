# CR and XER behaviour.
test_cmpw_cr6:
  #_ REGISTER_IN r3 0xFFFFFFFFFFFFFFFE
  #_ REGISTER_IN r4 3
  cmpw cr6, r3, r4
  blr
  #_ REGISTER_OUT cr 0x0000000000000080

test_cmplw_cr0_gt:
  #_ REGISTER_IN r3 0xFFFFFFFFFFFFFFFE
  #_ REGISTER_IN r4 3
  cmplw cr0, r3, r4
  blr
  #_ REGISTER_OUT cr 0x0000000040000000

test_addic_carry_chain:
  #_ REGISTER_IN r4 1
  addic r3, r4, -1
  adde r5, r6, r7
  blr
  #_ REGISTER_OUT r3 0
  #_ REGISTER_OUT r5 1
  #_ REGISTER_OUT xer_ca 0

test_addic_sets_ca:
  #_ REGISTER_IN r4 1
  addic r3, r4, -1
  blr
  #_ REGISTER_OUT r3 0
  #_ REGISTER_OUT xer_ca 1

test_addic_no_carry:
  #_ REGISTER_IN r4 0
  addic r3, r4, -1
  adde r5, r6, r7
  blr
  #_ REGISTER_OUT r3 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_OUT r5 0
  #_ REGISTER_OUT xer_ca 0

test_add_record_negative:
  #_ REGISTER_IN r4 2
  #_ REGISTER_IN r5 0xFFFFFFFFFFFFFFFB
  add. r3, r4, r5
  blr
  #_ REGISTER_OUT r3 0xFFFFFFFFFFFFFFFD
  #_ REGISTER_OUT cr 0x0000000080000000

test_srawi_carry:
  #_ REGISTER_IN r4 0xFFFFFFFFFFFFFFF9
  srawi r3, r4, 1
  blr
  #_ REGISTER_OUT r3 0xFFFFFFFFFFFFFFFC
  #_ REGISTER_OUT xer_ca 1
