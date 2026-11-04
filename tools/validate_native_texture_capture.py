"""Compare in-process native texture production with independent raw captures.

The native producer derives layouts from fetch descriptors and reads guest
memory on the GPU consuming thread. The Python reference uses raw bank dumps
and Xenia's independently captured mip layouts from that same register stream.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from replay_pack import (load_textures, build_texture_table, build_cube_table,
                         register_stream_hash)
from xenos_mips import load_mips, build_mip_table


def mip_records(data):
    """The table's record order is immaterial; resource indices are explicit."""
    at = 0

    def word():
        nonlocal at
        if at + 4 > len(data):
            raise ValueError('Truncated native mip table')
        value, = struct.unpack_from('<I', data, at)
        at += 4
        return value

    result = {}
    for _ in range(word()):
        index, count = word(), word()
        if index in result or not 1 <= count <= 15:
            raise ValueError('Duplicate resource or invalid native mip count')
        levels = []
        for _ in range(count):
            width, height = word(), word()
            size = width * height * 4
            if not width or not height or at + size > len(data):
                raise ValueError('Invalid native mip image extent')
            levels.append((width, height, data[at:at + size]))
            at += size
        result[index] = levels
    if at != len(data):
        raise ValueError('Unexpected trailing native mip bytes')
    return result


def validate(directory):
    report = json.loads((directory / 'native-texture-report.json').read_text())
    if (report.get('mode') != 'native-memory-texture-producer' or
            report.get('scope') != 'gpu-consumer-first-use-per-address' or
            not report.get('complete') or not report.get('passed') or
            report.get('failures') != 0 or report.get('native_rendering') is not False):
        raise ValueError('Native memory texture production did not pass')
    register_hash = register_stream_hash((directory / 'xenia_regs.bin').read_bytes())
    if report.get('register_hash') != f'{register_hash:016X}':
        raise ValueError('Native textures belong to a different GPU register stream')
    textures = load_textures(directory / 'gpu_textures.bin')
    indices, expected_textures = build_texture_table(textures)
    if not textures or report.get('textures') != len(textures):
        raise ValueError('Native texture resource count differs')
    expected_cubes = build_cube_table(textures, indices)
    expected_mips = build_mip_table(load_mips(directory / 'gpu_mips.bin', register_hash), indices)
    tables = {}
    for name, expected in (('textures', expected_textures), ('cubes', expected_cubes), ('mips', expected_mips)):
        actual = (directory / f'native-{name}.rgba').read_bytes()
        if name == 'mips':
            if mip_records(actual) != mip_records(expected):
                raise ValueError('Native memory mip pixels differ from independent reference')
        elif actual != expected:
            raise ValueError(f'Native memory {name} differ from independent reference')
        tables[name] = dict(bytes=len(actual), sha256=hashlib.sha256(actual).hexdigest())
    levels = sum(map(len, mip_records(expected_mips).values()))
    if report.get('mip_levels') != levels:
        raise ValueError('Native mip level count differs')
    return dict(passed=True, scope=report['scope'], native_rendering=False,
                register_hash=f'{register_hash:016X}', textures=len(textures),
                cubes=struct.unpack_from('<I', expected_cubes)[0], mip_levels=levels,
                tables=tables)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--json', type=Path)
    args = parser.parse_args()
    result = validate(args.directory)
    if args.json:
        args.json.write_text(json.dumps(result, indent=2) + '\n')
    print(f"Native memory texture capture: {result['textures']} bases, "
          f"{result['cubes']} cubes, {result['mip_levels']} mip levels, exact reference pixels")


if __name__ == '__main__':
    main()
