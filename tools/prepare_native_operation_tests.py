"""Own native clear/resolve fixtures, preserving explicit copy-register sources."""
import argparse,json,struct
from pathlib import Path
from analyze_capture import load
from replay_pack import load_xenia_registers,copy_dest_registers

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture',type=Path);parser.add_argument('replay',type=Path);args=parser.parse_args()
    cpu=[r for r in load(str(args.capture/'frame_capture.bin'),True) if r[0][1]!=3]
    gpu=load_xenia_registers(args.capture/'xenia_regs.bin')
    cases=[]
    for record in json.loads((args.replay/'replay-index.json').read_text())['records']:
        if record['kind'] not in ('clear','resolve'):continue
        h,device,objects=cpu[record['cpu_record']]
        commands=objects.get('commands',b'')[4:]
        if record['kind']=='clear':
            depth,rects=objects['clear'];flags,color,stencil=h[6],h[7],h[9]
            fetch=[0]*6;copy=[0]*4;parsed=False
        else:
            flags,texture,obj=objects['resolve'];fetch=struct.unpack_from('>6I',obj,0x1C)
            depth,color,stencil,rects=0,0,0,[]
            copy=copy_dest_registers(commands);parsed=bool(copy[0])
            if not parsed:
                candidates={tuple(state[r] for r in range(0x2318,0x231C)) for state in gpu if state.get(0x2318,0) and state.get(0x2319,0)&0x1FFFFFFF==fetch[1]&0x1FFFF000}
                if len(candidates)!=1:raise ValueError('No unique explicit copy-register source')
                copy=candidates.pop()
        body=struct.pack('<5If3I',record['record'],int(record['kind']=='resolve'),flags,color,stencil,depth,len(rects),int(parsed),len(device))+device
        body+=b''.join(struct.pack('<4i',*rect) for rect in rects)
        for offset in range(0x3190,0x31A4,4):
            address,data=objects[offset];body+=struct.pack('<2I',address,len(data))+data
        body+=struct.pack('<10I',*fetch,*copy)+struct.pack('<I',len(commands))+commands
        cases.append(body)
    (args.replay/'operation-tests.bin').write_bytes(struct.pack('<3I',0x504F4446,1,len(cases))+b''.join(cases))
    print(args.capture.name,len(cases),'owned clear/resolve fixtures')
if __name__=='__main__':main()
