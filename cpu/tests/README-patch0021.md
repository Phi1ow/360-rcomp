# Regression: generator patch 0021

`0021-update-forms-vmx-xenon-dialect-dataflow-jump-tables.patch` removes the
generator failures the disc catalog found in Halo 3, Skate 3 and NARUTO STORM 3
(and Fallout: New Vegas), generically:

- Load/store with update not translated: `lhzu lhau lfsu lfdu sthu stfsu
  stfdu lbzux lhzux lhaux lwzux lwaux ldux lfsux lfdux stbux sthux stdux
  stfsux stfdux` (EA computed first, access through `PPC_LOAD_*`/`PPC_STORE_*`,
  then rA = EA; single-precision conversion as `lfs`/`stfs`), `lhbrx`,
  `mfctr`, `bdzf`/`bdzt` (decrement CTR, branch if CTR == 0 and CR bit BI is
  false/true; a target in another function is a conditional tail call),
  `vaddsws vsubuwm vsububm vadduhs vsubuws vcmpgtuw[.] vrlw vrlw128`, and the
  cache-hint ("LRU") vector forms `lvxl stvxl lvlxl lvrxl stvlxl stvrxl` and
  their VMX128 encodings (same semantics as the forms without the hint).
- Record forms of doubleword results (`rldicl. rldicr. rldimi. rotldi. clrldi.
  sld. srd. srad. sradi. cntlzd.`) set CR0 from the 64-bit result (Xenia's
  `UpdateCR` on the i64 value); upstream set nothing.
- The decoder offered PowerPC 403/405 instructions to Xenon code: `vsldoi128`
  words whose low 11 bits equal `maclhwu`/`macchwu` decoded as those. The
  `cell` dialect drops `PPC_OPCODE_403`.
- XenonAnalyse only knew one instruction schedule per jump-table form. A data
  flow pass now follows the registers back from every bctr no pattern claimed
  (scheduled `lis/rlwinm/addi`, nops, computed / byte / 16-bit offset forms).
  Bounds: `cmplwi` with `bgt/ble` or `bge/blt` (lt-bit guards, N cases), also
  on the register the index was copied from (`mr`), also when the guard branches
  over an unconditional branch to the default. A table indexed with a register
  the code reuses before its bctr switches on the CTR target (TOML
  `target = "ctr"`). Arrays of function addresses (all entries outside the code
  between the `.pdata` starts around the bctr) stay indirect calls. A table it
  cannot bound is reported as `WARNING: unresolved jump table` (the inventory
  blocks on it) instead of being dropped silently.
- The nearby guard search of the patterns now uses the index register and the
  hoisted stops: Halo 3's 0x824F2F50 took an unrelated `cmplwi cr6,r29,1024`
  (1,025 labels read past a 5-entry inline table). An unguarded inline table may
  be followed by code that is not its first case (the table ends at the first
  word that cannot be an entry).
- Switch cases MSVC marks unreachable point past the end of the function: at a
  zero word they trap (`PPC_TRAP`, a program exception on the console); at the
  start of the next function they branch there (tail call), as the console does.
- The scan for code no table lists no longer turns data into functions (a guess
  whose straight-line flow reaches an undecodable word is skipped one word at a
  time): Skate 3's 487 "Unable to decode" were the data after the import thunks
  at the end of `.text`. An undecodable word inside a declared function keeps
  its diagnostic and now traps instead of falling through.

Fixtures are original (`patch0021_original.s`, `patch0021_diagnostics.s`);
`cpu/tools/mkxex.py` gained `#_ XEX_FUNCTION_END` to model padding after a
function. The runtime hooks of the execution test are the test doubles of
`patch0021_hooks.h`.

```sh
# Cygwin host, PowerPC binutils on PATH (build/prime-binutils/install/bin)
python3 cpu/tests/run_patch0021_regression.py \
  --source build/catalog-tools/xenonrecomp-v21-src \
  --build build/catalog-tools/xenonrecomp-v21 \
  --baseline-build build/catalog-tools/xenonrecomp-v20 \
  --out build/catalog-tools/patch0021-regression-new [--cxx build/catalog-tools/clang-p20on]
```

Checks: eleven tables found and none for the function-address array; one CTR
target table; no analyser warning and no generator diagnostic; the zero-word
case traps; 373 execution checks of the unmodified generated C++ against
values computed from the ISA definitions; the unbounded table is reported and
stays an indirect call; the undecodable word is reported and traps; v20 misses
the scheduled tables, rejects the other-register guard, reports every new
instruction as unrecognized and turns the data into code.
