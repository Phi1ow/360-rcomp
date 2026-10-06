# Derivation of non-trivial expected values

- `test_fmadd_is_fused`: a = 1 + 2^-30 (0x3FF0000000400000), b = 1 - 2^-30
  (0x3FEFFFFFFF800000), c = -1. Exact a*b = 1 - 2^-60. PowerPC `fmadd` rounds
  once: result -2^-60 = 0xBC30000000000000. A non-fused a*b rounds to 1.0 and
  gives +0.0, so this test distinguishes fused from unfused translation.
  (Python `fractions.Fraction` exact arithmetic, see commit message.)
- `test_frsp_third`: 1/3 = 0x3FD5555555555555, round-to-nearest single =
  0x3EAAAAAB, widened back to double = 0x3FD5555560000000.
- `test_cmpw_cr6`: CR field 6 occupies CR bits 24..27 (big-endian bit
  numbering), LT is bit 24 -> value 1 << (31-24) = 0x80.
- `test_vaddfp`: [1,2,3,4] + [0.5,1.5,2.5,3.5] = [1.5,3.5,5.5,7.5] =
  [3FC00000, 40600000, 40B00000, 40F00000] (IEEE single, exact).
- `test_be_load_store`: memory 11 22 33 44 85 66 77 88; lwz -> 0x11223344,
  lhz(+4) -> 0x8566, lha(+4) -> sign-extended 0xFFFFFFFFFFFF8566,
  lbz(+7) -> 0x88, ld(+0) -> 0x1122334485667788, lwbrx -> 0x44332211.
- `test_addic_carry_chain`: addic 1 + (-1) = 0 with CA=1; adde 0 + 0 + CA = 1
  and produces no carry, so CA is 0 afterwards. `test_addic_sets_ca` checks
  the CA written by addic alone.
- `test_srawi_carry`: low word 0xFFFFFFF9 = -7; -7 >> 1 = -4 (rounds toward
  -inf), CA = 1 because the source is negative and a 1 bit was shifted out.
- Traps (Power ISA `tw`/`twi`/`td`/`tdi`, TO bits 16 lt, 8 gt, 4 eq, 2 ltu,
  1 gtu). `twi 1, r3, -32` with r3 low word 0xFFFFFFF0: SI sign-extends to
  0xFFFFFFE0, 0xFFFFFFF0 >u 0xFFFFFFE0 ⇒ taken. `twi 8, r3, -8`: -16 > -8 is
  false ⇒ not taken. r3 = 0x1_0000_0000, r4 = 2: `td 16` compares
  doublewords (2^32 < 2 false ⇒ not taken) while `tw 16` compares low words
  (0 < 2 ⇒ taken), which separates the word and doubleword forms.
