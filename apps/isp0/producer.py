#!/usr/bin/env python3
"""Seal one fixed 640x480 BGGR12 input for native ISP0 processing."""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import struct


def preparation():
    spec=importlib.util.spec_from_file_location('isp0_producer_preparation',Path(__file__).with_name('prepare.py'))
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value


def extend(board,source,*,owner,authority,client,initial,memory,electrical,supervision):
    prep=preparation()
    prep.input_check(source)
    if len(board)!=592 or not all(type(v) is int for v in (owner,authority,client)) or not (
            0<owner<2**64 and 0<authority<2**64 and 0<=client<16):
        raise ValueError('ISP0 requires board592, nonzero owner/authority and a valid clock client.')
    facts=b''.join(hashlib.sha256(data).digest() for data in (initial,memory,electrical,supervision))
    raw=board+struct.pack('<6Q',0x4153373349504131,1,880,owner,authority,client)+facts
    raw+=hashlib.sha256(source).digest()+bytes(32)
    # A733_RAIL_DCDC2 is enum1 in include/a733_rail_leases.h.
    raw+=struct.pack('<4I',0x10007,0x10007,800000,1)
    raw+=struct.pack('<8I',640,480,1280,640,12,0,0,0)
    prep.session_check(raw)
    return raw


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('board','input','initial','memory','electrical','supervision','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    for name in ('owner','authority','client'):
        parser.add_argument('--'+name,type=lambda value:int(value,0),required=True)
    args=parser.parse_args()
    values={name:getattr(args,name) for name in ('owner','authority','client')}
    values.update({name:getattr(args,name).read_bytes() for name in ('initial','memory','electrical','supervision')})
    args.output.write_bytes(extend(args.board.read_bytes(),args.input.read_bytes(),**values))


if __name__=='__main__':main()
