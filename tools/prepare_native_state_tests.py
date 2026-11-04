"""Reference native device-state reconstruction against synchronized draws.

Scissor words are explicit packet inputs. Index offsets come from the actual
engine arguments; all other state is reconstructed from the device shadow.
"""
import argparse,json,struct
from analyze_capture import load
from pathlib import Path

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture',type=Path)
    parser.add_argument('replay',type=Path)
    args=parser.parse_args()
    cpu=[r for r in load(str(args.capture/'frame_capture.bin'),True) if r[0][1]!=3]
    records=json.loads((args.replay/'replay-index.json').read_text())['records']
    cases=[]
    for record in records:
        if record['kind']!='draw':continue
        header,device,_=cpu[record['cpu_record']]
        offset=header[5] if header[1] in (0,1) else 0
        cases.append(struct.pack('<3I',record['record'],offset,len(device))+device)
    (args.replay/'state-tests.bin').write_bytes(struct.pack('<3I',0x54534446,1,len(cases))+b''.join(cases))
    print('%s: %d device-state cases'%(args.capture.name,len(cases)))

if __name__=='__main__':main()
