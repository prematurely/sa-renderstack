#!/usr/bin/env python3
"""Prepare bounded, exact-binary RSYM1 sidecars from PE COFF or an MSVC map.

COFF function auxiliary TotalSize is explicit (E). A next-symbol/section-end
boundary is inferred (I), including linker-map ranges. Aliases are retained.
The source digest records provenance; sidecars must travel with that exact DLL.
No downloads, external tools, or system-export guesses are used.
"""
import argparse
import ctypes
import hashlib
import pathlib
import re
import shutil
import struct
import subprocess
import sys

MAX_BINARY = 512 * 1024 * 1024
MAX_METADATA = 64 * 1024 * 1024
MAX_SYMBOLS = 250000
MAX_NAME = 4096


def bounded_read(path, limit):
    with pathlib.Path(path).open("rb") as source:
        data = source.read(limit + 1)
    if len(data) > limit:
        raise ValueError(f"input exceeds {limit} byte limit")
    return data


def unpack(data, fmt, offset):
    if offset < 0 or offset + struct.calcsize(fmt) > len(data):
        raise ValueError("truncated PE/COFF metadata")
    return struct.unpack_from(fmt, data, offset)


def parse_pe(data):
    if data[:2] != b"MZ":
        raise ValueError("missing DOS header")
    pe, = unpack(data, "<I", 0x3c)
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("missing PE signature")
    _, count, timestamp, pointer, symbols, optional, _ = unpack(data, "<HHIIIHH", pe + 4)
    if not 0 < count <= 96 or symbols > MAX_SYMBOLS * 4:
        raise ValueError("PE metadata count exceeds limit")
    magic, = unpack(data, "<H", pe + 24)
    if magic not in (0x10b, 0x20b) or optional < (96 if magic == 0x10b else 112):
        raise ValueError("unsupported PE optional header")
    image_base, = unpack(data, "<I" if magic == 0x10b else "<Q", pe + 24 + (28 if magic == 0x10b else 24))
    image_size, = unpack(data, "<I", pe + 24 + 56)
    sections = []
    for index in range(count):
        row = unpack(data, "<8sIIIIIIHHI", pe + 24 + optional + index * 40)
        _, virtual_size, rva, raw_size, _, _, _, _, _, flags = row
        end = rva + (virtual_size or raw_size)
        if rva >= image_size or end > image_size:
            raise ValueError("section exceeds image")
        sections.append((rva, end, bool(flags & 0x20000000)))
    return dict(timestamp=timestamp, base=image_base, size=image_size, sections=sections,
                symbol_pointer=pointer, symbol_count=symbols)


def check_name(name):
    encoded = name.encode("utf-8")
    if not encoded or len(encoded) > MAX_NAME or any(c in name for c in "\0\t\r\n"):
        raise ValueError("invalid or oversized symbol name")
    return name


def coff_symbols(data, pe):
    pointer, count = pe["symbol_pointer"], pe["symbol_count"]
    if not count:
        return []
    if not pointer:
        raise ValueError("missing COFF symbol table")
    string_at = pointer + count * 18
    string_size, = unpack(data, "<I", string_at)
    if string_size < 4 or string_at + string_size > len(data):
        raise ValueError("truncated COFF string table")
    rows = []
    index = 0
    while index < count:
        raw_name, value, section, kind, storage, aux = unpack(data, "<8sIhHBB", pointer + index * 18)
        if index + aux >= count:
            raise ValueError("truncated COFF auxiliary symbol")
        if 0 < section <= len(pe["sections"]) and (kind & 0x30) == 0x20 and storage in (2, 3):
            start, end, executable = pe["sections"][section - 1]
            if executable:
                if raw_name[:4] == b"\0" * 4:
                    offset, = struct.unpack_from("<I", raw_name, 4)
                    if offset < 4 or offset >= string_size:
                        raise ValueError("invalid COFF string offset")
                    terminator = data.find(b"\0", string_at + offset, string_at + string_size)
                    if terminator < 0 or terminator - string_at - offset > MAX_NAME:
                        raise ValueError("unterminated COFF symbol name")
                    name = data[string_at + offset:terminator].decode("utf-8", errors="strict")
                else:
                    name = raw_name.rstrip(b"\0").decode("utf-8", errors="strict")
                size = unpack(data, "<I", pointer + (index + 1) * 18 + 4)[0] if aux and storage == 2 else 0
                rva = start + value
                if rva >= end or (size and rva + size > end):
                    raise ValueError("COFF function exceeds executable section")
                rows.append((section, rva, size, check_name(name)))
        index += 1 + aux
    return rows


def map_symbols(metadata, pe):
    text = metadata.decode("utf-8-sig", errors="strict")
    timestamp = re.search(r"Timestamp is\s+([0-9a-fA-F]+)", text)
    base = re.search(r"Preferred load address is\s+([0-9a-fA-F]+)", text)
    if not timestamp or int(timestamp[1], 16) != pe["timestamp"]:
        raise ValueError("linker map timestamp does not match binary")
    if not base or int(base[1], 16) != pe["base"]:
        raise ValueError("linker map image base does not match binary")
    rows = []
    pattern = re.compile(r"^\s+([0-9a-fA-F]{4}):([0-9a-fA-F]{8,16})\s+(\S+)\s+([0-9a-fA-F]{8,16})\s+(.*)$")
    for line in text.splitlines():
        if len(line) > 16384:
            raise ValueError("linker map line exceeds limit")
        match = pattern.match(line)
        if not match or not match[5].split() or match[5].split()[0] != "f":
            continue
        section, offset, address = int(match[1], 16), int(match[2], 16), int(match[4], 16)
        if not 0 < section <= len(pe["sections"]):
            raise ValueError("linker map function has invalid section")
        start, end, executable = pe["sections"][section - 1]
        if not executable or address != pe["base"] + start + offset or not start <= address - pe["base"] < end:
            raise ValueError("linker map function address does not match PE section")
        rows.append((section, address - pe["base"], 0, check_name(match[3])))
        if len(rows) > MAX_SYMBOLS:
            raise ValueError("too many linker map symbols")
    return rows


