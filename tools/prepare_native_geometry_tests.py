"""Prepare small test descriptors referencing existing captured geometry."""
import argparse
from pathlib import Path
import json
import struct
from analyze_capture import load
from replay_pack import scan_draw, track_registers


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('replay', type=Path)
    args = parser.parse_args()
    records = [r for r in load(str(args.capture / 'frame_capture.bin'), True) if r[0][1] != 3]
    index = json.loads((args.replay / 'replay-index.json').read_text())
    tracked, inline_fetches = {}, {}
    for cpu, (header, device, objects) in enumerate(records):
        if header[1] == 2 and 'up' in objects and header[2] == 0x8250D964:
            objects['commands'] = objects['up'][1]
        track_registers(tracked, objects.get('commands', b''))
        if 'up' in objects and 0x48BE in tracked:
            inline_fetches[cpu] = (tracked[0x48BE], tracked[0x48BF])
    cases = []
    for record in index['records']:
        if record['kind'] != 'draw':
            continue
        header, device, objects = records[record['cpu_record']]
        if header[1] == 2 and 'up' in objects and header[2] == 0x8250D964:
            objects['commands'] = objects['up'][1]
        packet = scan_draw(objects['commands'][4:])
        if packet is None:
            raise ValueError('Missing actual draw packet')
        payload = packet[1]
        initiator = payload[1]
        source = initiator >> 6 & 3
        code = objects['vs_code'][1]
        case = struct.pack('<7I', record['record'], initiator & 63, initiator >> 16,
                           int(source == 0), initiator >> 11 & 1,
                           payload[2] if source == 0 else 0, len(code)) + code
        fetches = list(struct.unpack_from('>192I', device, 0x480))
        if record['cpu_record'] in inline_fetches:
            fetches[190:192] = inline_fetches[record['cpu_record']]
        case += struct.pack('<192I', *fetches)
        cases.append(case)
    target = args.replay / 'geometry-tests.bin'
    target.write_bytes(struct.pack('<3I', 0x54474446, 2, len(cases)) + b''.join(cases))
    print('%d native geometry cases: %s' % (len(cases), target))


if __name__ == '__main__':
    main()
