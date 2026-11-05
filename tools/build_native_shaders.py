"""Build native shader assets from the user's extracted containers.

The catalog preserves full virtual metadata and physical code. It is derived
game data and belongs under build/, alongside its incremental compiler cache.
"""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def container_parts(data):
    if len(data) < 36:
        raise ValueError("Truncated shader container")
    flags, virtual, physical = struct.unpack_from(">3I", data)
    if flags & 0xFFFFFFFE != 0x102A1100 or virtual < 36 or not physical or virtual + physical != len(data):
        raise ValueError("Invalid shader container metadata")
    return 0 if flags & 1 else 1, data[:virtual], data[virtual:]


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shader-dir", type=Path, default=root / "build/shaders_split")
    parser.add_argument("--out", type=Path, default=root / "build/native-shaders")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 16:
        parser.error("--jobs must be between 1 and 16")
    args.out.mkdir(parents=True, exist_ok=True)
    xenos = root / "XenosRecomp/build/XenosRecomp/XenosRecomp.exe"
    common = root / "XenosRecomp/XenosRecomp/shader_common.h"
    dxc = root / "XenosRecomp/thirdparty/dxc-bin/bin/x64/dxc.exe"
    flags = ["-E", "main", "-HV", "2021", "-all-resources-bound", "-Wno-ignored-attributes"]
    tool_digest = hashlib.sha256(b"native-catalog-v1\0" + json.dumps(flags).encode())
    for path in (xenos, common, dxc, dxc.with_name("dxcompiler.dll"), dxc.with_name("dxil.dll")):
        tool_digest.update(path.read_bytes())
    tool_key = tool_digest.digest()
    paths = sorted(args.shader_dir.glob("*.bin"))
    if not paths:
        raise ValueError("No extracted shader containers")

    def compile_one(path):
        data = path.read_bytes()
        stage, virtual, physical = container_parts(data)
        key = hashlib.sha256(tool_key + data).hexdigest()
        source = args.out / (path.stem + ".hlsl")
        dxil = args.out / (path.stem + ".dxil")
        cache = args.out / (path.stem + ".key")
        cached = (cache.exists() and cache.read_text() == key and source.exists() and dxil.exists())
        if not cached:
            temporary = source.with_suffix(".hlsl.new")
            compiled = dxil.with_suffix(".dxil.new")
            commands = [[str(xenos), str(path), str(temporary), str(common)]]
            run = subprocess.run(commands[0], capture_output=True, timeout=60)
            if run.returncode:
                raise RuntimeError(path.name + ": XenosRecomp failed: " + run.stderr.decode(errors="replace"))
            text = temporary.read_text(encoding="utf-8")
            temporary.write_text(text + "\n// Per-draw specialization bits from the native renderer (shared constant 536): alpha test (bit 1).\nuint g_SpecConstants() { return g_NativeSpecConstants; }\n", encoding="utf-8")
            run = subprocess.run([str(dxc), "-T", "vs_6_0" if stage == 0 else "ps_6_0", *flags,
                                  str(temporary), "-Fo", str(compiled)], capture_output=True, timeout=120)
            if run.returncode:
                raise RuntimeError(path.name + ": DXC failed: " + run.stderr.decode(errors="replace"))
            temporary.replace(source)
            compiled.replace(dxil)
            cache.write_text(key)
        bytecode = dxil.read_bytes()
        if not bytecode.startswith(b"DXBC"):
            raise ValueError(path.name + ": invalid DXIL output")
        return path.stem, stage, virtual, physical, source.read_bytes(), bytecode, key, cached

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        entries = list(pool.map(compile_one, paths))
    identities = set()
    catalog = bytearray(struct.pack("<3I", 0x43534446, 1, len(entries)))
    manifest = []
    for name, stage, virtual, physical, source, dxil, key, cached in entries:
        identity = (stage, virtual, physical)
        if identity in identities:
            raise ValueError("Duplicate full shader identity: " + name)
        identities.add(identity)
        encoded_name = name.encode("ascii")
        catalog += struct.pack("<6I", len(encoded_name), stage, len(virtual), len(physical), len(source), len(dxil))
        catalog += encoded_name + virtual + physical + source + dxil
        manifest.append(dict(name=name, stage="vs" if stage == 0 else "ps", key=key,
                             virtual_bytes=len(virtual), physical_bytes=len(physical), dxil_bytes=len(dxil)))
    target = args.out / "catalog.bin"
    temporary = target.with_suffix(".bin.new")
    temporary.write_bytes(catalog)
    temporary.replace(target)
    (args.out / "manifest.json").write_text(json.dumps(dict(version=1, shaders=manifest), indent=2), encoding="utf-8")
    print("Native catalog: %d shaders, %d compiled, %d cached, %d bytes" %
          (len(entries), sum(not e[-1] for e in entries), sum(e[-1] for e in entries), len(catalog)))


if __name__ == "__main__":
    main()
