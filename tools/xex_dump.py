"""Decrypt + decompress an XEX2 (basic compression) into its raw PE image,
then list PE sections and locate the ABI helper functions XenonRecomp needs.

Usage: python xex_dump.py <in.xex> <out.bin>
"""
import struct
import sys

from Crypto.Cipher import AES

RETAIL_KEY = bytes.fromhex("20B185A59D28FDC340583FBB0896BF91")
DEVKIT_KEY = bytes(16)

PATTERNS = {
    "restgprlr_14_address": bytes.fromhex("e9c1ff68"),
    "savegprlr_14_address": bytes.fromhex("f9c1ff68"),
    "restfpr_14_address": bytes.fromhex("c9ccff70"),
    "savefpr_14_address": bytes.fromhex("d9ccff70"),
    "restvmx_14_address": bytes.fromhex("3960fee07dcb60ce"),
    "savevmx_14_address": bytes.fromhex("3960fee07dcb61ce"),
    "restvmx_64_address": bytes.fromhex("3960fc00100b60cb"),
    "savevmx_64_address": bytes.fromhex("3960fc00100b61cb"),
}


def be32(b, o):
    return struct.unpack(">I", b[o:o + 4])[0]


def load(path):
    b = open(path, "rb").read()
    pe_off, sec_off, count = be32(b, 8), be32(b, 0x10), be32(b, 0x14)
    ffi = None
    for i in range(count):
        key, val = struct.unpack(">II", b[0x18 + i * 8:0x20 + i * 8])
        if key == 0x000003FF:
            ffi = val
    load_addr = be32(b, sec_off + 0x110)
    file_key = b[sec_off + 0x150:sec_off + 0x160]
    enc, comp = struct.unpack(">HH", b[ffi + 4:ffi + 8])
    data = b[pe_off:]
    if enc == 1:
        for key in (RETAIL_KEY, DEVKIT_KEY):
            session = AES.new(key, AES.MODE_ECB).decrypt(file_key)
            n = len(data) & ~0xF
            out = AES.new(session, AES.MODE_CBC, bytes(16)).decrypt(data[:n])
            if out[:2] == b"MZ" or comp != 0:
                break
        data = out
    if comp == 1:
        size = be32(b, ffi)
        image = bytearray()
        src = 0
        for o in range(ffi + 8, ffi + size, 8):
            dsz, zsz = struct.unpack(">II", b[o:o + 8])
            image += data[src:src + dsz]
            image += bytes(zsz)
            src += dsz
        data = bytes(image)
    elif comp != 0:
        raise ValueError("compression type %d not supported" % comp)
    if data[:2] != b"MZ":
        raise ValueError("decryption failed (no MZ header)")
    return load_addr, data


def sections(img):
    pe = struct.unpack("<I", img[0x3C:0x40])[0]
    nsec = struct.unpack("<H", img[pe + 6:pe + 8])[0]
    opt = struct.unpack("<H", img[pe + 0x14:pe + 0x16])[0]
    o = pe + 0x18 + opt
    out = []
    for _ in range(nsec):
        name = img[o:o + 8].split(b"\0")[0].decode()
        vsize, vaddr, rsize, raddr, _, _, _, _, chars = struct.unpack("<IIIIIIHHI", img[o + 8:o + 40])
        out.append((name, vaddr, vsize, chars))
        o += 40
    return out


def main():
    base, img = load(sys.argv[1])
    open(sys.argv[2], "wb").write(img)
    print("image %d bytes @ %08X" % (len(img), base))
    code = []
    for name, va, vs, ch in sections(img):
        print("  %-8s %08X-%08X %s" % (name, base + va, base + va + vs, "CODE" if ch & 0x20 else ""))
        if ch & 0x20:
            code.append((va, vs))
    for key, pat in PATTERNS.items():
        hits = []
        for va, vs in code:
            o = img.find(pat, va, va + vs)
            while o != -1 and o % 4 != 0:
                o = img.find(pat, o + 1, va + vs)
            if o != -1:
                hits.append(o)
        print("  %-22s %s" % (key, ", ".join("0x%08X" % (base + h) for h in hits) or "not found"))


if __name__ == "__main__":
    main()
