"""Behavior tests: real PE/COFF bytes, real preparation CLI, real native resolver.

Each case catches incorrect identity/range acceptance or invented symbol precision.
Run with --probe <built measurement-symbols-probe.exe>; no game process is used.
"""
import hashlib
import json
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/prepare-measurement-symbols.py"
LLVM_CXXFILT = pathlib.Path(shutil.which("llvm-cxxfilt") or "")
if "--demangle-tool" in sys.argv:
    at = sys.argv.index("--demangle-tool")
    LLVM_CXXFILT = pathlib.Path(sys.argv[at + 1]).resolve()
    del sys.argv[at:at + 2]
PROBE = None
if "--probe" in sys.argv:
    at = sys.argv.index("--probe")
    PROBE = pathlib.Path(sys.argv[at + 1]).resolve()
    del sys.argv[at:at + 2]


def pe_bytes(symbols=(), timestamp=0x12345678):
    """Tiny PE32 with a real COFF table; literal .text RVA [0x1000,0x1100)."""
    data = bytearray(0x400)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3c, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    count = sum(1 + bool(size) for _, _, size in symbols)
    struct.pack_into("<HHIIIHH", data, 0x84, 0x14c, 2, timestamp, 0x400, count, 224, 0x2102)
    struct.pack_into("<H", data, 0x98, 0x10b)
    struct.pack_into("<I", data, 0x98 + 28, 0x10000000)
    struct.pack_into("<I", data, 0x98 + 56, 0x3000)
    for index, (name, rva, flags) in enumerate(((b".text", 0x1000, 0x60000020), (b".data", 0x2000, 0xc0000040))):
        at = 0x178 + index * 40
        struct.pack_into("<8sIIIIIIHHI", data, at, name, 0x100, rva, 0x100, 0x200 + index * 0x100, 0, 0, 0, 0, flags)
    strings = bytearray(b"\0\0\0\0")
    for name, value, size in symbols:
        encoded = name.encode()
        if len(encoded) > 8:
            n = struct.pack("<II", 0, len(strings))
            strings.extend(encoded + b"\0")
        else:
            n = encoded.ljust(8, b"\0")
        data.extend(struct.pack("<8sIhHBB", n, value, 1, 0x20, 2, bool(size)))
        if size:
            data.extend(struct.pack("<IIIIH", 0, size, 0, 0, 0))
    struct.pack_into("<I", strings, 0, len(strings))
    data.extend(strings)
    return bytes(data)


def sidecar(binary, rows=(), digest=None):
    content = ["RSYM1", "sha256 " + (digest or hashlib.sha256(binary).hexdigest()),
               "image_size 00003000", "source_kind coff", "source_sha256 " + hashlib.sha256(binary).hexdigest(),
               "section 00001000 00001100"]
    for start, end, quality, name in rows:
        content.append(f"symbol {start:08x} {end:08x} {quality} {name.encode().hex()}")
    return "\n".join(content + ["end", ""])


def export_pe_bytes(exports=(), export_in_code=False):
    """PE export directory with literal EAT/ENT/EOT tables and named aliases.

    Export name None is an ordinal-only entry; target strings are forwarders.
    """
    data = bytearray(pe_bytes()[:0x400])
    data.extend(b"\0" * 0x400)
    struct.pack_into("<II", data, 0x84 + 8, 0, 0)
    struct.pack_into("<I", data, 0x98 + 92, 16)
    directory_rva = 0x1000 if export_in_code else 0x2000
    directory_file = 0x200 if export_in_code else 0x300
    struct.pack_into("<II", data, 0x98 + 96, directory_rva, 0x200)
    if export_in_code:
        struct.pack_into("<I", data, 0x178 + 8, 0x500)
        struct.pack_into("<I", data, 0x178 + 16, 0x500)
        struct.pack_into("<I", data, 0x178 + 40 + 20, 0x700)
    else:
        struct.pack_into("<I", data, 0x178 + 40 + 8, 0x500)
        struct.pack_into("<I", data, 0x178 + 40 + 16, 0x500)
    named = [(index, name) for index, (name, _) in enumerate(exports) if name is not None]
    struct.pack_into("<IIHHIIIIIII", data, directory_file, 0, 0, 0, 0, directory_rva + 0xe0,
                     1, len(exports), len(named), directory_rva + 0x40,
                     directory_rva + 0x80, directory_rva + 0xc0)
    data[directory_file + 0xe0:directory_file + 0xeb] = b"fixture.dll"
    name_at, forward_at = 0x100, 0x1c0
    for index, (_, target) in enumerate(exports):
        if isinstance(target, str):
            encoded = target.encode() + b"\0"
            data[directory_file + forward_at:directory_file + forward_at + len(encoded)] = encoded
            target = directory_rva + forward_at
            forward_at += len(encoded)
        struct.pack_into("<I", data, directory_file + 0x40 + index * 4, target)
    for index, (ordinal_index, name) in enumerate(named):
        encoded = name.encode() + b"\0"
        struct.pack_into("<I", data, directory_file + 0x80 + index * 4, directory_rva + name_at)
        struct.pack_into("<H", data, directory_file + 0xc0 + index * 2, ordinal_index)
        data[directory_file + name_at:directory_file + name_at + len(encoded)] = encoded
        name_at += len(encoded)
    return bytes(data)


class PreparationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.dir = pathlib.Path(self.temp.name)
        self.binary = self.dir / "d3d9.dll"
        self.output = self.dir / "symbols.rsym"

    def prepare(self, data, *args):
        self.binary.write_bytes(data)
        return subprocess.run([sys.executable, str(TOOL), "--binary", str(self.binary),
                               "--output", str(self.output), *args], capture_output=True, text=True)

    def test_coff_explicit_size_is_exact_but_next_symbol_range_is_inferred(self):
        result = self.prepare(pe_bytes((("Exact", 0, 0x10), ("Inferred", 0x20, 0), ("Tail", 0x50, 0))))
        self.assertEqual(result.returncode, 0, result.stderr)
        text = self.output.read_text()
        self.assertIn("symbol 00001000 00001010 E 4578616374", text)
        self.assertIn("symbol 00001020 00001050 I 496e666572726564", text)
        self.assertIn("symbol 00001050 00001100 I 5461696c", text)
        self.assertIn(hashlib.sha256(self.binary.read_bytes()).hexdigest(), text)

    def test_aliases_are_preserved_for_ambiguous_runtime_attribution(self):
        result = self.prepare(pe_bytes((("A", 0, 0), ("B", 0, 0), ("Next", 0x10, 0))))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("symbol 00001000 00001010 I 41", self.output.read_text())
        self.assertIn("symbol 00001000 00001010 I 42", self.output.read_text())

    def test_map_timestamp_mismatch_rejected_without_output(self):
        map_path = self.dir / "d3d9.map"
        map_path.write_text(" Timestamp is 87654321\n Preferred load address is 10000000\n 0001:00000000 _A 10001000 f x.obj\n")
        result = self.prepare(pe_bytes(), "--map", str(map_path))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("timestamp", result.stderr.lower())
        self.assertFalse(self.output.exists())

    def test_valid_map_has_inferred_only_ranges_and_source_digest(self):
        map_path = self.dir / "d3d9.map"
        metadata = " Timestamp is 12345678\n Preferred load address is 10000000\n 0001:00000000 _A 10001000 f x.obj\n 0001:00000020 _B 10001020 f x.obj\n"
        map_path.write_bytes(metadata.encode())
        result = self.prepare(pe_bytes(), "--map", str(map_path))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("source_sha256 " + hashlib.sha256(metadata.encode()).hexdigest(), self.output.read_text())
        self.assertIn("symbol 00001000 00001020 I 5f41", self.output.read_text())

    def test_truncated_coff_fails_without_output(self):
        result = self.prepare(pe_bytes((("A", 0, 0),))[:-3])
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())

    def test_no_symbols_is_valid_missing_data(self):
        result = self.prepare(pe_bytes())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("symbol ", self.output.read_text())

    @unittest.skipUnless(sys.platform == "win32", "Windows system demangler")
    def test_msvc_demangling_preserves_raw_name_and_binary_binding(self):
        raw = "?Example@Widget@@QAEXH@Z"
        result = self.prepare(pe_bytes(((raw, 0, 0x10),)))
        self.assertEqual(result.returncode, 0, result.stderr)
        row = next(line.split() for line in self.output.read_text().splitlines() if line.startswith("symbol "))
        self.assertEqual(bytes.fromhex(row[4]).decode(), "Widget::Example")
        self.assertEqual(bytes.fromhex(row[5]).decode(), raw)
        self.assertIn(hashlib.sha256(self.binary.read_bytes()).hexdigest(), self.output.read_text())

    @unittest.skipUnless(LLVM_CXXFILT.is_file(), "LLVM demangler is optional")
    def test_itanium_coff_prefix_and_stdcall_suffix_are_demangled(self):
        raw = "__ZN4dxvk12D3D9DeviceEx7PresentEv@4"
        result = self.prepare(pe_bytes(((raw, 0, 0x10),)), "--demangle-tool", str(LLVM_CXXFILT))
        self.assertEqual(result.returncode, 0, result.stderr)
        row = next(line.split() for line in self.output.read_text().splitlines() if line.startswith("symbol "))
        self.assertEqual(bytes.fromhex(row[4]).decode(), "dxvk::D3D9DeviceEx::Present")
        self.assertEqual(bytes.fromhex(row[5]).decode(), raw)

    def test_missing_optional_demangler_preserves_original_name(self):
        raw = "__ZN4dxvk12D3D9DeviceEx7PresentEv@4"
        result = self.prepare(pe_bytes(((raw, 0, 0x10),)), "--demangle-tool", str(self.dir / "absent-cxxfilt.exe"))
        self.assertEqual(result.returncode, 0, result.stderr)
        row = next(line.split() for line in self.output.read_text().splitlines() if line.startswith("symbol "))
        self.assertEqual([bytes.fromhex(name).decode() for name in row[4:]], [raw, raw])
        self.assertIn("preserv", result.stderr.lower())

    @unittest.skipUnless(sys.platform == "win32", "Windows system demangler")
    def test_demangling_does_not_merge_raw_aliases(self):
        names = ("?Example@Widget@@QAEXH@Z", "?Example@Widget@@QAEXM@Z")
        result = self.prepare(pe_bytes(tuple((name, 0, 0x10) for name in names)))
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = [line.split() for line in self.output.read_text().splitlines() if line.startswith("symbol ")]
        self.assertEqual(len(rows), 2)
        self.assertEqual({bytes.fromhex(row[4]).decode() for row in rows}, {"Widget::Example"})
        self.assertEqual({bytes.fromhex(row[5]).decode() for row in rows}, set(names))


