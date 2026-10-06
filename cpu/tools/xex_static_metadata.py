#!/usr/bin/env python3
"""Static, metadata-only analysis of a decoded Xbox 360 XEX.

The script intentionally delegates PowerPC decoding to GNU objdump.  It does
not contain an opcode decoder and never emits instruction bytes or disassembly
text.  Output is limited to addresses, import names/ordinals, access widths and
direct-call reachability metadata.

For R-comp's pinned host disassembler see deps/deps.lock and
cpu/tools/build_binutils_vmx128.sh (GNU binutils 2.24 + the pinned Xenia VMX128
patch).  The caller supplies the objdump executable explicitly.
"""

from __future__ import annotations

import argparse
import bisect
from dataclasses import dataclass
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
from typing import Iterable


ROOT = Path(__file__).resolve().parents[2]


def _be16(data: bytes, offset: int) -> int:
    return struct.unpack_from(">H", data, offset)[0]


def _be32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def _u32(value: int) -> int:
    return value & 0xFFFFFFFF


def _parse_int(text: str) -> int | None:
    text = text.strip()
    try:
        return int(text, 0)
    except ValueError:
        try:
            return int(text, 16)
        except ValueError:
            return None


def _reg(text: str) -> int | None:
    match = re.fullmatch(r"%?r(\d+)", text.strip())
    return int(match.group(1)) if match else None


@dataclass(frozen=True)
class Section:
    name: str
    va: int
    size: int
    code: bool


@dataclass(frozen=True)
class ImportRecord:
    module: str
    record_type: int
    kind: str
    ordinal: int
    name: str
    address: int


@dataclass(frozen=True)
class Instruction:
    address: int
    mnemonic: str
    operands: str


@dataclass(frozen=True)
class CallEdge:
    caller: int | None
    callsite: int
    target: int


class FunctionIndex:
    def __init__(self, spans: dict[int, int]):
        self._spans = {start: end for start, end in spans.items() if end > start}
        self._starts = sorted(self._spans)

    def owner(self, address: int) -> int | None:
        index = bisect.bisect_right(self._starts, address) - 1
        if index < 0:
            return None
        start = self._starts[index]
        return start if address < self._spans[start] else None

    def starts(self) -> set[int]:
        return set(self._starts)


def parse_export_table(path: Path) -> dict[int, tuple[str, str]]:
    text = path.read_text(encoding="utf-8", errors="strict")
    result: dict[int, tuple[str, str]] = {}
    pattern = re.compile(
        r"XE_EXPORT\(\s*\w+\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\)"
    )
    for match in pattern.finditer(text):
        kind = "variable" if match.group(3) == "kVariable" else "function"
        result[int(match.group(1), 16)] = (match.group(2), kind)
    if not result:
        raise ValueError(f"no exports parsed from {path}")
    return result


