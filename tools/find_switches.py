"""Detect PPC jump tables by backward-slicing from every `mtctr rX; bctr`.

Unlike XenonAnalyse's fixed patterns, this tolerates the instruction
reordering and nops emitted by the compiler used for From Dust.

Usage: python find_switches.py <image.bin> <load_address_hex> <out.toml>
       (image.bin is produced by xex_dump.py)
"""
import struct
import sys

from capstone import CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN, Cs

from xex_dump import sections

MAX_BACK = 48
MAX_LABELS = 4096

md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
md.detail = True


class Image:
    def __init__(self, data, base):
        self.data = data
        self.base = base
        self.code = [(base + va, base + va + vs) for name, va, vs, ch in sections(data) if ch & 0x20]
        self.cache = {}

    def word(self, addr):
        o = addr - self.base
        return struct.unpack(">I", self.data[o:o + 4])[0]

    def insn(self, addr):
        if addr not in self.cache:
            o = addr - self.base
            self.cache[addr] = next(md.disasm(self.data[o:o + 4], addr), None)
        return self.cache[addr]

    def in_code(self, addr):
        return any(s <= addr < e for s, e in self.code)


NO_DEST_PREFIXES = ("st", "cmp", "b", "mt", "dcb", "icb", "tw", "td", "sync", "isync", "eieio", "lwsync", "nop")


def ops(ins):
    return [o.strip() for o in ins.op_str.split(",")]


def writes(ins, reg_name):
    """Capstone has no regs_access() for PPC, so infer the GPR destination."""
    m = ins.mnemonic.rstrip(".")
    o = ops(ins)
    # Update forms (lwzu, stwux, ...) also write back the base register.
    if (m.startswith("l") or m.startswith("st")) and (m.endswith("u") or m.endswith("ux")) and len(o) >= 2:
        base = o[1].split("(")[-1].rstrip(")")
        if base == reg_name:
            return True
    if m.startswith(NO_DEST_PREFIXES):
        return False
    return bool(o) and o[0] == reg_name


def is_barrier(ins):
    # Unconditional control flow means we left the straight-line block.
    return ins.mnemonic in ("b", "blr", "bctr", "bl", "bctrl", "rfi")


def find_def(img, reg, pos):
    addr = pos - 4
    for _ in range(MAX_BACK):
        ins = img.insn(addr)
        if ins is not None:
            if is_barrier(ins):
                return None
            if writes(ins, reg):
                return ins
        addr -= 4
    return None


def const_of(img, reg, pos, depth=0):
    if depth > 4:
        return None
    d = find_def(img, reg, pos)
    if d is None:
        return None
    o = ops(d)
    if d.mnemonic == "lis":
        return (int(o[1], 0) << 16) & 0xFFFFFFFF
    if d.mnemonic in ("addi", "addic") and len(o) == 3:
        if o[1] == "0":
            return int(o[2], 0) & 0xFFFFFFFF
        b = const_of(img, o[1], d.address, depth + 1)
        return None if b is None else (b + int(o[2], 0)) & 0xFFFFFFFF
    if d.mnemonic == "ori" and len(o) == 3:
        b = const_of(img, o[1], d.address, depth + 1)
        return None if b is None else b | int(o[2], 0)
    if d.mnemonic == "li":
        return int(o[1], 0) & 0xFFFFFFFF
    return None


def shift_source(img, reg, pos):
    """If `reg` = `src << n` (slwi/rlwinm), return (src, n, def); else (reg, 0, None)."""
    d = find_def(img, reg, pos)
    if d is None:
        return reg, 0, None
    o = ops(d)
    if d.mnemonic == "slwi":
        return o[1], int(o[2], 0), d
    if d.mnemonic == "rlwinm" and len(o) == 5:
        sh, mb, me = int(o[2], 0), int(o[3], 0), int(o[4], 0)
        if me == 31 - sh and mb == 0:
            return o[1], sh, d
    return reg, 0, None


