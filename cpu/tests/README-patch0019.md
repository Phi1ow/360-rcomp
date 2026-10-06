# Regression: generator patch 0019

`0019-vmx128-vnor-vcfpuxws-bdnzt-tail-entries-hoisted-guards.patch` removes
the generator failures the disc catalog found in the Unreal Engine 3 title
(`docs/COMPATIBILITY.md`): 44 `vcfpuxws128`, 20 `vnor128`, 2 `bdnzt`, 35 direct
calls to addresses without a function and one switch table that was not found.

- `vnor`/`vnor128`: `vD = ~(vA | vB)`. `vcfpuxws128`: the VMX128 form of
  `vctuxs` (scale by 2^UIMM, round toward zero, unsigned saturation, NaN -> 0),
  sharing its translation. Semantics as the pinned Xenia emitter
  (95a5c3e `ppc_emit_altivec.cc`: `InstrEmit_vnor_`, `InstrEmit_vctuxs_`).
- `bdnzt`/`bdnzf`: decrement CTR, branch if CTR != 0 and CR bit BI is true /
  false. Upstream translated `bdnzf` with the eq bit of the field whatever BI
  named; the fixture's `bdnzf` on gt stops one iteration earlier with it.
- Entry points inside a function: a `bl`, or a `b` leaving its own function,
  may target an address inside another function (a shared `blr` or epilogue).
  When the tail from that address to the end of the enclosing function is
  closed (its non-call branches stay inside it or tail-call a known function
  start; its switch tables only name labels inside it), the tail becomes a
  function of its own, a copy of that code. The search repeats until no new
  tail appears (a tail may end in a branch to another tail). A tail that is
  not closed keeps the blocking `Direct call` diagnostic.
- XenonAnalyse searched for a bounds check hoisted far above the table load
  (1,024 instructions, stopped by a `blr` or a write of the index register)
  only for absolute tables; the same search now applies to the computed,
  byte-offset and 16-bit-offset forms, with the index register of each form.

All inputs are original (`patch0019_original.s`, `patch0019_open_tail.s`).
The stock assembler lacks VMX128, so the fixture encodes `vnor128`, `vor128`
and `vcfpuxws128` with macros following the pinned decoder's field layout.

```sh
# Cygwin host, PowerPC binutils on PATH (build/prime-binutils/install/bin)
python3 cpu/tests/run_patch0019_regression.py \
  --source build/catalog-tools/xenonrecomp-v19-src \
  --build build/catalog-tools/xenonrecomp-v19 \
  --baseline-build build/cpu-xenonrecomp-v18 \
  --out build/catalog-tools/patch0019-regression-new
```

## Evidence, 2 October 2026 (host only)

- PASS 21/21 (`build/catalog-tools/patch0019-regression-3/report.json`):
  one switch table found; no generator diagnostic; both tails declared as
  functions; 58 execution checks of the unmodified generated C++ (tails entered
  by `b` and `bl`, the chained tail, `bdnzt`/`bdnzf` at four counts, `vnor`,
  `vnor128` with high registers, `vcfpuxws128` at scales 0, 3 and 31 on eight
  inputs including NaN, −0, negative and overflowing values, the hoisted
  16-bit-offset switch at eight indices); the open tail keeps its diagnostic;
  the generator before the patch reports the four failures and misses the table.
- Neighbour suites on the same build: switch 30/30, analysis 13/13,
  instructions 16/16 PASS.
- Titles 0x545407F2:0x4A53F9F6 and 0x545407F2:0x06759F9C: every generated
  artifact (224 and 234 files) is byte-identical to the generator before the
  patch. Title 0x4D53082D:0x33FCE762: 71,881 functions, no diagnostic.
- PS5 execution of any code generated with this patch: NOT TESTED.