def parse_mapping_starts(path: Path | None) -> set[int]:
    if path is None:
        return set()
    starts: set[int] = set()
    pattern = re.compile(r"\{\s*0x([0-9A-Fa-f]+)\s*,")
    with path.open(encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = pattern.search(line)
            if match:
                starts.add(int(match.group(1), 16))
    if not starts:
        raise ValueError(f"no function addresses parsed from {path}")
    return starts


def parse_plain_xex(
    path: Path,
    export_tables: dict[str, dict[int, tuple[str, str]]],
) -> tuple[dict, bytes, list[Section], list[ImportRecord], dict[int, int]]:
    data = path.read_bytes()
    if len(data) < 24 or data[:4] != b"XEX2":
        raise ValueError("input is not a decoded XEX2 file")
    header_size = _be32(data, 8)
    security_offset = _be32(data, 16)
    count = _be32(data, 20)
    if header_size > len(data) or 24 + count * 8 > header_size:
        raise ValueError("truncated XEX optional-header table")
    options = {_be32(data, 24 + i * 8): _be32(data, 28 + i * 8) for i in range(count)}
    image_size = _be32(data, security_offset + 4)
    base = options.get(0x00010201, _be32(data, security_offset + 0x114))
    entry = options.get(0x00010100, 0)
    if image_size == 0 or header_size + image_size > len(data):
        raise ValueError("truncated decoded image")
    image = data[header_size : header_size + image_size]

    if len(image) < 0x40:
        raise ValueError("truncated PE image")
    e_lfanew = struct.unpack_from("<I", image, 0x3C)[0]
    if e_lfanew + 24 > len(image):
        raise ValueError("truncated PE header")
    section_count = struct.unpack_from("<H", image, e_lfanew + 6)[0]
    optional_size = struct.unpack_from("<H", image, e_lfanew + 20)[0]
    sections: list[Section] = []
    for index in range(section_count):
        offset = e_lfanew + 24 + optional_size + index * 40
        if offset + 40 > len(image):
            raise ValueError("truncated PE section table")
        name = image[offset : offset + 8].rstrip(b"\0").decode("latin-1")
        virtual_size, rva = struct.unpack_from("<II", image, offset + 8)
        flags = struct.unpack_from("<I", image, offset + 36)[0]
        if rva >= len(image) and virtual_size:
            raise ValueError(f"section {name} starts outside decoded image")
        sections.append(Section(name, base + rva, virtual_size, bool(flags & 0x20)))

    records: list[ImportRecord] = []
    import_offset = options.get(0x000103FF)
    if import_offset is not None:
        if import_offset + 12 > header_size:
            raise ValueError("truncated import header")
        string_size = _be32(data, import_offset + 4)
        library_count = _be32(data, import_offset + 8)
        names: list[str] = []
        string_offset = 0
        while len(names) < library_count:
            start = import_offset + 12 + string_offset
            limit = import_offset + 12 + string_size
            if start >= limit:
                raise ValueError("truncated import string table")
            end = data.find(b"\0", start, limit)
            if end < 0:
                raise ValueError("unterminated import library name")
            names.append(data[start:end].decode("latin-1"))
            string_offset += ((end - start) + 1 + 3) & ~3
        library_offset = import_offset + 12 + string_size
        for _ in range(library_count):
            if library_offset + 40 > header_size:
                raise ValueError("truncated import library")
            library_size = _be32(data, library_offset)
            name_index = _be16(data, library_offset + 0x24)
            import_count = _be16(data, library_offset + 0x26)
            if name_index >= len(names) or library_size < 40 + import_count * 4:
                raise ValueError("invalid import library")
            module = names[name_index].lower()
            table = export_tables.get(module, {})
            for index in range(import_count):
                address = _be32(data, library_offset + 40 + index * 4)
                if address < base or address + 4 > base + len(image):
                    raise ValueError(f"import record 0x{address:08X} outside image")
                word = _be32(image, address - base)
                record_type = word >> 24
                ordinal = word & 0xFFFF
                export = table.get(ordinal)
                name = export[0] if export else f"ordinal_0x{ordinal:04X}"
                kind = export[1] if export else "unknown"
                records.append(ImportRecord(module, record_type, kind, ordinal, name, address))
            library_offset += library_size

    pdata = next((section for section in sections if section.name == ".pdata"), None)
    pdata_functions: dict[int, int] = {}
    if pdata is not None:
        offset = pdata.va - base
        for cursor in range(0, pdata.size - 7, 8):
            begin, packed = struct.unpack_from(">II", image, offset + cursor)
            length = ((packed >> 8) & 0x3FFFFF) * 4
            if length and base <= begin < base + len(image):
                pdata_functions.setdefault(begin, length)

    metadata = {
        "base": base,
        "image_size": image_size,
        "entry": entry,
        "header_size": header_size,
    }
    return metadata, image, sections, records, pdata_functions


def build_function_index(
    sections: list[Section], pdata_functions: dict[int, int], mapping_starts: set[int]
) -> FunctionIndex:
    code_sections = [section for section in sections if section.code]

    def containing_section(address: int) -> Section | None:
        return next(
            (section for section in code_sections if section.va <= address < section.va + section.size),
            None,
        )

    all_starts = {
        start for start in set(pdata_functions) | mapping_starts if containing_section(start) is not None
    }
    ordered = sorted(all_starts)
    spans: dict[int, int] = {}
    for index, start in enumerate(ordered):
        section = containing_section(start)
        assert section is not None
        if start in pdata_functions:
            end = min(start + pdata_functions[start], section.va + section.size)
        else:
            candidates = [section.va + section.size]
            if index + 1 < len(ordered) and ordered[index + 1] < section.va + section.size:
                candidates.append(ordered[index + 1])
            end = min(candidates)
        if end > start:
            spans[start] = end
    return FunctionIndex(spans)


_DISASM_LINE = re.compile(
    r"^\s*([0-9A-Fa-f]+):\s+([A-Za-z0-9_.+-]+)(?:\s+(.*))?$"
)
_DIRECT_TARGET = re.compile(r"^(?:0x)?([0-9A-Fa-f]{1,8})(?:\s|$)")
_MEMORY = re.compile(r"^([^()]+)\((%?r\d+)\)$")


def parse_objdump_line(line: str) -> Instruction | None:
    match = _DISASM_LINE.match(line)
    if not match:
        return None
    return Instruction(int(match.group(1), 16), match.group(2).lower(), (match.group(3) or "").strip())


def direct_target(instruction: Instruction) -> int | None:
    if instruction.mnemonic not in ("bl", "bla"):
        return None
    match = _DIRECT_TARGET.match(instruction.operands)
    return int(match.group(1), 16) if match else None


class StaticUseScanner:
    LOAD_WIDTHS = {
        "lbz": 1,
        "lbzu": 1,
        "lhz": 2,
        "lhzu": 2,
        "lha": 2,
        "lhau": 2,
        "lwz": 4,
        "lwzu": 4,
        "lfs": 4,
        "lfsu": 4,
        "lfd": 8,
        "lfdu": 8,
    }

    def __init__(
        self,
        functions: FunctionIndex,
        slots: dict[int, ImportRecord],
        function_imports: dict[int, ImportRecord],
    ) -> None:
        self.functions = functions
        self.slots = slots
        self.function_imports = function_imports
        self.current_function: int | None = None
        self.registers: dict[int, int | tuple[str, str, int]] = {}
        self.reads: list[dict] = []
        self.call_uses: list[dict] = []
        self.calls: list[CallEdge] = []

    def _set(self, register: int | None, value: int | tuple[str, str, int] | None) -> None:
        if register is None:
            return
        if value is None:
            self.registers.pop(register, None)
        else:
            self.registers[register] = value

    def _reset_for(self, function: int | None) -> None:
        if function != self.current_function:
            self.current_function = function
            self.registers.clear()

    def feed(self, instruction: Instruction) -> None:
        function = self.functions.owner(instruction.address)
        self._reset_for(function)
        operation = instruction.mnemonic
        parts = [part.strip() for part in instruction.operands.split(",")] if instruction.operands else []

        target = direct_target(instruction)
        if target is not None:
            self.calls.append(CallEdge(function, instruction.address, target))
            imported = self.function_imports.get(target)
            if imported is not None:
                for register in range(3, 11):
                    value = self.registers.get(register)
                    if isinstance(value, tuple) and value[0] == "var":
                        self.call_uses.append(
                            {
                                "variable": value[1],
                                "function": function,
                                "callsite": instruction.address,
                                "argument_register": register,
                                "variable_offset": value[2],
                                "callee_module": imported.module,
                                "callee_name": imported.name,
                                "callee_ordinal": imported.ordinal,
                            }
                        )
            for register in (0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12):
                self.registers.pop(register, None)
            return

        if operation == "lis" and len(parts) >= 2:
            dest, value = _reg(parts[0]), _parse_int(parts[1])
            self._set(dest, ((value & 0xFFFF) << 16) if value is not None else None)
            return
        if operation == "li" and len(parts) >= 2:
            dest, value = _reg(parts[0]), _parse_int(parts[1])
            self._set(dest, _u32(value) if value is not None else None)
            return
        if operation == "mr" and len(parts) >= 2:
            dest, source = _reg(parts[0]), _reg(parts[1])
            self._set(dest, self.registers.get(source))
            return
        if operation in ("addi", "addis") and len(parts) >= 3:
            dest, source, immediate = _reg(parts[0]), _reg(parts[1]), _parse_int(parts[2])
            base = self.registers.get(source)
            scale = 0x10000 if operation == "addis" else 1
            if isinstance(base, int) and immediate is not None:
                self._set(dest, _u32(base + immediate * scale))
            elif isinstance(base, tuple) and base[0] == "var" and immediate is not None:
                self._set(dest, ("var", base[1], base[2] + immediate * scale))
            else:
                self._set(dest, None)
            return
        if operation in ("ori", "oris") and len(parts) >= 3:
            dest, source, immediate = _reg(parts[0]), _reg(parts[1]), _parse_int(parts[2])
            base = self.registers.get(source)
            if isinstance(base, int) and immediate is not None:
                immediate &= 0xFFFF
                if operation == "oris":
                    immediate <<= 16
                self._set(dest, _u32(base | immediate))
            else:
                self._set(dest, None)
            return

        width = self.LOAD_WIDTHS.get(operation)
        if width is not None and len(parts) >= 2:
            dest = _reg(parts[0])
            memory = _MEMORY.match(parts[1])
            loaded: int | tuple[str, str, int] | None = None
            if memory:
                displacement, base_register = _parse_int(memory.group(1)), _reg(memory.group(2))
                base_value = self.registers.get(base_register)
                if displacement is not None and isinstance(base_value, int):
                    effective = _u32(base_value + displacement)
                    slot = self.slots.get(effective)
                    if slot is not None:
                        self.reads.append(
                            {
                                "variable": slot.name,
                                "function": function,
                                "instruction": instruction.address,
                                "kind": "slot",
                                "offset": 0,
                                "width": width,
                                "mnemonic": operation,
                            }
                        )
                        if width == 4:
                            loaded = ("var", slot.name, 0)
                elif displacement is not None and isinstance(base_value, tuple) and base_value[0] == "var":
                    offset = base_value[2] + displacement
                    self.reads.append(
                        {
                            "variable": base_value[1],
                            "function": function,
                            "instruction": instruction.address,
                            "kind": "field",
                            "offset": offset,
                            "width": width,
                            "mnemonic": operation,
                        }
                    )
            self._set(dest, loaded)
            return

        if parts:
            dest = _reg(parts[0])
            if dest is not None and operation not in (
                "cmpw",
                "cmpwi",
                "cmplw",
                "cmplwi",
                "stw",
                "stb",
                "sth",
                "std",
            ):
                self.registers.pop(dest, None)
        if operation in ("b", "ba", "blr", "bctr") or (
            operation.startswith("b") and operation not in ("bl", "bla")
        ):
            self.registers.clear()


def scan_disassembly(
    objdump: Path,
    image: bytes,
    base: int,
    sections: list[Section],
    scanner: StaticUseScanner,
    scratch: Path,
) -> str:
    scratch.mkdir(parents=True, exist_ok=True)
    version = subprocess.run(
        [str(objdump), "--version"], capture_output=True, text=True, check=True, timeout=10
    ).stdout.splitlines()[0]
    for section in (section for section in sections if section.code):
        image_offset = section.va - base
        available = max(0, min(section.size, len(image) - image_offset))
        payload = image[image_offset : image_offset + available]
        with tempfile.NamedTemporaryFile(
            prefix="rcomp-ppc-", suffix=".bin", dir=scratch, delete=False
        ) as temporary:
            temporary.write(payload)
            temporary_path = Path(temporary.name)
        try:
            process = subprocess.Popen(
                [
                    str(objdump),
                    "-D",
                    "-b",
                    "binary",
                    "-m",
                    "powerpc:common",
                    "-EB",
                    "--no-show-raw-insn",
                    f"--adjust-vma=0x{section.va:X}",
                    str(temporary_path),
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            assert process.stdout is not None
            for line in process.stdout:
                instruction = parse_objdump_line(line)
                if instruction is not None:
                    scanner.feed(instruction)
            stderr = process.stderr.read() if process.stderr is not None else ""
            return_code = process.wait()
            if return_code:
                raise RuntimeError(f"objdump failed for {section.name} ({return_code}): {stderr.strip()}")
        finally:
            temporary_path.unlink(missing_ok=True)
    return version


def compute_reachability(
    entry: int, calls: Iterable[CallEdge], function_starts: set[int]
) -> tuple[dict[int, int], dict[int, int | None], dict[int, list[CallEdge]]]:
    adjacency: dict[int, list[CallEdge]] = {}
    for edge in calls:
        if edge.caller is not None:
            adjacency.setdefault(edge.caller, []).append(edge)
    for edges in adjacency.values():
        edges.sort(key=lambda edge: edge.callsite)
    depth = {entry: 0}
    parent: dict[int, int | None] = {entry: None}
    queue = [entry]
    while queue:
        caller = queue.pop(0)
        for edge in adjacency.get(caller, []):
            if edge.target not in function_starts or edge.target in depth:
                continue
            depth[edge.target] = depth[caller] + 1
            parent[edge.target] = caller
            queue.append(edge.target)
    return depth, parent, adjacency


def _hex(value: int | None) -> str | None:
    return None if value is None else f"0x{value:08X}"


def _normalize_read(read: dict) -> dict:
    return {
        **read,
        "function": _hex(read["function"]),
        "instruction": _hex(read["instruction"]),
    }


def _normalize_call_use(use: dict) -> dict:
    return {
        **use,
        "function": _hex(use["function"]),
        "callsite": _hex(use["callsite"]),
        "callee_ordinal": f"0x{use['callee_ordinal']:04X}",
    }


def build_report(
    metadata: dict,
    records: list[ImportRecord],
    scanner: StaticUseScanner,
    functions: FunctionIndex,
    focus_variables: set[str],
    objdump_version: str,
    max_depth: int,
) -> dict:
    function_imports = {
        record.address: record
        for record in records
        if record.record_type == 1 and record.kind == "function" and record.address
    }
    variable_slots = {
        record.name: record
        for record in records
        if record.kind == "variable" and record.name in focus_variables
    }
    depth, parent, adjacency = compute_reachability(metadata["entry"], scanner.calls, functions.starts())

    def path_to(function: int) -> list[str]:
        path: list[int] = []
        current: int | None = function
        while current is not None:
            path.append(current)
            current = parent.get(current)
        path.reverse()
        return [_hex(address) for address in path]

    variables = {}
    for name in sorted(focus_variables):
        slot = variable_slots.get(name)
        reads = [read for read in scanner.reads if read["variable"] == name]
        uses = [use for use in scanner.call_uses if use["variable"] == name]
        field_reads = [read for read in reads if read["kind"] == "field" and read["offset"] >= 0]
        extent = max((read["offset"] + read["width"] for read in field_reads), default=0)
        variables[name] = {
            "module": slot.module if slot else None,
            "ordinal": f"0x{slot.ordinal:04X}" if slot else None,
            "slot": _hex(slot.address) if slot else None,
            "slot_reads": [_normalize_read(read) for read in reads if read["kind"] == "slot"],
            "field_reads": [_normalize_read(read) for read in field_reads],
            "call_uses": [_normalize_call_use(use) for use in uses],
            "minimum_readable_bytes_from_exported_address": extent,
        }

    entry_direct_imports = []
    reachable_imports = []
    for caller, edges in adjacency.items():
        caller_depth = depth.get(caller)
        for edge in edges:
            imported = function_imports.get(edge.target)
            if imported is None:
                continue
            item = {
                "caller": _hex(caller),
                "callsite": _hex(edge.callsite),
                "target": _hex(edge.target),
                "module": imported.module,
                "name": imported.name,
                "ordinal": f"0x{imported.ordinal:04X}",
            }
            if caller == metadata["entry"]:
                entry_direct_imports.append(item)
            if caller_depth is not None and caller_depth <= max_depth:
                reachable_imports.append(
                    {**item, "caller_depth": caller_depth, "caller_path": path_to(caller)}
                )
    reachable_imports.sort(key=lambda item: (item["caller_depth"], int(item["callsite"], 16)))
    entry_direct_imports.sort(key=lambda item: int(item["callsite"], 16))

    focus_functions = sorted(
        {
            read["function"]
            for read in scanner.reads
            if read["variable"] in focus_variables and read["function"] is not None
        }
    )
    focus_reachability = {
        _hex(function): {
            "direct_call_depth_from_entry": depth.get(function),
            "statically_reachable": function in depth,
        }
        for function in focus_functions
    }

    entry_direct_calls = []
    for edge in adjacency.get(metadata["entry"], []):
        imported = function_imports.get(edge.target)
        if imported is not None:
            entry_direct_calls.append(
                {
                    "callsite": _hex(edge.callsite),
                    "target": _hex(edge.target),
                    "kind": "import",
                    "module": imported.module,
                    "name": imported.name,
                    "ordinal": f"0x{imported.ordinal:04X}",
                }
            )
        else:
            entry_direct_calls.append(
                {
                    "callsite": _hex(edge.callsite),
                    "target": _hex(edge.target),
                    "kind": "internal" if edge.target in functions.starts() else "unknown",
                }
            )

    return {
        "analysis": {
            "kind": "static metadata only",
            "execution_observed": False,
            "direct_call_reachability_only": True,
            "decoder": objdump_version,
            "decoder_contract": "GNU objdump disassembly; this tool does not decode PPC opcodes",
        },
        "image": {
            "base": _hex(metadata["base"]),
            "size": metadata["image_size"],
            "entry": _hex(metadata["entry"]),
        },
        "variables": variables,
        "focus_reader_reachability": focus_reachability,
        "entry_direct_calls": entry_direct_calls,
        "entry_direct_imports": entry_direct_imports,
        "reachable_imports": reachable_imports,
        "reachable_function_count": len(depth),
    }


def _scratch_allowed(path: Path) -> bool:
    resolved = path.resolve()
    try:
        resolved.relative_to((ROOT / "build").resolve())
        return True
    except ValueError:
        return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("xex", type=Path, help="decoded plain XEX2 (derived content; keep local)")
    parser.add_argument("--objdump", type=Path, required=True)
    parser.add_argument("--func-mapping", type=Path)
    parser.add_argument("--scratch", type=Path, required=True, help="temporary directory under R-comp build/")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--max-depth", type=int, default=3)
    parser.add_argument("--focus-variable", action="append", default=[])
    args = parser.parse_args()
    if not _scratch_allowed(args.scratch):
        parser.error("--scratch must be under this checkout's build/ directory")
    if args.max_depth < 0:
        parser.error("--max-depth must be non-negative")

    xboxkrnl = parse_export_table(
        ROOT / "third_party/XenonRecomp/XenonUtils/xbox/xboxkrnl_table.inc"
    )
    xam = parse_export_table(ROOT / "third_party/XenonRecomp/XenonUtils/xbox/xam_table.inc")
    metadata, image, sections, records, pdata_functions = parse_plain_xex(
        args.xex, {"xboxkrnl.exe": xboxkrnl, "xam.xex": xam}
    )
    mapping_starts = parse_mapping_starts(args.func_mapping)
    functions = build_function_index(sections, pdata_functions, mapping_starts)
    focus = set(args.focus_variable)
    if not focus:
        focus = {record.name for record in records if record.kind == "variable"}
    slots = {
        record.address: record
        for record in records
        if record.record_type == 0 and record.kind == "variable" and record.name in focus
    }
    imports = {
        record.address: record
        for record in records
        if record.record_type == 1 and record.kind == "function"
    }
    scanner = StaticUseScanner(functions, slots, imports)
    objdump_version = scan_disassembly(
        args.objdump.resolve(), image, metadata["base"], sections, scanner, args.scratch
    )
    report = build_report(
        metadata, records, scanner, functions, focus, objdump_version, args.max_depth
    )
    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