def find_bound(img, idx_reg, pos, bctr):
    """Find `cmplwi crN, idx, max` + `bgt crN, default` dominating the dispatch."""
    addr = pos - 4
    cr = None
    default = 0
    aliases = {idx_reg}
    a = pos - 4
    for _ in range(MAX_BACK):
        ins = img.insn(a)
        if ins is not None:
            if is_barrier(ins):
                break
            if ins.mnemonic == "mr" and ops(ins)[0] in aliases:
                aliases.add(ops(ins)[1])
        a -= 4
    for _ in range(MAX_BACK):
        ins = img.insn(addr)
        if ins is not None:
            o = ops(ins)
            # `mr idx, rX` before the dispatch: the bound may be checked on rX.
            if ins.mnemonic == "mr" and o[0] in aliases:
                aliases.add(o[1])
                addr -= 4
                continue
            if cr is None and ins.mnemonic in ("bgt", "ble", "bgtlr", "blelr"):
                cr = o[0] if o and o[0].startswith("cr") else "cr0"
                if ins.mnemonic in ("bgt", "ble") and o:
                    try:
                        default = int(o[-1], 0)
                    except ValueError:
                        default = 0
            elif ins.mnemonic == "cmplwi":
                c, r, n = (o if len(o) == 3 else ["cr0"] + o)
                if r in aliases and (cr is None or c == cr):
                    # The index register must survive until the bctr
                    # (apart from the alias copy itself).
                    a = ins.address + 4
                    while a < bctr:
                        i2 = img.insn(a)
                        if i2 is not None and writes(i2, idx_reg) and not (
                                i2.mnemonic == "mr" and ops(i2)[1] in aliases):
                            return None
                        a += 4
                    return int(n, 0) + 1, default
            elif ins.mnemonic in ("b", "blr", "bctr"):
                return None
        addr -= 4
    return None


def analyze(img, mtctr, bctr):
    ins = img.insn(mtctr)
    target = ops(ins)[0]
    d = find_def(img, target, mtctr)
    if d is None:
        return None
    o = ops(d)
    if d.mnemonic == "lwzx":
        table = const_of(img, o[1], d.address)
        idx, sh, _ = shift_source(img, o[2], d.address)
        if table is None or sh != 2:
            return None
        kind = "absolute"
        load_pos = d.address
    elif d.mnemonic == "add":
        a, b = o[1], o[2]
        base = const_of(img, a, d.address)
        other = b
        if base is None:
            base, other = const_of(img, b, d.address), a
        if base is None:
            return None
        # Optional shift of the loaded offset (computed switch).
        src, out_shift, sdef = shift_source(img, other, d.address)
        ld = find_def(img, src, sdef.address if sdef else d.address)
        if ld is None or ld.mnemonic not in ("lbzx", "lhzx"):
            return None
        lo = ops(ld)
        table = const_of(img, lo[1], ld.address)
        idx, sh, _ = shift_source(img, lo[2], ld.address)
        if table is None:
            return None
        elem = 1 if ld.mnemonic == "lbzx" else 2
        if (elem == 1 and sh != 0) or (elem == 2 and sh != 1):
            return None
        kind = ("byte" if elem == 1 else "short") + ("_shifted" if out_shift else "")
        load_pos = ld.address
    else:
        return None

    bound = find_bound(img, idx, load_pos, bctr)
    if bound is None and kind == "absolute" and table == bctr + 4:
        # Table stored inline right after the bctr with no visible bound
        # check: take entries while they look like nearby code addresses.
        count = 0
        while count < MAX_LABELS:
            lab = img.word(table + count * 4)
            if not img.in_code(lab) or lab & 3 or abs(lab - bctr) > 0x10000:
                break
            count += 1
        if count == 0:
            return None
        bound = (count, 0)
        kind = "absolute_inline"
    if bound is None and not kind.startswith("absolute"):
        # Offset table with the bound checked by the caller: entries run
        # until the zero padding that aligns the next table.
        elem = 1 if kind.startswith("byte") else 2
        o = table - img.base
        count = 1
        while count < MAX_LABELS:
            run = img.data[o + count * elem:o + count * elem + 4]
            if run == bytes(4):
                break
            count += 1
        bound = (count, 0)
        kind += "_unbounded"
    if bound is None:
        return None
    count, default = bound
    if count > MAX_LABELS:
        return None

    labels = []
    for i in range(count):
        if kind.startswith("absolute"):
            lab = img.word(table + i * 4)
        elif kind.startswith("byte"):
            lab = base + (img.data[table - img.base + i] << out_shift)
        else:
            lab = base + (struct.unpack(">H", img.data[table - img.base + i * 2:table - img.base + i * 2 + 2])[0] << out_shift)
        if not img.in_code(lab) or lab & 3:
            return None
        labels.append(lab)
    return {"base": bctr, "r": int(idx[1:]), "default": default, "labels": labels, "kind": kind}


