"""Compare resolves with the actual Xenia render target at the preceding draw.

Uses shader hashes and draw counts to join a matching RDC and replay-index.json.
Unlike textures.bin, an event export preserves the exact intermediate version.
"""
import argparse
import hashlib
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from compare_replay_passes import native_pixels


def dds_pixels(path):
    data = path.read_bytes()
    if len(data) < 128 or data[:4] != b"DDS ":
        raise ValueError("Expected an uncompressed DDS: %s" % path)
    height, width = struct.unpack_from("<2I", data, 12)
    fourcc = data[84:88]
    offset = 148 if fourcc == b"DX10" else 128
    if fourcc == b"DX10":
        fmt = struct.unpack_from("<I", data, 128)[0]
    elif struct.unpack_from("<I", data, 80)[0] & 0x40 and struct.unpack_from("<I", data, 88)[0] == 32:
        # RenderDoc writes legacy headers for RGBA8/BGRA8. Honour the masks,
        # rather than assuming the byte order of the D3D render target.
        packed = np.frombuffer(data, "<u4", width * height, 128).reshape(height, width)
        channels = []
        for channel, mask in enumerate(struct.unpack_from("<4I", data, 92)):
            shift = (mask & -mask).bit_length() - 1 if mask else 0
            channels.append(((packed & mask) >> shift).astype(np.float32) / (mask >> shift)
                            if mask else np.full((height, width), float(channel == 3), np.float32))
        return np.stack(channels, axis=2), 28
    else:
        # Legacy D3DFMT floating-point FourCC values.
        fmt = {111: 54, 112: 34, 113: 10, 114: 41, 115: 16, 116: 2}.get(struct.unpack("<I", fourcc)[0])
    dtype, channels, divisor = {
        2: ("<f4", 4, 1), 10: ("<f2", 4, 1), 13: ("<i2", 4, 32767),
        16: ("<f4", 2, 1), 28: ("u1", 4, 255), 34: ("<f2", 2, 1),
        37: ("<i2", 2, 32767), 41: ("<f4", 1, 1), 54: ("<f2", 1, 1), 87: ("u1", 4, 255),
    }[fmt]
    pixels = np.frombuffer(data, dtype, width * height * channels, offset).reshape(height, width, channels).astype(np.float32)
    pixels /= divisor
    if fmt in (13, 37):
        pixels = np.maximum(pixels, -1)  # SNORM has two representations of -1.
    if fmt == 87:
        pixels = pixels[:, :, [2, 1, 0, 3]]
    return pixels, fmt


def game_draws(text):
    pattern = re.compile(r"event=(\d+) indices=(\d+).*? rt=(\d+).*? name=VS ([0-9A-F]{16})(?:, PS ([0-9A-F]{16}))?$")
    return [dict(event=int(m[1]), indices=int(m[2]), rt=int(m[3]), vs=m[4], ps=m[5])
            for line in text.splitlines() if (m := pattern.fullmatch(line))]


def align_draws(records, events, hashes, allow_prefix=False):
    draws = [r for r in records if r["kind"] == "draw"]
    expected = []
    for draw in draws:
        digest = hashes.get((draw["ps"], "ps")) if draw["ps"] else None
        if draw["ps"] and digest is None:
            raise ValueError("No microcode hash for PS %s" % draw["ps"])
        expected.append((digest, draw["indices"]))
    # XDK clears also submit shaders and draws; they are absent from the packed
    # draw list. Require one unique ordered subsequence, rather than greedily
    # picking the first matching shader and risking the wrong intermediate.
    actual = [(e["ps"], e["indices"]) for e in events]
    if allow_prefix:
        j, length = 0, 0
        for signature in expected:
            found = next((k for k in range(j, len(actual)) if actual[k] == signature), None)
            if found is None:
                break
            j, length = found + 1, length + 1
        draws, expected = draws[:length], expected[:length]
        if not draws:
            raise ValueError("No matching prefix in the RDC")
    ways = [[0] * (len(events) + 1) for _ in range(len(draws) + 1)]
    ways[-1] = [1] * (len(events) + 1)
    for i in range(len(draws) - 1, -1, -1):
        for j in range(len(events) - 1, -1, -1):
            ways[i][j] = min(2, ways[i][j + 1] + (ways[i + 1][j + 1] if expected[i] == actual[j] else 0))
    if ways[0][0] != 1:
        raise ValueError("Capture alignment rejected: %s ordered match (%d packed draws, %d RDC draws)" %
                         ("ambiguous" if ways[0][0] else "no complete", len(draws), len(events)))
    aligned, j = {}, 0
    for i, draw in enumerate(draws):
        while expected[i] != actual[j] or not ways[i + 1][j + 1]:
            j += 1
        aligned[draw["record"]] = events[j]
        j += 1
    return aligned