@unittest.skipUnless(PROBE, "pass --probe for native resolver cases")
class ResolverTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.dir = pathlib.Path(self.temp.name)
        self.data = pe_bytes()

    def module(self, subdir="a", text=None, data=None):
        directory = self.dir / subdir
        directory.mkdir(parents=True, exist_ok=True)
        binary = directory / "d3d9.dll"
        binary.write_bytes(data or self.data)
        symbols = directory / "symbols.rsym"
        symbols.write_text(text if text is not None else sidecar(self.data, ((0x1000, 0x1010, "E", "Alpha"), (0x1020, 0x1100, "I", "Beta"))))
        return str(binary), str(symbols)

    def resolve(self, modules, addresses):
        args = [str(PROBE)]
        for binary, symbols, base in modules:
            args += [binary, symbols, str(base)]
        result = subprocess.run(args + ["--"] + [str(a) for a in addresses], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return [line.split("\t") for line in result.stdout.splitlines()]

    def test_exact_inferred_missing_and_section_boundary(self):
        binary, symbols = self.module()
        result = self.resolve([(binary, symbols, 0x10000000)], [0x10001003, 0x10001010, 0x10001021, 0x10001100, 0x10003000])
        self.assertEqual([r[1] for r in result[1:]], ["exact", "missing", "inferred", "missing", "module-not-found"])
        self.assertEqual(result[1][2:5], ["268435456", "4099", "3"])
        self.assertEqual(result[1][6], "Alpha")

    def test_same_basename_modules_keep_distinct_path_base_and_rva(self):
        first = self.module("bridge")
        second = self.module("backend", sidecar(self.data, ((0x1000, 0x1010, "E", "Backend"),)))
        result = self.resolve([(*first, 0x10000000), (*second, 0x20000000)], [0x10001003, 0x20001004])
        self.assertEqual(result[2][6], "Alpha")
        self.assertEqual(result[3][6], "Backend")
        self.assertNotEqual(result[2][5], result[3][5])
        self.assertEqual(result[3][2:5], ["536870912", "4100", "4"])

    def test_wrong_binary_digest_never_names_function(self):
        binary, symbols = self.module(text=sidecar(self.data, ((0x1000, 0x1010, "E", "Wrong"),), "0" * 64))
        result = self.resolve([(binary, symbols, 0x10000000)], [0x10001000])
        self.assertEqual(result[0][1], "identity-mismatch")
        self.assertEqual(result[1][1], "identity-mismatch")
        self.assertEqual(result[1][6], "")

    def test_map_cannot_claim_explicit_function_size(self):
        text = sidecar(self.data, ((0x1000, 0x1010, "E", "Unproved"),)).replace("source_kind coff", "source_kind map")
        binary, symbols = self.module(text=text)
        result = self.resolve([(binary, symbols, 0x10000000)], [0x10001001])
        self.assertEqual(result[0][1], "invalid-sidecar")
        self.assertEqual(result[1][1], "invalid-sidecar")

    def test_coff_provenance_digest_must_be_the_binary_digest(self):
        text = sidecar(self.data, ((0x1000, 0x1010, "E", "WrongSource"),))
        text = text.replace("source_sha256 " + hashlib.sha256(self.data).hexdigest(), "source_sha256 " + "1" * 64)
        binary, symbols = self.module(text=text)
        result = self.resolve([(binary, symbols, 0x10000000)], [0x10001001])
        self.assertEqual(result[0][1], "identity-mismatch")
        self.assertEqual(result[1][1], "identity-mismatch")

    def test_aliases_and_overlaps_are_ambiguous(self):
        binary, symbols = self.module(text=sidecar(self.data, ((0x1000, 0x1040, "E", "A"), (0x1000, 0x1010, "I", "Alias"), (0x1020, 0x1050, "E", "Overlap"))))
        result = self.resolve([(binary, symbols, 0x10000000)], [0x10001003, 0x10001025, 0x10001045])
        self.assertEqual([r[1] for r in result[1:]], ["ambiguous", "ambiguous", "exact"])
        self.assertEqual(result[1][6], "")

    def test_missing_sidecar_keeps_module_identity(self):
        binary, symbols = self.module()
        pathlib.Path(symbols).unlink()
        result = self.resolve([(binary, symbols, 0x10000000)], [0x10001000])
        self.assertEqual(result[1][1], "missing")
        self.assertEqual(result[1][5], binary)

    def test_malformed_truncated_oversize_and_out_of_section_inputs_rejected(self):
        valid = sidecar(self.data)
        cases = [valid.replace("end\n", ""), valid + "garbage\n", valid.replace("RSYM1", "RSYM2"),
                 sidecar(self.data, ((0x1000, 0x2001, "E", "Bad"),)), valid.replace("00001100", "00001101"),
                 sidecar(self.data, ((0x1000, 0x1000, "E", "Bad"),)), valid.replace("source_kind coff", "source_kind " + "x" * 17000)]
        for index, text in enumerate(cases):
            with self.subTest(index=index):
                binary, symbols = self.module(str(index), text)
                result = self.resolve([(binary, symbols, 0x10000000)], [0x10001000])
                self.assertEqual(result[0][1], "invalid-sidecar")
                self.assertEqual(result[1][1], "invalid-sidecar")

    def test_exports_resolve_only_executable_ranges_with_inferred_provenance(self):
        data = export_pe_bytes((("First", 0x1000), ("Second", 0x1020), ("Data", 0x2300)))
        binary, _ = self.module(data=data)
        result = self.resolve([(binary, "@exports", 0x10000000)], [0x1000100c, 0x10001020, 0x10001100, 0x10002300])
        self.assertEqual(result[0][1], "ready")
        self.assertEqual([row[1] for row in result[1:]], ["inferred", "inferred", "missing", "missing"])
        self.assertEqual(result[1][4], "12")
        self.assertEqual(result[1][6:8], ["First", "exports"])
        self.assertEqual(result[2][6], "Second")

    def test_export_aliases_are_visible_without_arbitrary_primary_name(self):
        data = export_pe_bytes((("NtQueryVirtualMemory", 0x1000), ("ZwQueryVirtualMemory", 0x1000), ("Next", 0x1040)))
        binary, _ = self.module(data=data)
        result = self.resolve([(binary, "@exports", 0x10000000)], [0x1000100c, 0x10001040])
        self.assertEqual(result[0][1], "ready")
        self.assertEqual(result[1][1], "ambiguous")
        self.assertEqual(result[1][4], "12")
        self.assertEqual(result[1][6:9], ["", "exports", "NtQueryVirtualMemory|ZwQueryVirtualMemory"])
        self.assertEqual(result[2][6], "Next")

    def test_forwarded_exports_in_executable_section_are_not_function_ranges(self):
        data = export_pe_bytes((("Forwarded", "KERNEL32.Sleep"), ("Real", 0x1300)), export_in_code=True)
        binary, _ = self.module(data=data)
        result = self.resolve([(binary, "@exports", 0x10000000)], [0x100011c0, 0x1000130c, 0x10001500])
        self.assertEqual(result[0][1], "ready")
        self.assertEqual([row[1] for row in result[1:]], ["missing", "inferred", "missing"])
        self.assertEqual(result[2][6:8], ["Real", "exports"])

    def test_ordinal_only_export_bounds_preceding_named_export(self):
        data = export_pe_bytes((("Named", 0x1000), (None, 0x1010), ("Later", 0x1040)))
        binary, _ = self.module(data=data)
        result = self.resolve([(binary, "@exports", 0x10000000)], [0x1000100f, 0x10001010, 0x10001030])
        self.assertEqual(result[0][1], "ready")
        self.assertEqual([row[1] for row in result[1:]], ["inferred", "missing", "missing"])

    def test_export_registration_cannot_hide_same_base_sidecar_identity_mismatch(self):
        data = export_pe_bytes((("Exported", 0x1000),))
        binary, symbols = self.module(data=data, text=sidecar(data, digest="0" * 64))
        result = self.resolve([(binary, symbols, 0x10000000), (binary, "@exports", 0x10000000)], [0x10001001])
        self.assertEqual([row[1] for row in result], ["identity-mismatch", "module-conflict", "identity-mismatch"])
        self.assertEqual(result[2][6], "")

    def test_same_base_export_registration_preserves_original_module(self):
        first, _ = self.module("first", data=export_pe_bytes((("Original", 0x1000),)))
        second, _ = self.module("second", data=export_pe_bytes((("Replacement", 0x1000),)))
        result = self.resolve([(first, "@exports", 0x10000000), (second, "@exports", 0x10000000)], [0x10001001])
        self.assertEqual([row[1] for row in result], ["ready", "module-conflict", "inferred"])
        self.assertEqual(result[2][6], "Original")
        self.assertEqual(result[2][5], first)

    def test_malformed_export_tables_are_rejected(self):
        valid = export_pe_bytes((("Valid", 0x1000),))
        cases = []
        for offset, fmt, value in ((0x300 + 28, "<I", 0x2fff), (0x300 + 24, "<I", 250001),
                                   (0x300 + 0xc0, "<H", 3), (0x300 + 0x80, "<I", 0x2fff)):
            changed = bytearray(valid)
            struct.pack_into(fmt, changed, offset, value)
            cases.append(bytes(changed))
        for index, data in enumerate(cases):
            with self.subTest(index=index):
                binary, _ = self.module(str(index), data=data)
                result = self.resolve([(binary, "@exports", 0x10000000)], [0x10001001])
                self.assertEqual([row[1] for row in result], ["binary-unavailable", "module-not-found"])

    def test_binary_without_exports_preserves_missing_module_identity(self):
        binary, _ = self.module()
        result = self.resolve([(binary, "@exports", 0x10000000)], [0x10001001])
        self.assertEqual([row[1] for row in result], ["missing", "missing"])
        self.assertEqual(result[1][5], binary)

    def test_prepared_readable_name_retains_original_raw_name(self):
        text = sidecar(self.data, ((0x1000, 0x1010, "E", "Widget::Example"),))
        raw = "?Example@Widget@@QAEXH@Z"
        text = text.replace("E " + "Widget::Example".encode().hex(), "E " + "Widget::Example".encode().hex() + " " + raw.encode().hex())
        binary, symbols = self.module(text=text)
        result = self.resolve([(binary, symbols, 0x10000000)], [0x10001001])
        self.assertEqual(result[0][1], "ready")
        self.assertEqual(result[1][6:8], ["Widget::Example", "coff"])
        self.assertEqual(result[1][9], raw)


if __name__ == "__main__":
    unittest.main(verbosity=2)