def pdata_functions(img):
    for name, va, vs, ch in sections(img.data):
        if name == ".pdata":
            out = []
            for o in range(va, va + vs, 8):
                fn, info = struct.unpack(">II", img.data[o:o + 8])
                if fn:
                    out.append((fn, fn + ((info >> 8) & 0x3FFFFF) * 4))
            return sorted(out)
    return []


TERMINATORS = ("blr", "b", "bctr")


def branch_target(ins):
    o = ops(ins)
    if not ins.mnemonic.startswith("b") or ins.mnemonic in ("bl", "blr", "bctr", "bctrl") or not o:
        return None
    try:
        return int(o[-1], 0)
    except ValueError:
        return None


def block_start(img, addr, lo):
    """First instruction after the last terminator before `addr` (>= lo)."""
    a = addr - 4
    while a >= lo:
        ins = img.insn(a)
        if ins is not None and ins.mnemonic in TERMINATORS:
            break
        a -= 4
    a += 4
    while a < addr and img.word(a) == 0:
        a += 4
    return a


def block_end(img, addr, hi):
    """Address just past the first terminator at or after `addr`."""
    a = addr
    while a < hi:
        ins = img.insn(a)
        if ins is not None and ins.mnemonic in TERMINATORS:
            return a + 4
        a += 4
    return hi


def grow_region(img, lo, hi, start, end, switches, pending=()):
    """Grow [start, end) until every local branch/case target (forward or
    backward) inside the .pdata gap [lo, hi) is covered."""
    pending = set(pending)
    while True:
        for a in range(start, end, 4):
            ins = img.insn(a)
            if ins is not None:
                tgt = branch_target(ins)
                if tgt is not None and lo <= tgt < hi:
                    pending.add(tgt)
                if a in switches:
                    pending.update(l for l in switches[a] if lo <= l < hi)
        grow = [p for p in pending if not (start <= p < end)]
        if not grow and start > lo:
            # A preceding block that branches into us belongs to the same function.
            prev = block_start(img, start - 4, lo)
            for a in range(prev, start, 4):
                ins = img.insn(a)
                tgt = branch_target(ins) if ins is not None else None
                if tgt is not None and start <= tgt < end:
                    grow = [prev]
                    break
        if not grow:
            break
        for p in grow:
            if p < start:
                start = block_start(img, p, lo)
            else:
                end = max(end, block_end(img, p, hi))
        pending.clear()
    return start, end


def is_import_thunk(img, a):
    """XEX import thunk: two import records, then `mtctr r11; bctr`."""
    return (img.word(a) >> 24) == 1 and img.word(a + 8) == 0x7D6903A6 and img.word(a + 12) == 0x4E800420


def gap_regions(img, pdata, switches, skip):
    """Bounds for every frameless (non-.pdata) function.

    XenonRecomp's own Function::Analyze truncates functions whose
    fall-through blocks it mispredicts (e.g. bdzf chains), so we hand it
    explicit bounds for all of them.
    """
    out, seeds = [], []
    for cs, ce in img.code:
        starts = [cs] + [e for s, e in pdata if cs <= e < ce]
        ends = [s for s, e in pdata if cs <= s < ce] + [ce]
        gaps = []
        for lo, hi in zip(sorted(starts), sorted(ends)):
            # Import thunks split a gap: growth must never cross them (a tail
            # call over the thunk table would otherwise swallow it).
            sub = lo
            for t in range(lo, hi - 12, 4):
                if is_import_thunk(img, t):
                    if t > sub:
                        gaps.append((sub, t))
                    sub = t + 16
            if sub < hi:
                gaps.append((sub, hi))
        for lo, hi in gaps:
            a = lo
            while a < hi:
                if img.word(a) == 0 or any(s <= a < e for s, e in skip):
                    a += 4
                    continue
                if is_import_thunk(img, a):
                    # Must stay a symbol of its own (recompiled as __imp__Name).
                    a += 16
                    continue
                s, e = grow_region(img, lo, hi, a, block_end(img, a, hi), switches)
                out.append((s, e))
                seeds.append(a)
                a = max(e, a + 4)
    return out, seeds


