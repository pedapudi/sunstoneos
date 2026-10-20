#!/usr/bin/env python3
"""Produce the sealed two-picture H264 encoder extension from an existing admitted board session."""
import argparse,hashlib,struct
from pathlib import Path

def extend(board,raw,*,owner,authority,client,initial,memory,electrical,supervision,module,hz):
    if len(board)!=592 or len(raw)!=921600 or not owner or not authority or not 0<=client<16:
        raise ValueError('Encoder requires board592, two VGA NV12 frames and nonzero owners.')
    facts=[hashlib.sha256(x).digest() for x in (initial,memory,electrical,supervision)]
    result=board+struct.pack('<6Q',0x4153373348454131,1,856,owner,authority,client)+b''.join(facts)
    result+=hashlib.sha256(raw).digest()+bytes(32)+struct.pack('<6I',module,hz,0x10007,0x10007,800000,1)
    assert len(result)==856
    return result

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('board','input','initial','memory','electrical','supervision','output'):p.add_argument('--'+name,type=Path,required=True)
    for name in ('owner','authority','client','module','hz'):p.add_argument('--'+name,type=lambda x:int(x,0),required=True)
    a=p.parse_args();kw={n:getattr(a,n) for n in ('owner','authority','client','module','hz')}
    kw.update({n:getattr(a,n).read_bytes() for n in ('initial','memory','electrical','supervision')})
    a.output.write_bytes(extend(a.board.read_bytes(),a.input.read_bytes(),**kw))
if __name__=='__main__':main()
