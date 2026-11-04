"""Known BC blocks catch palette-index and channel-layout errors in replay inputs."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from xenos_texture import decode_texture
from xenos_tiling import tiled_2d


def decode(fmt, block):
    return decode_texture(block, dict(width=4, height=4, pitch=4, format_name=fmt))[2]


class TextureBlockTests(unittest.TestCase):
    def test_packed_base_level_uses_its_offset_for_tall_wide_and_square_images(self):
        for width, height in ((16, 64), (64, 16), (8, 8), (12, 9)):
            for tiled in (False, True):
                block = struct.pack('<HHI', 0xF800, 0xF800, 0)
                raw = bytearray(8192)
                wide = (width - 1).bit_length() > max((height - 1).bit_length(), 1)
                x0, y0 = (0, 4) if wide else (4, 0)
                for y in range((height + 3) // 4):
                    for x in range((width + 3) // 4):
                        offset = tiled_2d(x + x0, y + y0, 32, 3) if tiled else ((y + y0) * 32 + x + x0) * 8
                        raw[offset:offset + 8] = block
                info = dict(width=width, height=height, pitch=32, tiled=tiled,
                            format_name='k_DXT1', packed_mips=True, dimension=1)
                self.assertEqual(decode_texture(raw, info), (width, height, bytes((255, 0, 0, 255)) * width * height))
                with self.assertRaisesRegex(ValueError, 'Packed base mip'):
                    decode_texture(bytes(8), info)

    def test_bc1_stored_endpoints_are_indices_zero_and_one(self):
        block = struct.pack("<HHI", 0xF800, 0x001F, 0xE4E4E4E4)
        self.assertEqual(list(decode("k_DXT1", block)[:16]),
                         [255, 0, 0, 255, 0, 0, 255, 255, 170, 0, 85, 255, 85, 0, 170, 255])

    def test_bc1_three_colour_and_equal_endpoint_transparency(self):
        block = struct.pack("<HHI", 0, 0xFFFF, 0xE4E4E4E4)
        self.assertEqual(list(decode("k_DXT1", block)[:16]),
                         [0, 0, 0, 255, 255, 255, 255, 255, 127, 127, 127, 255, 0, 0, 0, 0])
        opaque_white = struct.pack("<HHI", 0xFFFF, 0xFFFF, 0x55555555)
        self.assertEqual(decode("k_DXT1", opaque_white), bytes([255] * 64))

    def test_bc2_and_bc3_always_use_four_colour_palette(self):
        colour = struct.pack("<HHI", 0, 0xFFFF, 0xFFFFFFFF)
        for fmt, alpha in (("k_DXT2_3", bytes([255] * 8)),
                           ("k_DXT4_5", bytes([255, 0]) + bytes(6))):
            self.assertEqual(decode(fmt, alpha + colour), bytes([170, 170, 170, 255] * 16))

    def test_bc5_uses_two_endpoint_and_index_blocks(self):
        indices = sum((p % 8) << (3 * p) for p in range(16)).to_bytes(6, "little")
        red = bytes((255, 0)) + indices
        green = bytes((0, 255)) + indices
        pixels = decode("k_DXN", red + green)
        self.assertEqual([pixels[p * 4] for p in range(8)], [255, 0, 218, 182, 145, 109, 72, 36])
        self.assertEqual([pixels[p * 4 + 1] for p in range(8)], [0, 255, 51, 102, 153, 204, 0, 255])


if __name__ == "__main__":
    unittest.main()
