"""Dump the useful XEX2 headers of one or more Xbox 360 executables.

Usage: python xex_info.py <file.xex> [...]
"""
import struct
import sys

KEYS = {
    0x000002FF: "resource_info",
    0x000003FF: "file_format_info",
    0x00010001: "original_base_address",
    0x00010100: "entry_point",
    0x00010201: "image_base_address",
    0x000103FF: "import_libraries",
    0x00018002: "checksum_timestamp",
    0x000183FF: "original_pe_name",
    0x000200FF: "static_libraries",
    0x00020104: "tls_info",
    0x00020200: "default_stack_size",
    0x00020301: "default_fs_cache_size",
    0x00020401: "default_heap_size",
    0x00030000: "system_flags",
    0x00040006: "execution_info",
    0x000405FF: "title_workspace_size",
    0x00E10402: "multidisc_media_ids",
}
ENC = {0: "none", 1: "normal"}
COMP = {0: "none", 1: "basic", 2: "normal(lzx)", 3: "delta"}


def be32(b, o):
    return struct.unpack(">I", b[o:o + 4])[0]


def info(path):
    b = open(path, "rb").read()
    print("== %s (%d bytes)" % (path, len(b)))
    flags, pe_off, _, sec_off, count = struct.unpack(">IIIII", b[4:0x18])
    kinds = []
    if flags & 1: kinds.append("title")
    if flags & 2: kinds.append("exports")
    if flags & 8: kinds.append("dll")
    if flags & 0x10: kinds.append("patch")
    print("  module flags: %08X (%s)  pe_offset=%X" % (flags, ",".join(kinds), pe_off))
    print("  image size: %X  load address: %08X" % (be32(b, sec_off + 4), be32(b, sec_off + 0x110)))
    for i in range(count):
        key, val = struct.unpack(">II", b[0x18 + i * 8:0x20 + i * 8])
        name = KEYS.get(key, "unk_%08X" % key)
        if (key & 0xFF) in (0, 1):
            print("  %-22s %08X" % (name, val))
            continue
        if key == 0x000003FF:
            enc, comp = struct.unpack(">HH", b[val + 4:val + 8])
            print("  %-22s encryption=%s compression=%s" % (name, ENC.get(enc, enc), COMP.get(comp, comp)))
        elif key == 0x000183FF:
            size = be32(b, val)
            print("  %-22s %s" % (name, b[val + 4:val + size].split(b"\0")[0].decode()))
        elif key == 0x000200FF:
            size = be32(b, val)
            libs = []
            for o in range(val + 4, val + size, 16):
                n = b[o:o + 8].split(b"\0")[0].decode()
                ma, mi, bu, qfe = struct.unpack(">HHHH", b[o + 8:o + 16])
                libs.append("%s %d.%d.%d.%d" % (n, ma, mi, bu, qfe & 0xFF))
            print("  %-22s %s" % (name, ", ".join(libs)))
        elif key == 0x000103FF:
            size, str_size, n = struct.unpack(">III", b[val:val + 12])
            names = [s.decode() for s in b[val + 12:val + 12 + str_size].split(b"\0") if s]
            print("  %-22s %s" % (name, ", ".join(names)))
            o = val + 12 + str_size
            for _ in range(n):
                lsize = be32(b, o)
                rec_count = struct.unpack(">H", b[o + 0x26:o + 0x28])[0]
                print("    lib #%d: %d import records" % (b[o + 0x25], rec_count))
                o += lsize
        elif key == 0x00040006:
            media, ver, base, tid = struct.unpack(">IIII", b[val:val + 16])
            print("  %-22s title_id=%08X version=%08X" % (name, tid, ver))
        else:
            print("  %-22s @%08X" % (name, val))


if __name__ == "__main__":
    for p in sys.argv[1:]:
        info(p)
