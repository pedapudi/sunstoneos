#!/usr/bin/env python3
"""Seal two contiguous HEVC input ranges onto an existing admitted board session."""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import struct


def preparation():
    spec=importlib.util.spec_from_file_location('hevc_producer_preparation',Path(__file__).with_name('prepare.py'))
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

# Sessions name MBUS at boot0's 600 MHz, which every EL3 entry restores (tools/vendor-mbus.py).
def _vendor_mbus():
    import importlib.util as u
    s=u.spec_from_file_location('vendor_mbus',Path(__file__).resolve().parents[2]/'tools/vendor-mbus.py')
    m=u.module_from_spec(s);s.loader.exec_module(m);return m

def extend(board,clip,*,first_bytes,owner,authority,client,initial,memory,electrical,supervision,module,hz):
    board=_vendor_mbus().board_session(board) if len(board)==592 else board
    if len(board)!=592 or not 0<first_bytes<len(clip):
        raise ValueError('HEVC requires the board592 prefix and two nonempty clip ranges.')
    facts=b''.join(hashlib.sha256(data).digest() for data in (initial,memory,electrical,supervision))
    raw=board+struct.pack('<6Q',0x4153373348354131,1,856,owner,authority,client)+facts
    raw+=hashlib.sha256(clip).digest()+struct.pack('<4Q',0,first_bytes,first_bytes,len(clip)-first_bytes)
    raw+=struct.pack('<6I',module,hz,0x10007,0x10007,800000,1)
    preparation().session_check(raw)
    return raw

# The vendor operating point (sun60iw2p1-cpu-vf.dtsi ve-opp-table): module mux 1,
# divider 1, 624 MHz. The decoder's mux 1 is PLL_VE0 and the encoder's PLL_VE1.
VENDOR={'module':0x01000000,'hz':624000000}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('board','input','initial','memory','electrical','supervision','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    for name in ('first-bytes','owner','authority','client','module','hz'):
        parser.add_argument('--'+name,type=lambda value:int(value,0),required=name not in VENDOR,default=VENDOR.get(name))
    args=parser.parse_args()
    values={name:getattr(args,name) for name in ('first_bytes','owner','authority','client','module','hz')}
    values.update({name:getattr(args,name).read_bytes() for name in ('initial','memory','electrical','supervision')})
    args.output.write_bytes(extend(args.board.read_bytes(),args.input.read_bytes(),**values))


if __name__=='__main__':main()
