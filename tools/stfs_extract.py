"""Extract files from an Xbox 360 STFS package (CON/LIVE/PIRS).

Usage: python stfs_extract.py <package> <out_dir> [--list]
"""
import os
import struct
import sys

BLOCK = 0x1000
PER_LEVEL = 170


def u24le(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)


def u24be(b, o):
    return (b[o] << 16) | (b[o + 1] << 8) | b[o + 2]


class Stfs:
    def __init__(self, path):
        self.f = open(path, "rb")
        hdr = self.f.read(0x971A)
        self.magic = hdr[:4]
        if self.magic not in (b"CON ", b"LIVE", b"PIRS"):
            raise ValueError("not an STFS package: %r" % self.magic)
        self.header_size = struct.unpack(">I", hdr[0x340:0x344])[0]
        self.content_type = struct.unpack(">I", hdr[0x344:0x348])[0]
        self.title_id = struct.unpack(">I", hdr[0x360:0x364])[0]
        if struct.unpack(">I", hdr[0x3A9:0x3AD])[0] != 0:
            raise ValueError("SVOD package, not STFS")
        vd = hdr[0x379:0x379 + 0x24]
        flags = vd[2]
        self.read_only = flags & 1
        self.bpht = 1 if self.read_only else 2
        self.ft_blocks = struct.unpack("<H", vd[3:5])[0]
        self.ft_start = u24le(vd, 5)
        self.data_base = (self.header_size + BLOCK - 1) & ~(BLOCK - 1)
        self.display_name = hdr[0x411:0x411 + 0x80].decode("utf-16-be", "ignore").split("\0")[0]

    def backing_block(self, idx):
        base = PER_LEVEL
        block = idx
        for _ in range(3):
            block += ((idx + base) // base) * self.bpht
            if idx < base:
                break
            base *= PER_LEVEL
        return block

    def hash_block(self, idx):
        if idx < PER_LEVEL:
            return 0
        block = (idx // PER_LEVEL) * (PER_LEVEL + self.bpht)
        block += ((idx // (PER_LEVEL * PER_LEVEL)) + 1) * self.bpht
        if idx < PER_LEVEL * PER_LEVEL:
            return block
        return block + self.bpht

    def read_block(self, idx):
        self.f.seek(self.data_base + self.backing_block(idx) * BLOCK)
        return self.f.read(BLOCK)

    def next_block(self, idx):
        # Only the primary hash table is consulted; fine for read-only packages.
        self.f.seek(self.data_base + self.hash_block(idx) * BLOCK + (idx % PER_LEVEL) * 0x18)
        e = self.f.read(0x18)
        return u24be(e, 0x15)

    def chain(self, start, count, contiguous):
        idx = start
        for _ in range(count):
            yield idx
            idx = idx + 1 if contiguous else self.next_block(idx)

    def entries(self):
        raw = b"".join(self.read_block(b) for b in self.chain(self.ft_start, self.ft_blocks, False))
        out = []
        for i in range(0, len(raw), 0x40):
            e = raw[i:i + 0x40]
            nlen = e[0x28] & 0x3F
            if nlen == 0:
                break
            out.append({
                "name": e[:nlen].decode("ascii", "replace"),
                "dir": bool(e[0x28] & 0x80),
                "contig": bool(e[0x28] & 0x40),
                "blocks": u24le(e, 0x29),
                "start": u24le(e, 0x2F),
                "parent": struct.unpack(">h", e[0x32:0x34])[0],
                "size": struct.unpack(">I", e[0x34:0x38])[0],
            })
        return out

    def path_of(self, entries, i):
        parts = []
        while i != -1:
            parts.append(entries[i]["name"])
            i = entries[i]["parent"]
        return "/".join(reversed(parts))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    pkg = Stfs(sys.argv[1])
    out_dir = sys.argv[2]
    print("%s  title=%08X  type=%06X  read_only=%d  '%s'" % (
        pkg.magic.decode(), pkg.title_id, pkg.content_type, pkg.read_only, pkg.display_name))
    entries = pkg.entries()
    for i, e in enumerate(entries):
        path = pkg.path_of(entries, i)
        if e["dir"]:
            print("  [dir] %s" % path)
            if "--list" not in sys.argv:
                os.makedirs(os.path.join(out_dir, path), exist_ok=True)
            continue
        print("  %12d  %s%s" % (e["size"], path, "" if e["contig"] else "  (chained)"))
        if "--list" in sys.argv:
            continue
        dst = os.path.join(out_dir, path)
        os.makedirs(os.path.dirname(dst) or out_dir, exist_ok=True)
        remaining = e["size"]
        with open(dst, "wb") as o:
            for b in pkg.chain(e["start"], e["blocks"], e["contig"]):
                data = pkg.read_block(b)
                o.write(data[:min(BLOCK, remaining)])
                remaining -= BLOCK
                if remaining <= 0:
                    break
    return 0


if __name__ == "__main__":
    sys.exit(main())
