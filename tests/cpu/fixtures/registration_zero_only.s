# Original single-function fixture: indirect registry must be empty.
test_direct_zero_only:
  li r3, 11
  blr
  #_ REGISTER_OUT r3 11
