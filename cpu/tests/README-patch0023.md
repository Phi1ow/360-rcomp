# Regression: generator patch 0023

`0023-non-trapping-integer-division.patch` emits every integer division
through helpers instead of a raw C++ `/`:

| Guest form | Quotient | XER[OV] (OE forms) |
| --- | --- | --- |
| `divw[o][.]` | `PPC_DIVW(rA.s32, rB.s32)` | `PPC_DIVW_OVERFLOW` |
| `divwu[o][.]` | `PPC_DIVWU(rA.u32, rB.u32)` | `PPC_DIVWU_OVERFLOW` |
| `divd[o][.]` | `PPC_DIVD(rA.s64, rB.s64)` | `PPC_DIVD_OVERFLOW` |
| `divdu[o][.]` | `PPC_DIVDU(rA.u64, rB.u64)` | `PPC_DIVDU_OVERFLOW` |

On Xenon a zero divisor, or the most negative value divided by -1 for the
signed forms, does not trap: the quotient is architecturally undefined.
Titles compute such quotients speculatively and discard them (Halo 3
`sub_824E2AF8`: `divw r11,r29,r11` before `cmpwi cr6,r11,0 / bgt`), and a
raw C++ division of those operands raises SIGFPE on x86-64 (the PS5).
The helpers (`include/rcomp/ppc_prelude.h`; non-trapping defaults with the
same values in the patched `ppc_context.h`) return 0 for a zero divisor and
the most negative value for most negative / -1. Those values are R-comp's
deterministic choice, not a claim about the hardware.

- The OE forms (`divwo divwuo divdo divduo`, with or without the record
  bit; upstream "Unrecognized instruction") set XER[OV] for exactly those
  operand pairs and OR it into XER[SO], both from the source operands
  before rD is written (rD may be rA or rB).
- The record forms keep the CR0 update the generator emitted before
  (`compare<int32_t>` of the low word with XER[SO]). `divd.` and `divdo.`
  still have none and stay reported as "RC bit enabled but no comparison
  was generated" (blocking for the inventory).
- Without OE forms the output differs from v22 only in the quotient lines
  (`a / b` -> `PPC_DIV*(a, b)`): the fixture here, and Halo 3 with its four
  AOT modules (838 lines in 100 of 202 generated files, every other
  generated `.cpp` byte-identical; `ppc_context.h` gains the defaults).

Fixtures are original: `patch0023_division.s` (plain and record forms,
rD == rB, the Halo 3 shape), `patch0023_overflow.s` (OE forms) and
`patch0023_diagnostics.s` (`divd.`, `divdo.`). `test_ppc_division.cpp`
checks the helpers alone against wide arithmetic (edge grid and 200,000
random operand pairs), also with `-fsanitize=undefined
-fsanitize-trap=undefined`; `test_patch0023_execution.cpp` runs the
generated C++ with the prelude and with the `ppc_context.h` defaults alone
(each also with the UB trap) and divides by zero (the process must
survive). With a baseline generator (v22) the same division by zero ends
the process with SIGFPE.

```sh
# Cygwin host, PowerPC binutils on PATH (build/prime-binutils/install/bin)
python3 cpu/tests/run_patch0023_regression.py \
  --source build/catalog-tools/xenonrecomp-v23-src \
  --build build/catalog-tools/xenonrecomp-v23 \
  --baseline-source build/catalog-tools/xenonrecomp-v22-src \
  --baseline-build build/catalog-tools/xenonrecomp-v22 \
  --out build/catalog-tools/patch0023-regression-new [--cxx build/catalog-tools/clang-p20on]
```

Host evidence only; the PS5 run of a title built with v23 is NOT TESTED here.
