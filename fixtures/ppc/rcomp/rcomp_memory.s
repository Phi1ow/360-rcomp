# Big-endian loads and stores.
test_be_load_store:
  #_ MEMORY_IN 10001000 11 22 33 44 85 66 77 88
  #_ REGISTER_IN r4 0x10001000
  lwz r3, 0(r4)
  lhz r5, 4(r4)
  lha r9, 4(r4)
  lbz r6, 7(r4)
  ld r7, 0(r4)
  lwbrx r10, 0, r4
  stw r3, 16(r4)
  sth r5, 20(r4)
  stb r6, 22(r4)
  std r7, 24(r4)
  blr
  #_ REGISTER_OUT r3 0x11223344
  #_ REGISTER_OUT r5 0x8566
  #_ REGISTER_OUT r9 0xFFFFFFFFFFFF8566
  #_ REGISTER_OUT r6 0x88
  #_ REGISTER_OUT r7 0x1122334485667788
  #_ REGISTER_OUT r10 0x44332211
  #_ MEMORY_OUT 10001010 11 22 33 44 85 66 88 00 11 22 33 44 85 66 77 88

test_store_update_loop:
  #_ REGISTER_IN r4 0x10000FFC
  li r5, 1
  li r6, 4
  mtctr r6
.Lstore:
  stwu r5, 4(r4)
  addi r5, r5, 1
  bdnz .Lstore
  blr
  #_ REGISTER_OUT r4 0x1000100C
  #_ MEMORY_OUT 10001000 00 00 00 01 00 00 00 02 00 00 00 03 00 00 00 04
