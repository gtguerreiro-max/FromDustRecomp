"""Compare a rendered PNG with a reference PNG numerically (no need to look).

Usage: python tools/img_compare.py rendered.png reference.png [--diff out.png]
Prints per-channel means, mean absolute difference, PSNR and a 4x4 grid of
mean differences (which regions are wrong).
"""
import sys

import numpy as np
from PIL import Image


def main():
    a = Image.open(sys.argv[1]).convert("RGB")
    b = Image.open(sys.argv[2]).convert("RGB")
    a = np.asarray(a.resize(b.size, Image.NEAREST), dtype=np.float32)
    b = np.asarray(b, dtype=np.float32)
    diff = np.abs(a - b)
    mse = float(np.mean((a - b) ** 2))
    print("rendered mean RGB  ", np.round(a.mean((0, 1)), 1))
    print("reference mean RGB ", np.round(b.mean((0, 1)), 1))
    print("mean abs diff RGB  ", np.round(diff.mean((0, 1)), 1))
    print("PSNR %.2f dB, pixels off by >32: %.1f%%" % (10 * np.log10(255 ** 2 / max(mse, 1e-9)), 100 * float(np.mean(diff.max(2) > 32))))
    h, w = diff.shape[:2]
    for gy in range(4):
        print("  " + " ".join("%5.1f" % diff[gy * h // 4:(gy + 1) * h // 4, gx * w // 4:(gx + 1) * w // 4].mean() for gx in range(4)))
    if "--diff" in sys.argv:
        Image.fromarray(np.clip(diff.max(2), 0, 255).astype(np.uint8)).save(sys.argv[sys.argv.index("--diff") + 1])


if __name__ == "__main__":
    main()