def demangle_names(names, tool=None):
    """Optional, isolated build-time presentation. Never discard the raw name."""
    display = {name: name for name in names}
    msvc_names = [name for name in names if name.startswith("?")]
    if msvc_names and sys.platform == "win32":
        try:
            # DbgHelp is used serially in this short-lived Python process only.
            library = ctypes.WinDLL("dbghelp.dll", winmode=0x800)  # System32 only.
            undecorate = library.UnDecorateSymbolName
            undecorate.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32]
            undecorate.restype = ctypes.c_uint32
            for name in msvc_names:
                buffer = ctypes.create_string_buffer(MAX_NAME + 1)
                length = undecorate(name.encode("utf-8"), buffer, len(buffer), 0x1000)  # UNDNAME_NAME_ONLY.
                if 0 < length <= MAX_NAME:
                    try:
                        readable = check_name(buffer.value.decode("utf-8", errors="strict"))
                        if readable != name:
                            display[name] = readable
                    except (ValueError, UnicodeError):
                        pass
        except (AttributeError, OSError):
            print("MSVC demangler unavailable; preserving original names", file=sys.stderr)
    itanium = []
    for name in names:
        canonical = name[1:] if name.startswith("__Z") else name
        if canonical.startswith("_Z"):
            # i686 COFF adds a leading underscore and may append @stackbytes.
            canonical = re.sub(r"@\d+$", "", canonical)
            itanium.append((name, canonical))
    executable = tool or shutil.which("llvm-cxxfilt")
    if itanium and not executable:
        print("Itanium demangler unavailable; preserving original names", file=sys.stderr)
    if itanium and executable:
        try:
            for at in range(0, len(itanium), 512):
                batch = itanium[at:at + 512]
                result = subprocess.run([str(executable), "--no-strip-underscore", "--no-params"],
                                        input="\n".join(canonical for _, canonical in batch) + "\n",
                                        capture_output=True, encoding="utf-8", errors="strict", timeout=15)
                lines = result.stdout.splitlines()
                if result.returncode or len(lines) != len(batch):
                    print("Itanium demangler failed; preserving original names", file=sys.stderr)
                    continue
                for (raw, canonical), readable in zip(batch, lines):
                    try:
                        if readable != canonical:
                            display[raw] = check_name(readable)
                    except ValueError:
                        pass
        except (OSError, UnicodeError, subprocess.TimeoutExpired):
            print("Itanium demangler unavailable or failed; preserving original names", file=sys.stderr)
    return display


def prepare(binary, map_path=None, demangle_tool=None):
    data = bounded_read(binary, MAX_BINARY)
    pe = parse_pe(data)
    metadata = bounded_read(map_path, MAX_METADATA) if map_path else data
    rows = map_symbols(metadata, pe) if map_path else coff_symbols(data, pe)
    if len(rows) > MAX_SYMBOLS:
        raise ValueError("too many function symbols")
    rows = sorted(set(rows))
    display_names = demangle_names(sorted({name for _, _, _, name in rows}), demangle_tool)
    boundaries = {}
    for section, rva, _, _ in rows:
        boundaries.setdefault(section, set()).add(rva)
    next_start = {}
    for section, starts in boundaries.items():
        ordered = sorted(starts)
        for index, start in enumerate(ordered):
            next_start[(section, start)] = ordered[index + 1] if index + 1 < len(ordered) else pe["sections"][section - 1][1]
    lines = ["RSYM1", "sha256 " + hashlib.sha256(data).hexdigest(), f"image_size {pe['size']:08x}",
             "source_kind " + ("map" if map_path else "coff"), "source_sha256 " + hashlib.sha256(metadata).hexdigest()]
    for start, end, executable in pe["sections"]:
        if executable:
            lines.append(f"section {start:08x} {end:08x}")
    for section, start, size, name in rows:
        end = start + size if size else next_start[(section, start)]
        display_name = display_names[name]
        lines.append(f"symbol {start:08x} {end:08x} {'E' if size else 'I'} {display_name.encode('utf-8').hex()} {name.encode('utf-8').hex()}")
    result = "\n".join(lines + ["end", ""])
    if len(result.encode("ascii")) > MAX_METADATA:
        raise ValueError("prepared sidecar exceeds limit")
    return result, len(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--map", help="MSVC map from the same link; timestamp/base/section addresses are checked")
    parser.add_argument("--output", required=True)
    parser.add_argument("--demangle-tool", help="optional llvm-cxxfilt executable for build-time Itanium names; raw names survive failure")
    args = parser.parse_args()
    try:
        content, count = prepare(args.binary, args.map, args.demangle_tool)
        output = pathlib.Path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(content, encoding="ascii", newline="\n")
        print(f"Prepared {count} function records for {pathlib.Path(args.binary).resolve()} -> {output.resolve()}")
        return 0
    except (ValueError, OSError, UnicodeError, struct.error) as error:
        print(f"symbol preparation failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
