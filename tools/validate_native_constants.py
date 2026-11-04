"""Check direct device float constants against the synchronized GPU draws.

Shader literals are embedded by XenosRecomp and therefore do not use the device
constant bank. Used registers come from Xenia's independent microcode analyzer.
"""
import argparse
import json
from pathlib import Path
import re
import struct
from analyze_capture import load
from replay_pack import load_xenia_registers


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('replay', type=Path)
    parser.add_argument('--assets', type=Path, default=Path('build/native-shaders'))
    args = parser.parse_args()
    capabilities = json.loads((args.assets / 'capabilities.json').read_text())
    used = {entry['name']:entry['float_registers'] for entry in capabilities['shaders']}
    literals = {name:set(map(int,re.findall(r'float4 c(\d+) = asfloat\(uint4\(', (args.assets / (name+'.hlsl')).read_text()))) for name in used}
    cpu = [r for r in load(str(args.capture / 'frame_capture.bin'),True) if r[0][1] != 3]
    gpu = load_xenia_registers(str(args.capture / 'xenia_regs.bin'))
    records = json.loads((args.replay / 'replay-index.json').read_text())['records']
    checked = 0
    differences = []
    for record in records:
        if record['kind'] != 'draw':
            continue
        device = cpu[record['cpu_record']][1]
        registers = gpu[record['gpu_draw']]
        for name,offset,base in ((record['vs'],0x780,0x4000),(record['ps'],0x1780,0x4400)):
            if not name:
                continue
            for index in used[name]:
                if index in literals[name]:
                    continue
                actual = struct.unpack_from('>4I',device,offset+index*16)
                expected = tuple(registers[base+index*4+k] for k in range(4))
                checked += 1
                if actual != expected:
                    differences.append(dict(record=record['record'],shader=name,register=index,device=actual,gpu=expected))
    result=dict(checked_float4=checked,mismatches=len(differences),examples=differences[:20])
    (args.replay / 'validation-device-constants.json').write_text(json.dumps(result,indent=2))
    print('%s: %d float4 registers checked, %d mismatches' % (args.capture.name,checked,len(differences)))
    if differences:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
