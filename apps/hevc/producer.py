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


def extend(board,clip,*,first_bytes,owner,authority,client,initial,memory,electrical,supervision,module,hz):
    if len(board)!=592 or not 0<first_bytes<len(clip):
        raise ValueError('HEVC requires the board592 prefix and two nonempty clip ranges.')
    facts=b''.join(hashlib.sha256(data).digest() for data in (initial,memory,electrical,supervision))
    raw=board+struct.pack('<6Q',0x4153373348354131,1,856,owner,authority,client)+facts
    raw+=hashlib.sha256(clip).digest()+struct.pack('<4Q',0,first_bytes,first_bytes,len(clip)-first_bytes)
    raw+=struct.pack('<6I',module,hz,0x10007,0x10007,800000,1)
    preparation().session_check(raw)
    return raw


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('board','input','initial','memory','electrical','supervision','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    for name in ('first-bytes','owner','authority','client','module','hz'):
        parser.add_argument('--'+name,type=lambda value:int(value,0),required=True)
    args=parser.parse_args()
    values={name:getattr(args,name) for name in ('first_bytes','owner','authority','client','module','hz')}
    values.update({name:getattr(args,name).read_bytes() for name in ('initial','memory','electrical','supervision')})
    args.output.write_bytes(extend(args.board.read_bytes(),args.input.read_bytes(),**values))


if __name__=='__main__':main()
