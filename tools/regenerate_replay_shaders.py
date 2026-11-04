"""Regenerate HLSL for saved replay shaders without changing container numbers.

Run after updating/building the locally patched XenosRecomp. replay_pack.py
then recompiles the regenerated sources to DXIL in each replay directory.
"""
import argparse
import json
import subprocess
from pathlib import Path


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--replay-dir", type=Path, action="append", required=True)
    parser.add_argument("--shader-dir", type=Path, default=root / "build/shaders_split")
    args = parser.parse_args()
    names = set()
    for directory in args.replay_dir:
        for record in json.loads((directory / "replay-index.json").read_text())["records"]:
            if record["kind"] == "draw":
                names.update(filter(None, (record["vs"], record["ps"])))
    executable = root / "XenosRecomp/build/XenosRecomp/XenosRecomp.exe"
    common = root / "XenosRecomp/XenosRecomp/shader_common.h"
    for name in sorted(names):
        source = args.shader_dir / (name + ".bin")
        target = args.shader_dir / (name + ".hlsl")
        temporary = target.with_suffix(".hlsl.new")
        subprocess.run([str(executable), str(source), str(temporary), str(common)], check=True, capture_output=True)
        if not temporary.is_file() or temporary.stat().st_size == 0:
            raise ValueError("No generated HLSL for " + name)
        temporary.replace(target)
    print("Regenerated %d shader sources; repack each replay to compile DXIL" % len(names))


if __name__ == "__main__":
    main()
