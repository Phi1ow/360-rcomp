# R-comp original PPC fixtures (owner: Agent 6)

Original tests written for this project (no third-party code). Format follows
Xenia's `src/xenia/cpu/ppc/testing/README.md` annotations, plus R-comp
extensions understood only by `tests/cpu/gen_harness.py`:

- `#_ REGISTER_OUT xer_ca|xer_ov|xer_so <0|1>`
- `#_ EXPECT_FATAL <kind>` — the test must end in `rcomp_fatal(kind)` (see
  `include/rcomp/diag.h`), not return.

Labels starting with `test_` are tests; `fn_*` are helpers; `.L*` are local.
Registers not listed are zero on entry; memory is zero except MEMORY_IN.

Oracles: expected values are computed from the PowerPC architecture
definition (Power ISA 2.x semantics, big-endian storage), independently of
XenonRecomp. Non-trivial values are derived in `oracle_notes.md`.
