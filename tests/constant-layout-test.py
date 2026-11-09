"""Check the measured linked x86 constant-control layout.

PS/int comparison loops use a measured 64-byte header alignment; the VS-int
update loop also keeps the validated mod64=0 position. VS float keeps
the measured +16 offset within a 64-byte boundary, avoiding a known regression
in small non-deduplicated writes. A changed fingerprint needs new A/B evidence.
Changing that layout requires remeasuring the constant/draw control workloads.
This is not a timing threshold and does not make a game FPS claim.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import re
import subprocess
import sys


def constant_functions(data):
    if data[:2] != b"MZ":
        raise ValueError("not a PE image")
    pe = struct.unpack_from("<I", data, 60)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("invalid PE signature")
    machine, sections, _, symbols, count, optional_size, _ = struct.unpack_from("<HHIIIHH", data, pe + 4)
    if machine != 0x14C:
        raise ValueError("hot-function test requires the x86 backend")
    section_base = pe + 24 + optional_size
    addresses = [struct.unpack_from("<I", data, section_base + i * 40 + 12)[0] for i in range(sections)]
    strings = symbols + count * 18
    rows = []
    index = 0
    while index < count:
        offset = symbols + index * 18
        name = data[offset:offset + 8]
        if name[:4] == b"\0\0\0\0":
            pointer = strings + struct.unpack_from("<I", name, 4)[0]
            name = data[pointer:data.index(b"\0", pointer)]
        else:
            name = name.split(b"\0", 1)[0]
        value, section = struct.unpack_from("<Ih", data, offset + 8)
        if b"D3D9DeviceEx18SetShaderConstantsI" in name and section > 0:
            rows.append({"name": name.decode("ascii"), "rva": value + addresses[section - 1]})
        index += 1 + data[offset + 17]
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("backend", type=Path)
    parser.add_argument("--objdump", required=True, type=Path)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    data = args.backend.read_bytes()
    rows = [row for row in constant_functions(data) if re.search(r"D3D9ConstantTypeE[01]", row["name"])]
    pe = struct.unpack_from("<I", data, 60)[0]
    image_base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
    dump = subprocess.check_output([str(args.objdump), "-d", "--no-show-raw-insn", "--x86-asm-syntax=intel",
        f"--start-address={image_base + min(row['rva'] for row in rows)}",
        f"--stop-address={image_base + max(row['rva'] for row in rows) + 4096}", str(args.backend)], text=True)
    by_name = {row["name"]: row for row in rows}
    instructions = {row['name']: [] for row in rows}
    current = None
    for line in dump.splitlines():
        header = re.match(r"[0-9a-f]+ <(.+)>:", line)
        if header:
            current = by_name.get(header[1])
        instruction = re.match(r"\s*([0-9a-f]+):\s+(\w+)\s*(.*)", line)
        if current is not None and instruction:
            instructions[current['name']].append((int(instruction[1], 16), instruction[2], instruction[3]))
    for row in rows:
        code = instructions[row['name']]
        loops = set()
        update_loops = set()
        for source, mnemonic, operands in code:
            jump = re.match(r"0x([0-9a-f]+)", operands)
            if not mnemonic.startswith('j') or mnemonic == 'jmp' or not jump:
                continue
            target = int(jump[1], 16)
            body = [(op, arg) for addr, op, arg in code if target <= addr <= source]
            # Identify the actual 16-byte equality loop rather than an arbitrary
            # earlier back edge in SetShaderConstants. clang 22 uses pxor/ptest;
            # older compilers may use pcmpeqd/pmovmskb for the same comparison.
            ops = [op for op, _ in body]
            loads = sum(op in ('movdqu','movups','movdqa','movaps') and 'xmmword ptr [' in arg
                        for op, arg in body)
            compare = ('pxor' in ops and 'ptest' in ops) or ('pcmpeqd' in ops and 'pmovmskb' in ops)
            exits = any(op.startswith('j') and re.match(r'0x([0-9a-f]+)', arg)
                        and int(re.match(r'0x([0-9a-f]+)', arg)[1], 16) > source for op, arg in body)
            # A scalar pointer reload may precede the two vector loads. The
            # update loop instead checks the count and branches before loading.
            head = body[1:] if body and body[0][0] == 'mov' else body
            header_loads = len(head) >= 2 and all(op in ('movdqu','movups','movdqa','movaps')
                and 'xmmword ptr [' in arg for op, arg in head[:2])
            if target < source and len(body) <= 20 and loads == 2 and compare and exits:
                stores = any(arg.startswith('xmmword ptr [') for op, arg in body)
                # An update scan reaches its first vector store either by
                # falling through the back edge (old while form) or through
                # the mismatch exit (rotated for form). The early dedup scan
                # goes to journal capture instead and never stores constants.
                exits_to = [int(match[1], 16) for op, arg in body
                    if op.startswith('j') and (match := re.match(r'0x([0-9a-f]+)', arg))
                    and int(match[1], 16) > source]
                destinations = exits_to + [next((addr for addr, _, _ in code if addr > source), 0)]
                writes_on_exit = any(any(arg.startswith('xmmword ptr [')
                    for _, op, arg in [(a,o,v) for a,o,v in code if a >= dest][:4]) for dest in destinations)
                if not stores and writes_on_exit:
                    update_loops.add(target - image_base)
                elif header_loads:
                    loops.add(target - image_base)
        row['comparisonLoops'] = sorted(loops)
        if len(loops) == 1:
            row['compareLoopRva'] = next(iter(loops))
        vertex_float = 'ShaderType0E' in row['name'] and 'D3D9ConstantTypeE0E' in row['name']
        row['requiredOffsetMod64'] = 16 if vertex_float else 0
        row["layoutMatches"] = len(loops) == 1 and row["compareLoopRva"] % 64 == row['requiredOffsetMod64']
        row['updateLoops'] = sorted(update_loops)
        if 'ShaderType0E' in row['name'] and 'D3D9ConstantTypeE1E' in row['name']:
            row['requiredUpdateOffsetMod64'] = 0
            row['layoutMatches'] = row['layoutMatches'] and len(update_loops) == 1 and next(iter(update_loops)) % 64 == 0
    passed = len(rows) == 4 and all(row["layoutMatches"] for row in rows)
    report = {"backend": str(args.backend.resolve()), "sha256": hashlib.sha256(data).hexdigest(),
              "objdump": str(args.objdump.resolve()), "passed": passed, "functions": rows}
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2), encoding="utf-8", newline="\n")
    print(json.dumps(report, indent=2))
    if not passed:
        print("FAIL PS/int comparisons require mod64=0 and VS-int update requires mod64=0; "
              "VS-float comparison requires offset mod64=16", file=sys.stderr)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
