# Original harness regression: zero entry is direct; indirect zero is fatal.
registration_origin:
test_direct_zero:
  li r3, 37
  blr
  #_ REGISTER_OUT r3 37

test_indirect_nonzero:
  li r12, registration_helper - registration_origin
  mtctr r12
  bctrl
  addi r3, r3, 1
  blr
  #_ REGISTER_OUT r3 43

registration_helper:
  li r3, 42
  blr

test_indirect_zero:
  li r12, 0
  mtctr r12
  bctrl
  blr
  #_ EXPECT_FATAL indirect_target