def compare(native, reference, fmt):
    # EDRAM host textures have tile-padded width (e.g. 560 for a 512 view).
    height, width, channels = native.shape
    if reference.shape[0] < height or reference.shape[1] < width or reference.shape[2] < channels:
        raise ValueError("Reference is smaller than the copied rectangle: %s vs %s" % (reference.shape, native.shape))
    reference = reference[:height, :width, :channels]
    if fmt in (13, 37):
        native = native / 32  # Xenos fixed16 RT range is [-32, 32].
    error = np.abs(native - reference)
    if not np.isfinite(error).all():
        raise ValueError("Non-finite reference or native pixels")
    return dict(mae=float(error.mean()), maximum=float(error.max()),
                per_channel=error.mean((0, 1)).tolist(), width=width, height=height)


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--capture-dir", type=Path, required=True)
    parser.add_argument("--replay-dir", type=Path, required=True)
    parser.add_argument("--inspector", type=Path, default=root / "build/renderdoc-sdk-v1.47/replay_inspect.exe")
    parser.add_argument("--threshold", type=float, default=0.01)
    parser.add_argument("--refresh", action="store_true")
    parser.add_argument("--allow-prefix", action="store_true", help="compare only the uniquely aligned prefix if later GPU draws are absent")
    args = parser.parse_args()
    index = json.loads((args.replay_dir / "replay-index.json").read_text())
    digest = hashlib.sha256((args.capture_dir / "frame_capture.bin").read_bytes()).hexdigest()
    if digest != index["capture_sha256"]:
        raise ValueError("replay-index.json belongs to a different CPU capture; repack first")
    output = args.replay_dir / "rdoc-reference"
    output.mkdir(exist_ok=True)
    capture = args.capture.resolve()
    cache_key = dict(path=str(capture), size=capture.stat().st_size, mtime=capture.stat().st_mtime_ns, index_version=1)
    cache_file = output / "capture.json"
    refresh = args.refresh or not cache_file.exists() or json.loads(cache_file.read_text()) != cache_key
    states = output / "draw-states.txt"
    if refresh or not states.exists():
        with states.open("w") as stdout, (output / "inspect.log").open("w") as stderr:
            subprocess.run([str(args.inspector), str(capture), "--draw-index"], stdout=stdout, stderr=stderr, check=True)
    hash_text = subprocess.check_output([str(args.inspector), "--shader-hashes", str(root / "build/shaders_split")], text=True)
    hashes = {(name, stage): digest for name, stage, digest in (line.split() for line in hash_text.splitlines())}
    aligned = align_draws(index["records"], game_draws(states.read_text()), hashes, args.allow_prefix)
    checkpoints, exports, previous, excluded, order = [], [], {}, [], 0
    cleared, unverified = set(), []
    for record in index["records"]:
        key = (record["surface"], record["color"])
        if record["kind"] == "draw":
            if record["record"] not in aligned:
                break  # No comparisons beyond an unverified draw.
            previous[key] = aligned[record["record"]]
            cleared.discard(key)
        elif record["kind"] == "clear" and record["flags"] & 15:
            previous.pop(key, None)
            cleared.add(key)
        elif record["kind"] == "resolve":
            if record.get("source", 0) == 4:
                continue
            raw = args.replay_dir / ("resolve_%02d_%s.raw" % (order, record["destination"]))
            order += 1
            last_draw = previous.get(key)
            if last_draw is None or not last_draw["rt"]:
                excluded.append(record["record"])
                if key not in cleared:
                    unverified.append(record["record"])
                continue
            if not raw.exists():
                raise ValueError("Missing native resolve readback; run fd_replay: %s" % raw)
            if raw.stat().st_mtime_ns < (args.replay_dir / "replay.bin").stat().st_mtime_ns:
                raise ValueError("Stale native readback; run fd_replay after packing: %s" % raw)
            dds = output / ("event_%06d_rt%d.dds" % (last_draw["event"], record["source"]))
            if refresh or not dds.exists():
                exports.append((last_draw["event"], record["source"], dds.resolve()))
            checkpoints.append((record, last_draw, raw, dds))
    if exports:
        listing = output / "exports.tsv"
        listing.write_text("".join("%d\t%d\t%s\n" % (event, slot, path) for event, slot, path in sorted(set(exports))), encoding="utf-8")
        with (output / "exports.log").open("w") as log:
            subprocess.run([str(args.inspector), str(capture), "--export-list", str(listing.resolve())], stdout=log, stderr=subprocess.STDOUT, check=True)
    cache_file.write_text(json.dumps(cache_key), encoding="utf-8")
    rows = []
    for record, event, raw, dds in checkpoints:
        reference, fmt = dds_pixels(dds)
        row = dict(record=record["record"], cpu_record=record["cpu_record"], event=event["event"],
                   destination=record["destination"], native=str(raw), reference=str(dds), dxgi_format=fmt)
        native = native_pixels(raw)
        # A partial atlas readback contains the whole destination. Only the
        # rectangle just updated has a reference in this source render target.
        x, y = record["x"], record["y"]
        if x or y or native.shape[0] > reference.shape[0] or native.shape[1] > reference.shape[1]:
            native = native[y:y + reference.shape[0], x:x + reference.shape[1]]
        row.update(compare(native, reference, fmt))
        row["exceeds_threshold"] = row["mae"] > args.threshold
        rows.append(row)
        print("record %4d event %6d %s MAE %.6f max %.6f%s" %
              (row["record"], row["event"], row["destination"], row["mae"], row["maximum"], " <--" if row["exceeds_threshold"] else ""))
    if not rows:
        raise ValueError("No exact-event checkpoints were compared; run the native replay first")
    unmatched = [r["record"] for r in index["records"] if r["kind"] == "draw" and r["record"] not in aligned]
    report = dict(capture=cache_key, threshold=args.threshold, matched_draws=len(aligned), unmatched_draws=unmatched,
                  compared_resolves=len(rows), excluded_records=excluded,
                  unverified_resolves=unverified,
                  first_difference=next((r for r in rows if r["exceeds_threshold"]), None), passes=rows)
    (args.replay_dir / "validation-rdoc.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("%d draws aligned; %d exact-event checkpoints compared" % (len(aligned), len(rows)))
    if unverified:
        print("Unverified colour resolves: %s" % unverified)
    if report["first_difference"]:
        print("First checkpoint above threshold: record %d" % report["first_difference"]["record"])


if __name__ == "__main__":
    main()
