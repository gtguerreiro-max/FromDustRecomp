"""Presentation checks must catch alpha that is invisible to RGB metrics."""
import contextlib
import io
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import img_compare


class PresentationTests(unittest.TestCase):
    def compare(self, rendered_alpha, reference_alpha):
        pictures = [Image.new("RGB" if alpha is None else "RGBA", (2, 2),
                             (16, 32, 64) if alpha is None else (16, 32, 64, alpha))
                    for alpha in (rendered_alpha, reference_alpha)]
        with patch.object(img_compare.Image, "open", side_effect=pictures), \
             patch.object(sys, "argv", ["img_compare", "rendered.png", "reference.png", "--require-opaque"]), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            img_compare.main()

    def test_transparent_render_cannot_pass_with_matching_rgb(self):
        with self.assertRaises(SystemExit) as result:
            self.compare(32, None)
        self.assertEqual(result.exception.code, 2)

    def test_transparent_reference_is_also_rejected(self):
        with self.assertRaises(SystemExit) as result:
            self.compare(None, 32)
        self.assertEqual(result.exception.code, 2)

    def test_rgb_and_opaque_rgba_are_accepted(self):
        self.compare(None, 255)
        self.compare(255, None)


if __name__ == "__main__":
    unittest.main()
