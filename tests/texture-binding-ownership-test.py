"""Check a linked x86 cache-hit path for redundant ownership work.

This toolchain-specific cost guard supplements real D3D9/Vulkan lifecycle and
pixel tests. It does not establish resource correctness or a timing speedup.
It checks acyclic cache-hit paths; actual performance is measured separately.
The batch default covers the first dirty-slot iteration, not later loop visits.
"""
import argparse
import hashlib
import json
import re
import struct
import subprocess
from pathlib import Path

PREPARE = "__ZN4dxvk12D3D9DeviceEx26PrepareGtaSaTextureBindingEmRKNS_2RcINS_13DxvkImageViewEEE"
BIND = "__ZN4dxvk12D3D9DeviceEx11BindTextureEm"
BATCH = "__ZN4dxvk12D3D9DeviceEx16BindTextureBatchEj"


def disassemble(tool, binary, symbol):
    dump = subprocess.check_output([
        str(tool), "-d", "--no-show-raw-insn", "--x86-asm-syntax=intel",
        "--disassemble-symbols=" + symbol, str(binary)], text=True)
    code = []
    for line in dump.splitlines():
        match = re.match(r"\s*([0-9a-f]+):\s+(\w+)\s*(.*)", line)
        if match:
            code.append((int(match[1], 16), match[2], match[3]))
    if not code:
        raise ValueError("Missing linked symbol: " + symbol)
    return code


def counter_offset(code):
    matches = []
    for i, (_, op, args) in enumerate(code[:-1]):
        match = re.fullmatch(r"dword ptr \[(e\w+) \+ (0x[0-9a-f]+)\], 0x1", args)
        if op != "add" or not match:
            continue
        base, offset = match[1], int(match[2], 16)
        if code[i + 1][1:] == ("adc", f"dword ptr [{base} + {hex(offset + 4)}], 0x0"):
            matches.append(offset)
    if len(matches) != 1:
        raise ValueError("Cannot uniquely identify the real texture-cache hit counter")
    return matches[0]


def reachable_costs(code, start, stop):
    by_address = {row[0]: i for i, row in enumerate(code)}
    pending = [(start, False, False)]
    visited = set()
    outcomes = set()
    while pending:
        address, atomic, call = pending.pop()
        state = address, atomic, call
        if state in visited or address not in by_address:
            continue
        visited.add(state)
        if address == stop:
            outcomes.add((atomic, call))
            continue
        i = by_address[address]
        _, op, args = code[i]
        atomic |= op == "lock" or op.startswith("cmpxchg") or op == "xchg"
        call |= op == "call"
        if op in ("ret", "ud2", "int3"):
            continue
        successors = []
        if op.startswith("j"):
            target = re.match(r"0x([0-9a-f]+)", args)
            if target:
                successors.append(int(target[1], 16))
            elif op == "jmp":
                raise ValueError("Unreviewed indirect jump before cache-hit anchor")
        if op != "jmp" and i + 1 < len(code):
            successors.append(code[i + 1][0])
        # One straight-line iteration is sufficient to expose an unconditional
        # retain before the decision. Do not model retry/outer-loop iterations.
        pending.extend((target, atomic, call) for target in successors if target > address)
    return outcomes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("backend", type=Path)
    parser.add_argument("--objdump", required=True, type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--method", choices=("single", "batch"), default="batch")
    args = parser.parse_args()
    binary = args.backend.read_bytes()
    pe = struct.unpack_from("<I", binary, 0x3c)[0]
    if binary[:2] != b"MZ" or binary[pe:pe + 4] != b"PE\0\0" or struct.unpack_from("<H", binary, pe + 4)[0] != 0x14c:
        raise ValueError("Expected the linked x86 backend")
    offset = counter_offset(disassemble(args.objdump, args.backend, PREPARE))
    code = disassemble(args.objdump, args.backend, BIND if args.method == "single" else BATCH)
    anchors = [address for address, op, operands in code if op == "add" and
        re.fullmatch(r"dword ptr \[e\w+ \+ " + hex(offset) + r"\], 0x1", operands)]
    if len(anchors) != 1:
        raise ValueError("Expected one inline cache-hit counter; review changed code generation")
    costs = reachable_costs(code, code[0][0], anchors[0])
    if not costs:
        raise ValueError("Cache-hit anchor is unreachable in inspected code")
    report = dict(backend=str(args.backend.resolve()), method=args.method, sha256=hashlib.sha256(binary).hexdigest(),
        cacheHitCounterOffset=hex(offset), cacheHitAddress=hex(anchors[0]),
        pathCosts=[dict(atomic=atomic, call=call) for atomic, call in sorted(costs)])
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n")
    if any(atomic or call for atomic, call in costs):
        raise AssertionError("cache-hit path performs redundant atomic ownership work or an unreviewed call")
    print(f"PASS linked {args.method} texture-cache hit avoids atomic ownership work")


if __name__ == "__main__":
    main()
