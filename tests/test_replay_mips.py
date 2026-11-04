import struct
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from xenos_mips import load_mips
from xenos_tiling import tiled_2d


class MipCaptureTests(unittest.TestCase):
    def capture(self, tiled):
        fetch = [2 | (1 << 22) | (int(tiled) << 31), 0x1000 | 18,
                 7 | (7 << 13), 1 << 6, 1 << 6, (1 << 9) | (1 << 11)]
        bank = bytearray(8192)
        offset = tiled_2d(2, 1, 32, 3) if tiled else 256 + 16
        bank[offset:offset + 8] = struct.pack("<HHI", 0xFFFF, 0xFFFF, 0x55555555)
        return (struct.pack("<IIQ", 0x31504D46, 1, 123) + struct.pack("<9I", 0x1000, *fetch, 1, len(bank)) +
                struct.pack("<4I", 256, 0, 2, 1) + bank)

    def test_packed_tail_offsets_with_linear_and_tiled_storage(self):
        for tiled in (False, True):
            with patch.object(Path, "read_bytes", return_value=self.capture(tiled)):
                levels = load_mips("mips.bin", 123)[0x1000]
            self.assertEqual(levels, [(4, 4, bytes([255] * 64))])

    def test_mixed_frame_metadata_and_truncated_banks_are_rejected(self):
        with patch.object(Path, "read_bytes", return_value=self.capture(True)):
            with self.assertRaisesRegex(ValueError, "different GPU register stream"):
                load_mips("mips.bin", 124)
        with patch.object(Path, "read_bytes", return_value=self.capture(True)[:-1]):
            with self.assertRaisesRegex(ValueError, "Invalid mip extent"):
                load_mips("mips.bin", 123)


if __name__ == "__main__":
    unittest.main()
