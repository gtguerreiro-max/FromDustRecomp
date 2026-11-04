"""Compare native resolve readbacks in frame order, preserving signed height data.

python tools/compare_replay_passes.py --capture-dir build --replay-dir build/gameplay-final-replay
These are heuristic memory snapshots, not exact-event references. Even the last
copy may differ from a snapshot taken at first use. Use rdoc/compare_passes.py
with a matching RDC to establish the first genuinely divergent checkpoint.
"""
import argparse
import json
import math
import re
import struct
from pathlib import Path

import numpy as np

import replay_pack as rp
from xenos_fetch import decode_texture_fetch
from xenos_texture import decode_texture


def native_pixels(path):
    data = path.read_bytes()
    width, height, fmt, bpp = struct.unpack_from("<4I", data)
    dtype, channels = {2: ("<f4", 4), 10: ("<f2", 4), 16: ("<f4", 2),
                       13: ("<i2", 4), 20: ("u1", 4), 26: ("<f2", 2),
                       29: ("<i2", 2), 34: ("<f4", 1)}[fmt]
    pixels = np.frombuffer(data, dtype, width * height * channels, 16).reshape(height, width, channels).astype(np.float32)
    if fmt == 20:
        pixels /= 255
    elif fmt in (13, 29):
        pixels = np.maximum(pixels / 32767, -1) * 32
    return pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture-dir", type=Path, default=Path("build"))
    parser.add_argument("--replay-dir", type=Path, default=Path("build/replay"))
    parser.add_argument("--json", type=Path)
    parser.add_argument("--gpu-textures", action="store_true")
    args = parser.parse_args()
    textures = {base & 0x1FFFFFFF: data for base, data in
                rp.load_textures(args.capture_dir / ("gpu_textures.bin" if args.gpu_textures else "textures.bin")).items()}
    copies = []
    gpu_states = rp.load_xenia_registers(args.capture_dir / "xenia_regs.bin")
    for record, (header, _, objects) in enumerate(rp.load(args.capture_dir / "frame_capture.bin", True)):
        if header[1] != 4 or "resolve" not in objects:
            continue
        flags, texture, obj = objects["resolve"]
        if not texture or flags & 7 == 4:
            continue
        fetch = struct.unpack_from(">6I", obj, 0x1C)
        info, x, y, mip, _ = rp.copy_destination_offset(objects.get("commands", b"")[4:], fetch, gpu_states)
        copies.append({"record": record, "destination": fetch[1] & 0x1FFFF000,
                       "copy_info": info, "mip": mip, "x": x, "y": y})
    last = {c["destination"]: i for i, c in enumerate(copies) if c["mip"] == 0}
    rows = []
    for path in sorted(args.replay_dir.glob("resolve_*.raw")):
        order, address = re.fullmatch(r"resolve_(\d+)_([0-9A-F]+)\.raw", path.name).groups()
        order, address = int(order), int(address, 16)
        if order >= len(copies) or copies[order]["destination"] != address:
            continue  # old files left by a different capture
        row = dict(copies[order], order=order, address="%08X" % address)
        if row["mip"] or last[address] != order or address not in textures:
            continue
        fetch, raw = textures[address]
        info = decode_texture_fetch(fetch)
        a = native_pixels(path)
        bias = ((row["copy_info"] >> 16 & 63) ^ 32) - 32
        if info["format_name"] in ("k_16_16", "k_16_16_16_16"):
            width, height, data = rp.decode_fixed16(raw, fetch)
            b = np.frombuffer(data, "<f4").reshape(height, width, 4)[:, :, :a.shape[2]]
            exp = ((fetch[3] >> 13 & 63) ^ 32) - 32
            a *= math.ldexp(1.0, bias + exp)
            metric = "signed float MAE"
        else:
            width, height, data = decode_texture(raw, info)
            b = np.frombuffer(data, "u1").reshape(height, width, 4).astype(np.float32) / 255
            if a.shape[2] < 4:
                a = np.concatenate([a, np.zeros((*a.shape[:2], 4 - a.shape[2]), np.float32)], 2)
                a[:, :, 3] = 1
            a = np.clip(a * math.ldexp(1.0, bias), 0, 1)
            if row["copy_info"] & (1 << 24):
                a = a[:, :, [2, 1, 0, 3]]
            metric = "UNORM MAE"
        if a.shape != b.shape:
            row["mismatch"] = "dimensions %s vs %s" % (a.shape, b.shape)
        else:
            error = np.abs(a - b)
            row.update(metric=metric, mae=float(error.mean()), maximum=float(error.max()),
                       per_channel=error.mean((0, 1)).tolist(),
                       native_mean=a.mean((0, 1)).tolist(), reference_mean=b.mean((0, 1)).tolist())
        rows.append(row)
        print("%02d record %-4d %08X %-22s %s" %
              (order, row["record"], address, info["format_name"],
               row.get("mismatch", "%s %.6f" % (metric, row.get("mae", 0)))))
    if args.json:
        args.json.write_text(json.dumps(rows, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
