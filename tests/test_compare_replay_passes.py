"""Keep diagnostic comparisons from hiding negative or out-of-range heights."""
import struct
import sys
import unittest
from pathlib import Path
from unittest.mock import Mock

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from compare_replay_passes import native_pixels
from img_compare import image_metrics


class ResolveReadbackTests(unittest.TestCase):
    def test_signed_half_heights_are_not_clamped(self):
        path = Mock()
        path.read_bytes.return_value = struct.pack("<4I4e", 2, 1, 26, 4, -32, 24.5, -1, 0.5)
        np.testing.assert_array_equal(native_pixels(path), [[[-32, 24.5], [-1, 0.5]]])

    def test_unorm_bytes_use_unit_scale(self):
        path = Mock()
        path.read_bytes.return_value = struct.pack("<4I", 1, 1, 20, 4) + bytes((0, 255, 128, 255))
        np.testing.assert_allclose(native_pixels(path), [[[0, 1, 128 / 255, 1]]])

    def test_fixed16_snorm_retains_xenos_range(self):
        path = Mock()
        path.read_bytes.return_value = struct.pack("<4I4h", 2, 1, 29, 4, -32768, 32767, 0, 0)
        np.testing.assert_array_equal(native_pixels(path), [[[-32, 32], [0, 0]]])


class ImageMetricTests(unittest.TestCase):
    def test_outlier_boundary_and_mse_do_not_wrap_unsigned_bytes(self):
        rendered = np.array([[[32, 0, 0], [33, 0, 0]]], dtype=np.uint8)
        reference = np.zeros_like(rendered)
        result = image_metrics(reference, rendered)
        self.assertEqual(result["pixels_over_32"], 1)
        self.assertEqual(result["percentage_over_32"], 50)
        self.assertAlmostEqual(result["psnr"], 10 * np.log10(255 ** 2 * 6 / (32 ** 2 + 33 ** 2)))


if __name__ == "__main__":
    unittest.main()
