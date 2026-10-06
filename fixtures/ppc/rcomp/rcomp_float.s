# Floating point: fused multiply-add, rounding to single, compare.
test_fmadd_is_fused:
  #_ REGISTER_IN f1 0x3FF0000000400000
  #_ REGISTER_IN f2 0x3FEFFFFFFF800000
  #_ REGISTER_IN f3 -1.0
  fmadd f4, f1, f2, f3
  blr
  #_ REGISTER_OUT f4 0xBC30000000000000

test_frsp_third:
  #_ REGISTER_IN f1 0x3FD5555555555555
  frsp f2, f1
  blr
  #_ REGISTER_OUT f2 0x3FD5555560000000

test_fdiv_simple:
  #_ REGISTER_IN f1 10.0
  #_ REGISTER_IN f2 4.0
  fdiv f3, f1, f2
  blr
  #_ REGISTER_OUT f3 2.5

test_fcmpu_lt:
  #_ REGISTER_IN f1 -1.0
  #_ REGISTER_IN f2 2.0
  fcmpu cr1, f1, f2
  blr
  #_ REGISTER_OUT cr 0x0000000008000000
