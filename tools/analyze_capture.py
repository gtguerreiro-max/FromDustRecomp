"""Summarize build/frame_capture.bin (runtime/gpu/frame_capture.cpp): which
dwords of the D3D device object change between the draws of one frame.

Usage: python tools/analyze_capture.py [capture] [--ranges] [--offset HEX...]
"""
import struct, sys, collections

KINDS = {0: "indexed", 1: "rect", 2: "rect'", 3: "swap", 4: "resolve", 5: "clear"}


OBJECT_COUNT, OBJECT_SIZE, CODE_SIZE = 26, 0x80, 0x400  # frame_capture.cpp constants


def load(path, with_objects=False):
    """Records as (header, device snapshot[, {device offset: (pointer, bytes)}])."""
    data = open(path, "rb").read()
    records, i = [], 0
    while i < len(data):
        # BeginVertices data completed after the call (frame_capture.cpp FlushPendingVertices).
        if data[i:i + 4] == b"FDUP":
            address, length = struct.unpack_from("<2I", data, i + 4)
            if records and with_objects:
                records[-1][2]["up"] = (address, data[i + 12:i + 12 + length])
            i += 12 + length
            continue
        header = struct.unpack_from("<12I", data, i)
        size = header[11]
        snapshot = data[i + 48:i + 48 + size]
        i += 48 + size
        objects = {}
        if header[1] != 3:
            for _ in range(OBJECT_COUNT):
                offset, pointer = struct.unpack_from("<2I", data, i)
                objects[offset] = (pointer, data[i + 8:i + 8 + OBJECT_SIZE])
                i += 8 + OBJECT_SIZE
            # Then the pixel and vertex shader microcode heads (+0x328C is the pixel shader).
            for key in ("ps_code", "vs_code"):
                _, pointer = struct.unpack_from("<2I", data, i)
                objects[key] = (pointer, data[i + 8:i + 8 + CODE_SIZE])
                i += 8 + CODE_SIZE
            # Then the command bytes the draw wrote (frame_capture.cpp RecordCommands).
            if data[i:i + 4] == b"BCDF":
                length = struct.unpack_from("<I", data, i + 4)[0]
                objects["commands"] = data[i + 8:i + 8 + length]
                i += 8 + length
            # Resolves: flags, destination texture object and its first 0x40 bytes.
            if data[i:i + 4] == b"SRDF":
                flags, texture = struct.unpack_from("<2I", data, i + 4)
                objects["resolve"] = (flags, texture, data[i + 12:i + 12 + 0x40])
                i += 12 + 0x40
            if data[i:i + 4] == b"FDCL":
                depth, count = struct.unpack_from("<fI", data, i + 4)
                rectangles = [struct.unpack_from(">4i", data, i + 12 + r * 16) for r in range(count)]
                objects["clear"] = (depth, rectangles)
                i += 12 + count * 16
        records.append((header, snapshot, objects) if with_objects else (header, snapshot))
    return records


def main():
    args = sys.argv[1:]
    path = args[0] if args and not args[0].startswith("--") else "build/frame_capture.bin"
    records = [r for r in load(path) if r[0][1] != 3]
    kinds = collections.Counter(KINDS[h[1]] for h, _ in records)
    print("draws:", len(records), dict(kinds))
    size = len(records[0][1])
    words = size // 4
    distinct = []
    for w in range(words):
        values = {struct.unpack_from(">I", snap, w * 4)[0] for _, snap in records}
        distinct.append(len(values))
    if "--offset" in args:
        for off in args[args.index("--offset") + 1:]:
            o = int(off, 16) // 4
            seq = [struct.unpack_from(">I", snap, o * 4)[0] for _, snap in records]
            print("+%04X distinct=%d first: %s" % (o * 4, distinct[o], " ".join("%08X" % v for v in seq[:12])))
        return
    # Ranges of consecutive changing dwords.
    ranges, start = [], None
    for w in range(words + 1):
        changing = w < words and distinct[w] > 1
        if changing and start is None:
            start = w
        elif not changing and start is not None:
            if w - start >= 1:
                ranges.append((start, w))
            start = None
    print("changing dwords: %d of %d, in %d ranges" % (sum(d > 1 for d in distinct), words, len(ranges)))
    for s, e in ranges:
        peak = max(distinct[s:e])
        print("  +%04X..+%04X  %4d dwords  max distinct %d" % (s * 4, e * 4, e - s, peak))


if __name__ == "__main__":
    main()
