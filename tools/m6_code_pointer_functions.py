#!/usr/bin/env python3
"""Find the functions a title reaches only through code pointers stored in data (virtual method tables,
callback tables), which XenonRecomp's discovery (.pdata entries and direct `bl` targets) misses, and
write them as `functions = [{address, size}]` hints for the generator configuration (tools/m6_inventory.py
--function-hints).

Why: on 2026-10-04 GTA IV's free play in the city called, through a vtable, a two-instruction function
(`li r3,51 ; blr` at 0x823A61A8) that had no recompiled body: RCOMP-FATAL indirect_target
no_function_at_address. Such leaf functions have no prologue and no direct caller, so only the data that
points at them reveals them.

usage: m6_code_pointer_functions.py <inventory dir> [--out hints.json] [--verbose]

The inventory directory holds plain.xex (the decoded image, an XEX2 header followed by the loaded image) and
ppc/ppc_func_mapping.cpp of a previous generation (the functions already known). Metadata only is written:
addresses and sizes, no bytes of the game.

Rules (documented heuristics, not a proof):
  candidate   a 4-byte aligned big-endian word in an initialized data section (.rdata, .data, BINK*DATA)
              whose value is a 4-byte aligned address inside a code section and not a known function start;
  function    the word before the candidate is not the middle of a function: it is a function end
              (blr, bctr, unconditional b), a nop or zero padding word, an address constant (a table that
              lives in code), or the candidate is the first word of a code section; a candidate whose
              previous word is an ordinary instruction is a mid-function label (an exception handler
              funclet, a computed branch) and is left out;
  calls       the `bl` targets inside an accepted function that are no known function are listed
              (recursively) and, by default, NOT written: XenonRecomp discovers the direct-call targets
              of a hinted function itself, and a function present twice (hint + discovery) makes the
              title refuse its function table ("invalid or duplicate entries"); --include-call-targets
              writes them when a generation reports "Direct call to X with no recompiled function";
  size        instructions from the candidate to the first unconditional return or branch (blr, bctr,
              b without link) that lies at or beyond every forward branch target seen so far, capped at the
              next known function start or the next accepted candidate; candidates without such an end
              before the cap take the cap.
"""
import argparse
import json
import re
import struct
import sys
from pathlib import Path

NOP = 0x60000000
BLR = 0x4E800020
BCTR = 0x4E800420


