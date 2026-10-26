#!/usr/bin/env python3
"""Produce the sealed two-picture H264 encoder extension from an existing admitted board session."""
import argparse,hashlib,struct
from pathlib import Path

# Sessions name MBUS at boot0's 600 MHz, which every EL3 entry restores (tools/vendor-mbus.py).
def _vendor_mbus():
    import importlib.util as u
    s=u.spec_from_file_location('vendor_mbus',Path(__file__).resolve().parents[2]/'tools/vendor-mbus.py')
    m=u.module_from_spec(s);s.loader.exec_module(m);return m

def extend(board,raw,*,owner,authority,client,initial,memory,electrical,supervision,module,hz):
    board=_vendor_mbus().board_session(board) if len(board)==592 else board
    if len(board)!=592 or len(raw)!=921600 or not owner or not authority or not 0<=client<16:
        raise ValueError('Encoder requires board592, two VGA NV12 frames and nonzero owners.')
    facts=[hashlib.sha256(x).digest() for x in (initial,memory,electrical,supervision)]
    result=board+struct.pack('<6Q',0x4153373348454131,1,856,owner,authority,client)+b''.join(facts)
    result+=hashlib.sha256(raw).digest()+bytes(32)+struct.pack('<6I',module,hz,0x10007,0x10007,800000,1)
    assert len(result)==856
    return result
# The vendor operating point (sun60iw2p1-cpu-vf.dtsi ve-opp-table): module mux 1,
# divider 1, 624 MHz. The decoder's mux 1 is PLL_VE0 and the encoder's PLL_VE1.
VENDOR={'module':0x01000000,'hz':624000000}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('board','input','initial','memory','electrical','supervision','output'):p.add_argument('--'+name,type=Path,required=True)
    for name in ('owner','authority','client','module','hz'):p.add_argument('--'+name,type=lambda x:int(x,0),required=name not in VENDOR,default=VENDOR.get(name))
    a=p.parse_args();kw={n:getattr(a,n) for n in ('owner','authority','client','module','hz')}
    kw.update({n:getattr(a,n).read_bytes() for n in ('initial','memory','electrical','supervision')})
    a.output.write_bytes(extend(a.board.read_bytes(),a.input.read_bytes(),**kw))
if __name__=='__main__':main()
