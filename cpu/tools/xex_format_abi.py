#!/usr/bin/env python3
"""Report PPC call/stack metadata for selected imported functions.

This is intentionally a metadata-only companion to xex_static_metadata.py.
PowerPC instructions are decoded by the caller-provided GNU objdump.  The JSON
report contains addresses, register numbers, access widths and stack-relative
offsets, never instruction bytes or disassembly text.

The analysis is static and linear within each discovered function.  It is useful
for ABI evidence (for example register-save areas and stack argument slots), but
it is not evidence that a callsite executed and is not a path-sensitive proof.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import re
import subprocess
import tempfile

from xex_static_metadata import (
    FunctionIndex,
    ImportRecord,
    Instruction,
    ROOT,
    _parse_int,
    _reg,
    build_function_index,
    direct_target,
    parse_export_table,
    parse_mapping_starts,
    parse_objdump_line,
    parse_plain_xex,
)


_MEMORY = re.compile(r"^([^()]+)\((%?r\d+)\)$")


@dataclass(frozen=True)
class StackPointer:
    """Absolute synthetic stack coordinate relative to function-entry r1."""

    coordinate: int


@dataclass
class StackWrite:
    pc: int
    coordinate: int
    width: int
    mnemonic: str
    source_register: int | None
    stored_value: StackPointer | None


class CallAbiScanner:
    STORE_WIDTHS = {
        "stb": 1,
        "stbu": 1,
        "sth": 2,
        "sthu": 2,
        "stw": 4,
        "stwu": 4,
        "stfs": 4,
        "stfsu": 4,
        "std": 8,
        "stdu": 8,
        "stfd": 8,
        "stfdu": 8,
    }
    LOAD_WIDTHS = {
        "lbz": 1,
        "lbzu": 1,
        "lhz": 2,
        "lhzu": 2,
        "lha": 2,
        "lhau": 2,
        "lwz": 4,
        "lwzu": 4,
        "ld": 8,
        "ldu": 8,
    }

    def __init__(self, functions: FunctionIndex, focused_imports: dict[int, ImportRecord]) -> None:
        self.functions = functions
        self.focused_imports = focused_imports
        self.current_function: int | None = None
        self.stack_pointer_coordinate = 0
        self.gprs: dict[int, StackPointer] = {1: StackPointer(0)}
        self.stack_memory: dict[tuple[int, int], StackPointer] = {}
        self.stack_writes: list[StackWrite] = []
        self.branch_pcs: list[int] = []
        self.calls: list[dict] = []

    def _reset(self, function: int | None) -> None:
        self.current_function = function
        self.stack_pointer_coordinate = 0
        self.gprs = {1: StackPointer(0)}
        self.stack_memory.clear()
        self.stack_writes.clear()
        self.branch_pcs.clear()

    def _set_gpr(self, register: int | None, value: StackPointer | None) -> None:
        if register is None or register == 1:
            return
        if value is None:
            self.gprs.pop(register, None)
        else:
            self.gprs[register] = value

    def _memory_operand(self, text: str) -> tuple[int, int] | None:
        match = _MEMORY.match(text)
        if not match:
            return None
        displacement = _parse_int(match.group(1))
        base_register = _reg(match.group(2))
        if displacement is None or base_register is None:
            return None
        return displacement, base_register

    def _record_focus_call(self, instruction: Instruction, imported: ImportRecord) -> None:
        sp = self.stack_pointer_coordinate
        args = [
            {"register": register, "stack_offset": value.coordinate - sp}
            for register in range(3, 11)
            if (value := self.gprs.get(register)) is not None
        ]
        writes = []
        for write in self.stack_writes:
            distance = instruction.address - write.pc
            offset = write.coordinate - sp
            if distance <= 0 or distance > 0x100 or not 0x40 <= offset <= 0x220:
                continue
            stored_offset = (
                write.stored_value.coordinate - sp if write.stored_value is not None else None
            )
            writes.append(
                {
                    "instruction": f"0x{write.pc:08X}",
                    "byte_distance_to_call": distance,
                    "stack_offset": offset,
                    "width": write.width,
                    "mnemonic": write.mnemonic,
                    "source_register": write.source_register,
                    "stored_stack_pointer_offset": stored_offset,
                    "branch_between": any(write.pc < pc < instruction.address for pc in self.branch_pcs),
                }
            )
        self.calls.append(
            {
                "module": imported.module,
                "name": imported.name,
                "ordinal": f"0x{imported.ordinal:04X}",
                "function": None
                if self.current_function is None
                else f"0x{self.current_function:08X}",
                "callsite": f"0x{instruction.address:08X}",
                "frame_size_at_call": max(0, -sp),
                "argument_stack_pointers": args,
                "recent_stack_writes": writes,
            }
        )

    def _clobber_volatile_after_call(self) -> None:
        for register in range(0, 13):
            if register != 1:
                self.gprs.pop(register, None)

    def feed(self, instruction: Instruction) -> None:
        function = self.functions.owner(instruction.address)
        if function != self.current_function:
            self._reset(function)

        operation = instruction.mnemonic
        parts = [part.strip() for part in instruction.operands.split(",")] if instruction.operands else []

        target = direct_target(instruction)
        if target is not None:
            imported = self.focused_imports.get(target)
            if imported is not None:
                self._record_focus_call(instruction, imported)
            self._clobber_volatile_after_call()
            return

        if operation == "mr" and len(parts) >= 2:
            self._set_gpr(_reg(parts[0]), self.gprs.get(_reg(parts[1])))
            return

        if operation in ("addi", "addis") and len(parts) >= 3:
            dest = _reg(parts[0])
            source = _reg(parts[1])
            immediate = _parse_int(parts[2])
            base = self.gprs.get(source)
            if base is not None and immediate is not None:
                scale = 0x10000 if operation == "addis" else 1
                self._set_gpr(dest, StackPointer(base.coordinate + immediate * scale))
            else:
                self._set_gpr(dest, None)
            return

        width = self.STORE_WIDTHS.get(operation)
        if width is not None and len(parts) >= 2:
            memory = self._memory_operand(parts[-1])
            source_register = _reg(parts[0])
            if memory is not None:
                displacement, base_register = memory
                base = self.gprs.get(base_register)
                if base is not None:
                    coordinate = base.coordinate + displacement
                    stored_value = self.gprs.get(source_register)
                    self.stack_writes.append(
                        StackWrite(
                            instruction.address,
                            coordinate,
                            width,
                            operation,
                            source_register,
                            stored_value,
                        )
                    )
                    if width in (4, 8) and stored_value is not None:
                        self.stack_memory[(coordinate, width)] = stored_value
                    if operation in ("stwu", "stdu") and source_register == 1 and base_register == 1:
                        self.stack_pointer_coordinate = coordinate
                        self.gprs[1] = StackPointer(coordinate)
            return

        width = self.LOAD_WIDTHS.get(operation)
        if width is not None and len(parts) >= 2:
            dest = _reg(parts[0])
            value = None
            memory = self._memory_operand(parts[-1])
            if memory is not None:
                displacement, base_register = memory
                base = self.gprs.get(base_register)
                if base is not None:
                    value = self.stack_memory.get((base.coordinate + displacement, width))
            self._set_gpr(dest, value)
            return

        if operation.startswith("b"):
            self.branch_pcs.append(instruction.address)
            return

        if parts:
            dest = _reg(parts[0])
            if dest is not None and dest != 1 and operation not in (
                "cmpw",
                "cmpwi",
                "cmplw",
                "cmplwi",
            ):
                self.gprs.pop(dest, None)


def scan(
    objdump: Path,
    image: bytes,
    base: int,
    sections,
    scanner: CallAbiScanner,
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
            prefix="rcomp-format-abi-", suffix=".bin", dir=scratch, delete=False
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
                raise RuntimeError(
                    f"objdump failed for {section.name} ({return_code}): {stderr.strip()}"
                )
        finally:
            temporary_path.unlink(missing_ok=True)
    return version


def _scratch_allowed(path: Path) -> bool:
    try:
        path.resolve().relative_to((ROOT / "build").resolve())
        return True
    except ValueError:
        return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("xex", type=Path)
    parser.add_argument("--objdump", type=Path, required=True)
    parser.add_argument("--func-mapping", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--focus-call", action="append", required=True)
    args = parser.parse_args()
    if not _scratch_allowed(args.scratch):
        parser.error("--scratch must be under this checkout's build/ directory")

    tables = {
        "xboxkrnl.exe": parse_export_table(
            ROOT / "third_party/XenonRecomp/XenonUtils/xbox/xboxkrnl_table.inc"
        ),
        "xam.xex": parse_export_table(ROOT / "third_party/XenonRecomp/XenonUtils/xbox/xam_table.inc"),
    }
    metadata, image, sections, records, pdata = parse_plain_xex(args.xex, tables)
    functions = build_function_index(sections, pdata, parse_mapping_starts(args.func_mapping))
    focus = set(args.focus_call)
    focused_imports = {
        record.address: record
        for record in records
        if record.record_type == 1 and record.kind == "function" and record.name in focus
    }
    missing = focus - {record.name for record in focused_imports.values()}
    if missing:
        parser.error("focused import(s) absent from XEX: " + ", ".join(sorted(missing)))

    scanner = CallAbiScanner(functions, focused_imports)
    version = scan(args.objdump.resolve(), image, metadata["base"], sections, scanner, args.scratch)
    calls = sorted(scanner.calls, key=lambda call: int(call["callsite"], 16))
    report = {
        "analysis": {
            "kind": "static metadata only",
            "execution_observed": False,
            "path_sensitive": False,
            "decoder": version,
            "decoder_contract": "GNU objdump disassembly; this tool does not decode PPC opcodes",
        },
        "image": {"entry": f"0x{metadata['entry']:08X}", "base": f"0x{metadata['base']:08X}"},
        "focus_calls": sorted(focus),
        "call_counts": {name: sum(call["name"] == name for call in calls) for name in sorted(focus)},
        "calls": calls,
    }
    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(encoded, encoding="utf-8")
    print(encoded, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