def pe_sections(image):
    """[(name, va, vsize, characteristics)] of the loaded image (va relative to the base)."""
    if image[:2] != b"MZ":
        raise ValueError("not a PE image")
    e_lfanew = struct.unpack_from("<I", image, 0x3C)[0]
    if image[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        raise ValueError("PE signature missing")
    count = struct.unpack_from("<H", image, e_lfanew + 6)[0]
    optional = struct.unpack_from("<H", image, e_lfanew + 20)[0]
    table = e_lfanew + 24 + optional
    out = []
    for i in range(count):
        name = image[table + 40 * i:table + 40 * i + 8].rstrip(b"\0").decode(errors="replace")
        vsize, va, rawsz, rawptr, _, _, _, _, chars = struct.unpack_from("<IIIIIIHHI", image, table + 40 * i + 8)
        out.append((name, va, vsize, rawsz, chars))
    return out


def load(inventory):
    xex = (inventory / "plain.xex").read_bytes()
    if xex[:4] != b"XEX2":
        raise ValueError("plain.xex is not an XEX2 file")
    header = struct.unpack_from(">I", xex, 8)[0]
    image = xex[header:]
    base = struct.unpack_from("<I", image, struct.unpack_from("<I", image, 0x3C)[0] + 24 + 28)[0]
    return image, base


def known_functions(inventory):
    text = (inventory / "ppc" / "ppc_func_mapping.cpp").read_text(errors="replace")
    return sorted({int(m, 16) for m in re.findall(r"\bsub_([0-9A-F]{8})\b", text)})


def is_data_word(w, base, size):
    return w == 0 or w == NOP or (base <= w < base + size and w % 4 == 0)


def unconditional_end(w):
    return w == BLR or w == BCTR or ((w >> 26) == 18 and not (w & 1))


def function_boundary(w):
    """The word before a function entry: an end (blr, bctr, unconditional b). A `bl`/`bctrl` (a noreturn
    call) or the toolchain's `or r8,r8,r8` before the candidate were tried on 2026-10-04: they admitted
    49 more entries, all in the C runtime's exception-handling area, mostly without an explicit end, and the
    generation then reported a direct call without function; they are left out until a title needs them."""
    return unconditional_end(w)


def branch_target(pc, w):
    op = w >> 26
    if op == 18:
        li = w & 0x03FFFFFC
        if li & 0x02000000:
            li -= 1 << 26
        return li if w & 2 else pc + li
    if op == 16:
        bd = w & 0xFFFC
        if bd & 0x8000:
            bd -= 1 << 16
        return bd if w & 2 else pc + bd
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inventory", type=Path)
    ap.add_argument("--out", type=Path, help="hints JSON: [{\"address\": \"0x...\", \"size\": N}, ...]")
    ap.add_argument("--max-instructions", type=int, default=4096, help="longest function accepted (instructions)")
    ap.add_argument("--include-call-targets", action="store_true",
                    help="also write the direct-call targets found inside the accepted functions; by default they are "
                         "only reported, since XenonRecomp discovers the `bl` targets of a hinted function itself "
                         "(hinting them too made 19 duplicate entries on 2026-10-04 and the title refused its table)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    image, base = load(args.inventory)
    sections = pe_sections(image)
    code = [(va, vsize) for name, va, vsize, rawsz, chars in sections if chars & 0x20]
    data = [(name, va, min(vsize, rawsz)) for name, va, vsize, rawsz, chars in sections
            if not (chars & 0x20) and name not in (".pdata", ".reloc", ".idata", ".tls", ".XEXID", ".XBLD", ".XBMOVIE")]
    known = known_functions(args.inventory)
    known_set = set(known)
    size = len(image)

    def word(addr):
        off = addr - base
        if off < 0 or off + 4 > size:
            return None
        return struct.unpack_from(">I", image, off)[0]

    def in_code(addr):
        return any(base + va <= addr < base + va + vsize for va, vsize in code) and addr % 4 == 0

    # Candidates: distinct code addresses referenced from data, with their first reference.
    refs = {}
    for name, va, length in data:
        start = base + va
        for off in range(0, length - 3, 4):
            w = struct.unpack_from(">I", image, va + off)[0]
            if in_code(w) and w not in known_set and w not in refs:
                refs[w] = (name, start + off)

    import bisect

    def measure(addr, others):
        """(size, explicit_end) of the function at addr: up to the first unconditional end at or beyond
        every forward branch target, capped at the next known function or the next other candidate."""
        i = bisect.bisect_right(known, addr)
        limit = addr + 4 * args.max_instructions
        if i < len(known):
            limit = min(limit, known[i])
        j = bisect.bisect_right(others, addr)
        if j < len(others):
            limit = min(limit, others[j])
        end = None
        reach = addr
        pc = addr
        while pc < limit:
            w = word(pc)
            if w is None:
                break
            t = branch_target(pc, w)
            if t is not None and addr <= t < limit and t > reach:
                reach = t
            if unconditional_end(w) and pc >= reach:
                end = pc + 4
                break
            pc += 4
        return (end if end is not None else limit) - addr, end is not None

    def direct_calls(addr, fsize):
        """Targets of the `bl` instructions inside [addr, addr + fsize) that lie in code."""
        out = set()
        for pc in range(addr, addr + fsize, 4):
            w = word(pc)
            if w is not None and (w >> 26) == 18 and (w & 1):
                t = branch_target(pc, w)
                if t is not None and in_code(t):
                    out.add(t)
        return out

    accepted, rejected = [], []
    entries = {}  # address -> how it was found
    for addr in sorted(refs):
        prev = word(addr - 4)
        first_of_section = any(addr == base + va for va, vsize in code)
        if not (first_of_section or prev is None or function_boundary(prev) or is_data_word(prev, base, size)):
            rejected.append((addr, "previous word is an instruction", prev))
            continue
        entries[addr] = ("data", refs[addr])
    # The generator does not discover new functions inside a hinted range: every `bl` target of a hinted
    # function that is no known function must be hinted too ("Direct call to X with no recompiled function"
    # is a generation failure), recursively.
    pending = sorted(entries)
    while pending:
        others = sorted(entries)
        new = []
        for addr in pending:
            fsize, _ = measure(addr, others)
            for t in direct_calls(addr, fsize):
                if t not in known_set and t not in entries:
                    entries[t] = ("call", (f"from 0x{addr:08X}", addr))
                    new.append(t)
        pending = sorted(new)
    others = sorted(entries)
    for addr in others:
        fsize, explicit = measure(addr, others)
        if fsize <= 0:
            rejected.append((addr, "no room before the next known function", word(addr - 4)))
            continue
        how, ref = entries[addr]
        accepted.append({"address": f"0x{addr:08X}", "size": fsize, "ends_with_return": explicit,
                         "found_by": how, "referenced_from": ref[0],
                         "reference": f"0x{ref[1]:08X}" if isinstance(ref[1], int) else ref[1]})

    print(f"known functions {len(known)}; code addresses referenced from data and unknown: {len(refs)}; "
          f"accepted {len(accepted)} (with an explicit end {sum(a['ends_with_return'] for a in accepted)}, "
          f"direct-call targets of those {sum(a['found_by'] == 'call' for a in accepted)}); rejected {len(rejected)}")
    if args.verbose:
        for a in accepted:
            print("  +", a["address"], a["size"], "ret" if a["ends_with_return"] else "cap", a["found_by"], a["referenced_from"], a["reference"])
        for addr, why, prev in rejected[:50]:
            print("  -", f"0x{addr:08X}", why, f"prev=0x{prev:08X}" if prev is not None else "")
    if args.out:
        written = [a for a in accepted if args.include_call_targets or a["found_by"] == "data"]
        args.out.write_text(json.dumps([{"address": a["address"], "size": a["size"]} for a in written], indent=1) + "\n")
        print(f"hints written: {len(written)}")
        print("wrote", args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
