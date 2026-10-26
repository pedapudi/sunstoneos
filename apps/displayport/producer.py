#!/usr/bin/env python3
"""Seal the board provider prefix and independent TYPEC24 DisplayPort authority."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct


def preparation():
    spec=importlib.util.spec_from_file_location('displayport_producer_prepare',Path(__file__).with_name('prepare.py'))
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

# Sessions name MBUS at boot0's 600 MHz, which every EL3 entry restores (tools/vendor-mbus.py).
def _vendor_mbus():
    import importlib.util as u
    s=u.spec_from_file_location('vendor_mbus',Path(__file__).resolve().parents[2]/'tools/vendor-mbus.py')
    m=u.module_from_spec(s);s.loader.exec_module(m);return m

def extend(board,*,owner,authority,client,facts,clocks,typec,latch,source,pins):
    board=_vendor_mbus().board_session(board) if len(board)==592 else board
    prep=preparation()
    if len(board)!=592 or len(facts)!=6 or len(clocks)!=8 or len(typec)!=3 or len(latch)!=4 or len(source)!=6 or len(pins)!=4:
        raise ValueError('DisplayPort requires the typed board, six facts and complete clock/TYPEC24 fields.')
    if not (0<owner<2**64 and 0<authority<2**64 and 0<=client<16):
        raise ValueError('Invalid DisplayPort owner, authority or clock client.')
    raw=board+struct.pack('<6Q',0x4153373344504131,1,952,owner,authority,client)
    raw+=b''.join(hashlib.sha256(value).digest() for value in facts)
    raw+=struct.pack('<8I3Q2Q2I6I',*clocks,*typec,*latch,*source)
    raw+=bytes(value for row in pins for value in row)+bytes(4)
    prep.session_check(raw)
    return raw


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('board','profile','output','isolation','memory','analog','power','serdes','supervision'):
        parser.add_argument('--'+name,type=Path,required=True)
    for name in ('owner','authority','client'):parser.add_argument('--'+name,type=lambda value:int(value,0),required=True)
    args=parser.parse_args();profile=json.loads(args.profile.read_bytes())
    require_keys={'clocks','typec','latch','source','pins'}
    if set(profile)!=require_keys:raise ValueError('DisplayPort profile must contain exactly clocks/typec/latch/source/pins.')
    facts=[getattr(args,name).read_bytes() for name in ('isolation','memory','analog','power','serdes','supervision')]
    raw=extend(args.board.read_bytes(),owner=args.owner,authority=args.authority,client=args.client,facts=facts,**profile)
    args.output.write_bytes(raw)

if __name__=='__main__':main()
