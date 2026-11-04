"""Compare a rendered PNG with a reference PNG numerically (no need to look).

Usage: python tools/img_compare.py rendered.png reference.png [--diff out.png]
Prints per-channel means, mean absolute difference, PSNR and a 4x4 grid of
mean differences (which regions are wrong).
"""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


def image_metrics(a, b):
    diff = np.abs(a.astype(np.float64) - b.astype(np.float64))
    mse = float(np.mean(diff ** 2))
    count = int(np.count_nonzero(diff.max(2) > 32))
    total = diff.shape[0] * diff.shape[1]
    return dict(psnr=float(10 * np.log10(255 ** 2 / max(mse, 1e-9))),
                mae=diff.mean((0, 1)).tolist(), pixels_over_32=count, total_pixels=total,
                percentage_over_32=100 * count / total)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rendered", type=Path)
    parser.add_argument("reference", type=Path)
    parser.add_argument("--diff", type=Path)
    parser.add_argument("--json", type=Path)
    parser.add_argument("--strict-size", action="store_true")
    parser.add_argument("--require-opaque", action="store_true", help="reject presentation PNGs with transparent pixels")
    parser.add_argument("--min-psnr", type=float, default=0)
    parser.add_argument("--max-outlier-percent", type=float, default=100)
    args = parser.parse_args()
    a = Image.open(args.rendered)
    b = Image.open(args.reference)
    if args.require_opaque:
        for label, picture in (("rendered", a), ("reference", b)):
            if picture.convert("RGBA").getchannel("A").getextrema()[0] != 255:
                parser.error(label + " presentation image is not opaque")
    a, b = a.convert("RGB"), b.convert("RGB")
    if args.strict_size and a.size != b.size:
        parser.error("rendered and reference dimensions differ")
    a = np.asarray(a.resize(b.size, Image.NEAREST), dtype=np.float32)
    b = np.asarray(b, dtype=np.float32)
    diff = np.abs(a - b)
    metrics = image_metrics(a, b)
    metrics["passed"] = metrics["psnr"] >= args.min_psnr and metrics["percentage_over_32"] <= args.max_outlier_percent
    print("rendered mean RGB  ", np.round(a.mean((0, 1)), 1))
    print("reference mean RGB ", np.round(b.mean((0, 1)), 1))
    print("mean abs diff RGB  ", np.round(diff.mean((0, 1)), 1))
    print("PSNR %.2f dB, pixels off by >32: %.4f%% (%d/%d)" %
          (metrics["psnr"], metrics["percentage_over_32"], metrics["pixels_over_32"], metrics["total_pixels"]))
    h, w = diff.shape[:2]
    for gy in range(4):
        print("  " + " ".join("%5.1f" % diff[gy * h // 4:(gy + 1) * h // 4, gx * w // 4:(gx + 1) * w // 4].mean() for gx in range(4)))
    if args.diff:
        Image.fromarray(np.clip(diff.max(2), 0, 255).astype(np.uint8)).save(args.diff)
    if args.json:
        args.json.write_text(json.dumps(metrics, indent=2), encoding="utf-8")
    if not metrics["passed"]:
        raise SystemExit("Image fidelity thresholds failed")


if __name__ == "__main__":
    main()
