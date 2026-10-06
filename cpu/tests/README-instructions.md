# Estimate, select and cache instruction regressions

Patch `0010-estimate-select-cache.patch` implements `frsqrte[.]`, `vsel128`
and `dcbst`, plus the FPSCR and memory-ordering operations needed to observe
their effects. Fixtures here are original PPC instructions; no game bytes are
included. `instruction-references.json` pins public reference files by SHA-256.

`frsqrte` is an ISA-compliant estimate implemented with binary64 sqrt/divide,
not a claim to reproduce the particular Xenon estimate lookup table bit for
bit. PowerPC Book I v2.02, printed page 126, permits relative error 1/32 and
leaves FR/FI/XX undefined. The test uses an independent 90-digit Decimal oracle
over every normal exponent, subnormal samples and all four rounding modes.
It checks the stronger error bound 2^-50 for 24,768 inputs. Integer
classification handles signed zero/infinity, NaN payloads, signalling NaNs
and negative inputs before host arithmetic; errno is preserved. Guest
FPSCR cause/summary bits, result classification, exception suppression and
CR1 are tested separately. FPSCR remains four bytes; PPCContext offsets and
size remain unchanged. `mffs[.]` reads the guest shadow; `mtfsf[.]` applies
the encoded field mask and changes host rounding without mixing guest flags
with host MXCSR bits. This does not implement all pre-existing arithmetic
instructions' missing guest exception flags.

If a guest enables both a relevant FPSCR exception and MSR floating exception
delivery, `frsqrte` explicitly calls `PPC_TRAP`: guest floating exception
transport is not implemented. A test double proves this dispatch; it does
not claim to test real exception delivery or PS5 execution.

`vsel128` selects bits with the old destination as its mask. High register
indices and source/destination aliases are exercised with 256 masks over
16 bytes, with ordinary `vsel` as a neighboring regression. Stock binutils
does not accept VMX128: the original fixture macro encodes the instruction
using the public pinned Xenia decoder/emitter convention.

`dcbst` performs an addressed guest load (including RA=0 and 32-bit address
wrap semantics), then a full CPU fence. Book II v2.02, printed page 21,
classifies this as a load for protection and requires cache-block writeback.
R-comp has one coherent CPU-backed guest RAM allocation and no independent
dirty CPU cache backing to write back. The GPU backend copies guest RAM
into GPU resources and owns Vulkan allocation flushing; direct noncoherent
GPU aliases of guest RAM are outside this contract. `sync`, `lwsync` and
`eieio` use a conservative full CPU fence. Tests cover unchanged addressed
bytes, store publication and a protected page fault. The publication test
uses a release/acquire handshake, which itself provides ordering: it does
not independently demonstrate the fence's necessity. These tests do not
prove a complete PowerPC memory model or Vulkan cache behavior.

Reproduce in the Cygwin host environment after preparing/building the pinned
generator with all patches:

```sh
python3 cpu/tests/run_instruction_regression.py \
  --source build/cpu-xenonrecomp-instructions-v2-src \
  --build build/cpu-xenonrecomp-instructions-v2 \
  --out build/cpu-instructions-new
```

Each output directory must be fresh. The runner retains commands, statuses,
diagnostics and numerical evidence in `report.json`. Generated C++ is
compiled without modification. An unsupported neighboring `frsqrtes` must
still produce a blocking generator diagnostic.

2026-09-28 host evidence: `build/cpu-cfg-instructions-test2/report.json` PASS,
16 steps including 12 C++ verdicts (with `mtfsf.` mask 0x42 and preservation
of other fields); largest sampled relative error
2.0393428062065357e-16. Baseline through patch 0009:
`build/cpu-instructions-negative-baseline/report.json` FAIL as expected at
the unsupported-instruction gate (runner exit 1). PS5: NOT TESTED.

The full existing CPU corpus was regenerated with the same pins+0001..0011
generator and its matching context header: `build/cpu-global-regression/cpu-host-final.json`
records 1,508 PASS, zero FAIL and one pre-existing BLOCKED test
(`seq_jumptable_constants` requires linking its object relocations), runner
exit 0 and completed=true. The test harness preserves direct fixture entry
at address zero while omitting zero from the production indirect registry.
`fixture-pin-proof.json` independently verifies all 167 public Xenia source
files against commit 95a5c3ee; the raw manifest's unresolved `HEAD` value is
not valid provenance. `generator-pin-proof.json` verifies the 55 tracked
generator files against ddd128bc with CRLF normalization, explaining the
raw Cygwin dirty count. `diagnostics-audit.json` retains all generator logs:
338 missing auxiliary-driver source/disassembly messages concern the unused
upstream test driver; the only other diagnostics are three unresolved calls
in the already BLOCKED relocation fixture. No unimplemented or RC warnings
were ignored. The original refused-registration run remains preserved as
`cpu-host.json` and `failed-registration.exe`.

Primary references: [IBM frsqrte](https://www.ibm.com/docs/en/aix/7.3.0?topic=set-frsqrte-floating-reciprocal-square-root-estimate-instruction),
[IBM dcbst](https://www.ibm.com/docs/en/aix/7.2.0?topic=set-dcbst-data-cache-block-store-instruction),
[PowerPC Book I v2.02](https://powerpc.dev/general/PPC_Vers202_Book1_public.pdf),
[Book II v2.02](https://powerpc.dev/general/PPC_Vers202_Book2_public.pdf),
[pinned Xenia vector emitter](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/cpu/ppc/ppc_emit_altivec.cc).
