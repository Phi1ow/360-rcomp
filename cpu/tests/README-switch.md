# Heuristic switch discovery

Patch `0011-switch-discovery-edges.patch` adds explicit configured switch
edges to `Function::Analyze`. A reachable `bctr` makes its case blocks
reachable even across padding; a case already discovered through another
branch still establishes that discontinuous blocks are valid. Heuristic
functions stop at the next declared `.pdata`, configuration or ABI helper
entry, found through a sorted immutable snapshot and `upper_bound`.

Mapping a switch base to a `bctr` is limited to 64 bytes inside one basic
block and one code section. Invalid or unaligned base/labels, non-code
addresses, invalid registers, empty labels, absent `bctr` and ambiguous
multiple bases fail analysis with a diagnostic and process exit 1 before
any generated output is written. This validates the discovery mapping; it
does not make the upstream TOML parser a general untrusted-input validator.
The generated control-flow checks still reject targets outside a function.

`switch_original.s` is original source. The runner builds a synthetic XEX,
then removes only the two dispatcher `.pdata` entries, preserving the entry
point and following function boundary. It executes both switches at nine
indices each (including negative signed representations and out-of-range
unsigned indices) and the following function independently. One switch has
four distinct cases; the second has duplicate labels also reached through
the conditional/default paths, separated by 32 padding bytes. The fixture
must generate exactly four functions. Ten malformed configurations must
exit 1 without producing C++.

```sh
python3 cpu/tests/run_switch_regression.py \
  --source build/cpu-xenonrecomp-cfg-src \
  --build build/cpu-xenonrecomp-cfg \
  --out build/cpu-switch-new
```

2026-09-28 host evidence: `build/cpu-switch-test1/report.json` PASS all 30
steps, including 19 execution verdicts. Baseline through patch 0009:
`build/cpu-cfg-negative-baseline/report.json` FAIL as expected with eight
switch-target-outside-function errors; the old generator incorrectly
returns 0, but the test gate returns 1. Neighbor regressions on the same
fresh pins+0001..0011 build:
`build/cpu-cfg-analyse-test/report.json` PASS (bounded discovery and actual
tail calls); `build/cpu-cfg-instructions-test2/report.json` PASS (estimate,
vector selection, memory ordering and unsupported-neighbor checks).
PS5: NOT TESTED by these scripts.

## Patches 0013-0015 (found by the GTA IV PS5 run, 2026-09-29)

* **0013 32-bit table index.** The generated `switch` tested `rN.u64`
  while the guest guard (`cmplwi`) and the index scaling (`rlwinm`) use the
  low word. `addi r24,r11,3` on `r11 = 0xFFFFFFFE` leaves
  `0x1_00000001` in the 64-bit register: the guard passed (index 1) and the
  switch took `default: __builtin_unreachable()`, so the host jump table was
  read out of bounds (GTA IV `sub_823B1880`, SIGSEGV at table+0x3FFFFFFF8).
  The switch now tests `.u32`, and `default` calls
  `PPC_SWITCH_OUT_OF_RANGE(pc, index)` when the prelude defines it: R-comp
  stops with `jump table at 0x... index=N outside the analysed table`.
* **0014 overridable prologue.** `PPC_FUNC_PROLOGUE` is `#ifndef`-guarded so
  the prelude can supply the diagnostic function ring
  (`RCOMP_DIAGNOSTIC_FUNCTION_RING`, `RCOMP_M6_FUNCTION_RING=ON`) without
  editing generated code.
* **0015 guard scope and inline tables (XenonAnalyse).** The hoisted-guard
  search of 0012 (1024 instructions back) now stops at a `blr` and at any
  instruction that writes the index register; GTA IV `0x82237104` had taken a
  `cmplwi r11` from the previous function, 165 instructions away, across
  `li r11,0..3`, and got one label instead of four. When no guard exists and
  the absolute table follows the `bctr`, its length is the first `n` whose
  smallest entry is the address just past `n` entries (entries never point
  into the table, so `n` is unique). On GTA IV exactly that one table
  changed (1 -> 4 labels); 1169 tables are identical to 0012 and three
  functions that were falsely split at case labels merged back.

Host regression for 0013-0015: NOT TESTED (no fixture yet; the GTA IV
inventory diff above is the evidence). PS5: GTA IV reached gameplay with
0001-0015 (exploratory title, see docs/SESSION_GTA_ENTRY_20260929.md).
