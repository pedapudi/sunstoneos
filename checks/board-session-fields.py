#!/usr/bin/env python3
"""Check native board invariants on recorded and existing radio templates."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('board_prepare',ROOT/'apps/board/prepare.py')
board=importlib.util.module_from_spec(spec);spec.loader.exec_module(board)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('recorded','radio','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();recorded,radio=a.recorded.read_bytes(),a.radio.read_bytes()
    assert board.session_check(recorded[:592],usb_reference_modes=(1,))==2
    assert board.session_check(radio[:592],usb_reference_modes=(1,))==1
    warm=bytearray(recorded[:592]);struct.pack_into('<2Q',warm,24,1,1)
    assert board.session_check(warm,usb_reference_modes=(1,))==2
    cases=['recorded-template','warm-recorded','accepted-radio-headset']
    for name,offset,fmt,value in [('missing-owner',456,'Q',0),('duplicate-owner',464,'Q',struct.unpack_from('<Q',warm,456)[0]),
            ('missing-authority',200,'Q',0),('unused-radio',352,'Q',1),('unused-host',472,'Q',1),
            ('missing-fabric',500,'I',0),('bad-supply',512,'I',0),('reserved',582,'B',1),
            ('qualification-recovery-mismatch',80,'B',warm[80]^255)]:
        bad=bytearray(warm);struct.pack_into('<'+fmt,bad,offset,value)
        try:board.session_check(bad,usb_reference_modes=(1,))
        except ValueError:cases.append(name)
        else:raise AssertionError(name)
    result={'passed':True,'physical_access':False,'cases':cases,
            'inputs':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in (a.recorded,a.radio)},
            'sources':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()
                       for p in (Path(__file__),ROOT/'apps/board/prepare.py')}}
    a.output.write_text(json.dumps(result,indent=2)+'\n');print('PASS',len(cases),'shared board session cases')

if __name__=='__main__':main()
