#!/usr/bin/env python3
"""Flag recompiled functions that read state the C2 options of the audit would turn into locals.

usage: scan_register_inputs.py <ppc dir of the generated code, built WITHOUT the *_as_local options> [--out report.json]

XenonRecomp's local-variable options make a register a variable of the function instead of a field of
the PPCContext that a caller shares with its callee:

  group 1  cr_as_local, ctr_as_local, xer_as_local, reserved_as_local   condition registers, CTR, XER and
           the reservation. Right unless a function reads one that its caller (or the code that falls into
           it, for a function the analysis split off another one) set.
  group 2  non_argument_as_local (r0, r2, r11, r12, f0, v32-v63) and non_volatile_as_local (r14-r31, f14-f31,
           v14-v31, v64-v127; it also removes the calls of the __save/__rest helpers). Right for code that
           follows the PowerPC ABI: a function neither expects those registers as input nor hands one back.

A function that does silently misbehaves: its local starts at zero. This scanner reads the generated C++
of the unmodified configuration and lists, per class, the functions in which such a register is read before
its first write in the order of the text. Reads that only spill a register into the function's own stack
frame (the save of a prologue), the mfcr that saves the condition registers, and the __save/__rest helpers
themselves are ignored.

It is a text heuristic, not a data-flow analysis: branches that execute out of text order give false
positives, and an input read through a register the function also writes earlier in the text gives false
negatives. A function it lists is a candidate to inspect; a clean report is evidence, not proof. The result
is host evidence about the image, never a PS5 result.
"""
import argparse
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

FUNCTION = re.compile(r"^PPC_FUNC_IMPL\(__imp__(\w+)\)")
ASSIGN = re.compile(r"^\s*ctx\.([rfv])(\d+)\.\w+(?:\[\d+\])*\s*=[^=]")
SPECIAL_ASSIGN = re.compile(r"^\s*ctx\.(cr[0-7]|ctr|xer|reserved)\b[\w.\[\]]*\s*=[^=]")
CR_METHOD = re.compile(r"^\s*ctx\.(cr[0-7])\.(?:compare|setFromMask)(?:<[^>]*>)?\(")
REGISTER = re.compile(r"ctx\.([rfv])(\d+)\b")
SPECIAL = re.compile(r"ctx\.(cr[0-7]|ctr|xer|reserved)\b")
VECTOR_STORE = re.compile(r"\((?:simde__m128i|__m128i|simde__m128d|__m128d|simde__m128|__m128)\s*\*\s*\)\s*ctx\.v(\d+)\b")
FRAME_SPILL = re.compile(r"PPC_STORE_U(?:32|64)\(ctx\.r1\.u32\s*\+\s*-?\d+,\s*ctx\.([rf])(\d+)\.u(?:32|64)\)")
HELPER_CALL = re.compile(r"^\s*__(?:save|rest)(?:gpr|fpr|vmx)")
LABEL = re.compile(r"^loc_[0-9A-Fa-f]+:")

# register -> class: what each option turns into locals
NON_ARGUMENT = {"r": {0, 2, 11, 12}, "f": {0}, "v": set(range(32, 64))}
NON_VOLATILE = {"r": set(range(14, 32)), "f": set(range(14, 32)), "v": set(range(14, 32)) | set(range(64, 128))}
CLASSES = ("condition", "non_argument", "non_volatile")


def classify(name):
    if name[0] == "c" and name[1] in "rt" or name in ("xer", "reserved"):  # cr0-cr7, ctr
        return "condition"
    kind, number = name[0], int(name[1:])
    if number in NON_ARGUMENT[kind]:
        return "non_argument"
    if number in NON_VOLATILE[kind]:
        return "non_volatile"
    return None


