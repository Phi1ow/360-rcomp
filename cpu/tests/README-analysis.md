# Regression: function discovery bounds (patch 0009)

`Function::Analyze` previously followed conditional branches preceding the
function base. A backward tail call could underflow the block offset, discover
code outside the supplied range, and grow the worklist indefinitely or crash.
The patch keeps the fallthrough path and leaves the taken tail call to normal
code generation. It also makes the input end exclusive, bounds the special
two-word lookahead, and uses an absolute address when looking up conditional
return fallthrough blocks.

All PPC inputs here are original test examples. No game bytes are included.

## Run

In a host shell with Python 3, Clang, GNU patch, CMake/Ninja and
`powerpc-linux-gnu-{as,objdump}` available:

```sh
python3 cpu/tools/prepare_xenonrecomp.py --out build/cpu-xenonrecomp-branch-src
cmake -S build/cpu-xenonrecomp-branch-src -B build/cpu-xenonrecomp-branch -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/cpu-xenonrecomp-branch --target XenonRecomp XenonAnalyse -j 4
python3 cpu/tests/run_analyse_regression.py \
  --source build/cpu-xenonrecomp-branch-src \
  --build build/cpu-xenonrecomp-branch --out build/cpu-analyse-regression-final
```

The output directory must be fresh. `report.json` records every command, return
code, diagnostic and assertion. A nonzero return code or missing verdict is
FAIL, including a generator diagnostic about unsupported instructions.

## Evidence, 2026-09-28

- PASS (host): fresh pinned source with patches 0001–0009 builds both tools.
- PASS (host): ten discovery cases, including conditional backward/forward
  tail calls, internal loops, conditional returns and reads ending immediately
  before a protected page.
- PASS (host): original assembly without relocations → real XenonRecomp →
  unmodified generated C++ → independent execution assertions. The taken
  backward tail call returns `r3=77`; the fallthrough returns `r3=11`.
- FAIL (expected negative control): the same bounds runner linked against the
  previously patched source without 0009 crashes (`returncode=-11`) on its
  first original conditional-tail fixture.
- NOT TESTED: execution on PS5. These results establish host discovery and
  code-generation behavior only.

The target in the assembly fixture deliberately starts at offset 4. Offset 0
would collide with XenonRecomp's unset special `longjmp` address and would not
exercise ordinary tail-call dispatch. The runner requires both the ordinary
generated call and the explicit execution verdict, as process exit code alone
is insufficient evidence.
