import struct
import tempfile
import unittest
from pathlib import Path

from tools.android.validate_runtime_elf import ElfError, inspect, rewrite_interpreter


def fake_elf(path, interpreter, capacity=64):
    """Create the smallest ELF64 image needed to exercise PT_INTERP handling."""
    header_size = 64
    phoff = header_size
    phentsize = 56
    interp_offset = header_size + phentsize
    value = interpreter.encode() + b"\0"
    if len(value) > capacity:
        raise ValueError("test interpreter does not fit its fixture")
    header = bytearray(64)
    header[:4] = b"\x7fELF"
    header[4:6] = b"\x02\x01"  # ELF64, little-endian
    struct.pack_into("<HHIQQQIHHHHHH", header, 16,
                     2, 183, 1, 0, phoff, 0, 0, 64, phentsize, 1, 0, 0, 0)
    phdr = struct.pack("<IIQQQQQQ", 3, 0, interp_offset, 0, 0, capacity, capacity, 1)
    path.write_bytes(bytes(header) + phdr + value.ljust(capacity, b"\0"))


class RuntimeElfTests(unittest.TestCase):
    def test_rewrite_preserves_fixed_interpreter_slot(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "probe"
            fake_elf(path, "/lib/ld-linux-aarch64.so.1")
            old = inspect(path)
            updated = rewrite_interpreter(path, "/data/user/0/gow3/rootfs/ld.so")
            self.assertEqual(old["interpreter_capacity"], updated["interpreter_capacity"])
            self.assertEqual(updated["interpreter"], "/data/user/0/gow3/rootfs/ld.so")

    def test_rewrite_rejects_interpreter_growth(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "probe"
            fake_elf(path, "/lib/ld.so", capacity=16)
            with self.assertRaisesRegex(ElfError, "explicit loader"):
                rewrite_interpreter(path, "/data/user/0/com.gow3.android/files/rootfs/ld.so")