def scan_function(lines):
    """{register name: (first read line number, text)} for registers read before their first write."""
    written = set()
    flagged = {}
    dead = False  # after an unconditional jump or a return: unreachable up to the next label (jump tables decode as code)
    for number, line in enumerate(lines):
        text = line.strip()
        if not text or text.startswith("//") or HELPER_CALL.match(line):
            continue
        if LABEL.match(text):
            dead = False
            continue
        if dead:
            continue
        if text.startswith("goto ") or text == "return;" or "__builtin_unreachable();" in text:
            dead = True
        spill = FRAME_SPILL.search(text)
        spilled = f"{spill.group(1)}{spill.group(2)}" if spill else None
        stores = {f"v{n}" for n in VECTOR_STORE.findall(text)}
        body = text
        target = None
        assign = ASSIGN.match(line)
        special = SPECIAL_ASSIGN.match(line)
        method = CR_METHOD.match(line)
        if assign:
            target = f"{assign.group(1)}{assign.group(2)}"
            body = text[text.index("=", assign.end(2)):]
        elif special:
            target = special.group(1)
            body = text[special.end() - 1:]
        elif method:
            target = method.group(1)
            body = text[method.end():]
        special_names = SPECIAL.findall(body)
        if method:  # compare(a, b, ctx.xer) only copies XER[SO], which a function does not hand over to its caller
            special_names = [n for n in special_names if n != "xer"]
        names = [f"{kind}{digits}" for kind, digits in REGISTER.findall(body)] + special_names
        crs = [n for n in names if n.startswith("cr")]
        mfcr = len(set(crs)) >= 6        # mfcr (the save of the condition registers) reads all eight
        for name in names:
            if name in written or name in stores or name == spilled or (mfcr and name.startswith("cr")):
                continue
            if classify(name) and name not in flagged:
                flagged[name] = (number, text)
        written |= stores
        if target:
            written.add(target)
    return flagged


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("ppc_dir")
    parser.add_argument("--out", help="write the full report as JSON")
    parser.add_argument("--show", type=int, default=15, help="functions to print per class (default 15)")
    args = parser.parse_args()
    files = sorted(Path(args.ppc_dir).glob("ppc_recomp.*.cpp"), key=lambda p: int(p.stem.split(".")[1]))
    if not files:
        sys.exit(f"{args.ppc_dir}: no ppc_recomp.N.cpp files")
    functions = 0
    flagged_by_class = defaultdict(dict)
    registers = Counter()
    for path in files:
        current, body = None, []
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            match = FUNCTION.match(line)
            if match:
                current, body = match.group(1), []
                continue
            if current is None:
                continue
            if line == "}":
                functions += 1
                for register, (at, text) in scan_function(body).items():
                    flagged_by_class[classify(register)].setdefault(current, []).append((register, at, text))
                    registers[register] += 1
                current = None
            else:
                body.append(line)
    print(f"{functions} functions scanned in {len(files)} files")
    for cls in CLASSES:
        found = {n: v for n, v in flagged_by_class[cls].items() if not n.startswith("__")}
        helpers = len(flagged_by_class[cls]) - len(found)
        print(f"{cls}: {len(found)} functions read a register before writing it"
              + (f" (and {helpers} __save/__rest helpers, which take their input in a register by design)" if helpers else ""))
        for name, items in sorted(found.items())[:args.show]:
            at, text = items[0][1], items[0][2]
            print(f"  {name}: {', '.join(i[0] for i in items)}  e.g. line {at}: {text[:110]}")
        by_register = Counter(i[0] for v in found.values() for i in v)
        print("  registers (functions): " + ", ".join(f"{r} {n}" for r, n in by_register.most_common(16)))
    if args.out:
        report = {"functions": functions,
                  "classes": {cls: {name: [(r, at, text) for r, at, text in items] for name, items in found.items()}
                              for cls, found in flagged_by_class.items()}}
        Path(args.out).write_text(json.dumps(report, indent=1), encoding="utf-8")
        print("report:", args.out)


if __name__ == "__main__":
    main()
