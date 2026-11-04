"""Exact-event comparisons must preserve signed data and reject ambiguous joins."""
import struct
import sys
import unittest
from pathlib import Path
from unittest.mock import Mock

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/rdoc"))
from compare_passes import align_draws, compare, dds_pixels


class RenderDocReplayTests(unittest.TestCase):
    def test_snorm_and_edram_padding(self):
        header = bytearray(148)
        header[:4] = b"DDS "
        struct.pack_into("<2I", header, 12, 1, 3)
        header[84:88] = b"DX10"
        struct.pack_into("<I", header, 128, 37)
        path = Mock()
        path.read_bytes.return_value = header + struct.pack("<6h", -32768, 32767, 0, 0, 123, 456)
        pixels, fmt = dds_pixels(path)
        native = np.array([[[-32, 32], [0, 0]]], dtype=np.float32)
        self.assertEqual(compare(native, pixels, fmt)["maximum"], 0)

    def test_legacy_bgra_masks(self):
        header = bytearray(128)
        header[:4] = b"DDS "
        struct.pack_into("<2I", header, 12, 1, 1)
        struct.pack_into("<I", header, 80, 0x41)
        struct.pack_into("<5I", header, 88, 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000)
        path = Mock()
        path.read_bytes.return_value = header + bytes((0, 128, 255, 255))
        pixels, _ = dds_pixels(path)
        np.testing.assert_allclose(pixels, [[[1, 128 / 255, 0, 1]]])

    def test_join_skips_internal_clear_draws(self):
        records = [dict(kind="draw", record=7, ps="088", indices=3)]
        events = [dict(event=30, ps=None, indices=1), dict(event=311, ps="HASH", indices=3)]
        self.assertEqual(align_draws(records, events, {("088", "ps"): "HASH"})[7]["event"], 311)

    def test_join_rejects_ambiguous_or_wrong_shader(self):
        records = [dict(kind="draw", record=7, ps="088", indices=3)]
        events = [dict(event=311, ps="HASH", indices=3), dict(event=425, ps="HASH", indices=3)]
        with self.assertRaisesRegex(ValueError, "ambiguous"):
            align_draws(records, events, {("088", "ps"): "HASH"})
        with self.assertRaisesRegex(ValueError, "no complete"):
            align_draws(records, events, {("088", "ps"): "OTHER"})


if __name__ == "__main__":
    unittest.main()