def merge_regions(regions):
    out = []
    for s, e in sorted(regions):
        if out and s < out[-1][1]:
            out[-1] = (out[-1][0], max(out[-1][1], e))
        else:
            out.append((s, e))
    return [(s, e - s) for s, e in out]


def main():
    data = open(sys.argv[1], "rb").read()
    img = Image(data, int(sys.argv[2], 16))
    found, missed = [], []
    for start, end in img.code:
        for a in range(start, end - 4, 4):
            w = img.word(a)
            if (w & 0xFC1FFFFF) != 0x7C0903A6 or img.word(a + 4) != 0x4E800420:
                continue
            t = analyze(img, a, a + 4)
            (found if t else missed).append(t or a + 4)

    kinds = {}
    with open(sys.argv[3], "w") as f:
        f.write("# Generated by tools/find_switches.py\n")
        for t in found:
            kinds[t["kind"]] = kinds.get(t["kind"], 0) + 1
            f.write("[[switch]]\nbase = 0x%X\nr = %d\ndefault = 0x%X\nlabels = [\n" % (t["base"], t["r"], t["default"]))
            for lab in t["labels"]:
                f.write("    0x%X,\n" % lab)
            f.write("]\n\n")
    pdata = pdata_functions(img)
    switches = {t["base"]: t["labels"] for t in found}
    inline_tables = [(t["base"] + 4, t["base"] + 4 + 4 * len(t["labels"]))
                     for t in found if t["kind"] == "absolute_inline"]
    regions, seeds = gap_regions(img, pdata, switches, inline_tables)
    leaves = merge_regions(regions)
    # `bl` into the middle of a region is a second entry point; XenonRecomp
    # would otherwise analyze it on its own and cut it short at the bctr.
    inner = set()
    for cs, ce in img.code:
        for a in range(cs, ce, 4):
            w = img.word(a)
            if (w >> 26) == 18 and (w & 3) == 1:
                off = w & 0x03FFFFFC
                tgt = (a + (off - 0x04000000 if off & 0x02000000 else off)) & 0xFFFFFFFF
                for s, n in leaves:
                    if s < tgt < s + n:
                        inner.add((tgt, s + n - tgt))
    # Every discovered function start stays an entry point even when its code
    # got merged into a neighbour (e.g. a tail call back into it); such
    # functions may only be reachable through pointers.
    for seed in seeds:
        for s, n in leaves:
            if s < seed < s + n:
                inner.add((seed, s + n - seed))
    # Functions only reachable through pointers stored in data (vtables,
    # callbacks): every aligned data word pointing into code becomes an entry
    # point, running to the end of the function that contains it.
    starts = {s for s, _ in pdata} | {s for s, _ in leaves} | {s for s, _ in inner}
    containers = sorted(list(pdata) + [(s, s + n) for s, n in leaves])
    data_targets = 0
    for name, va, vs, ch in sections(img.data):
        if ch & 0x20 or name in (".pdata", ".reloc", ".idata"):
            continue
        for o in range(va, va + vs - 3, 4):
            t = struct.unpack(">I", img.data[o:o + 4])[0]
            if t in starts or t & 3 or not img.in_code(t) or img.word(t) == 0:
                continue
            for s, e in containers:
                if s < t < e:
                    inner.add((t, e - t))
                    starts.add(t)
                    data_targets += 1
                    break
    print("entry points referenced only from data: %d" % data_targets)
    leaves = sorted(set(leaves) | inner)
    with open(sys.argv[3].replace(".toml", "_functions.toml"), "w") as f:
        f.write("# Generated by tools/find_switches.py: bounds of all frameless (non-.pdata) functions\n")
        f.write("functions = [\n")
        for s, n in leaves:
            f.write("    { address = 0x%X, size = 0x%X },\n" % (s, n))
        f.write("]\n")
    print("jump tables found: %d %s" % (len(found), kinds))
    print("frameless functions with explicit bounds: %d" % len(leaves))
    print("unresolved mtctr/bctr sites: %d (virtual calls / function pointers are expected here)" % len(missed))
    # Sites whose preceding word is a nop/table-like are the suspicious ones.
    sus = [m for m in missed if (img.insn(m - 4) and ops(img.insn(m - 4))[0] in ("r0", "r12"))]
    print("suspicious unresolved (mtctr r0/r12): %d" % len(sus))
    for m in sus[:15]:
        print("   %08X" % m)


if __name__ == "__main__":
    main()
