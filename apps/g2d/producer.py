#!/usr/bin/env python3
"""Seal one caller-selected ARGB8888 operation and its exact source bytes."""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import struct


def preparation():
    spec=importlib.util.spec_from_file_location('g2d_producer_preparation',Path(__file__).with_name('prepare.py'))
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value


def extend(board,source,*,owner,authority,client,initial,memory,electrical,supervision,
           width,height,stride,operation,dx,dy,w,h,sx=0,sy=0,color=0):
    prep=preparation()
    geometry=(width,height,stride,0,operation,dx,dy,w,h,sx,sy,color)
    size=prep.geometry_check(geometry)
    if len(board)!=592 or len(source)!=size or not (0<owner<2**64 and 0<authority<2**64 and 0<=client<16):
        raise ValueError('G2D requires board592, nonzero owner/authority and exact stride*height source bytes.')
    facts=b''.join(hashlib.sha256(data).digest() for data in (initial,memory,electrical,supervision))
    raw=board+struct.pack('<6Q',0x4153373347324131,1,904,owner,authority,client)+facts
    raw+=hashlib.sha256(source).digest()+bytes(32)
    raw+=struct.pack('<6I',0x01000003,100000000,0x10007,0x10007,800000,1)
    raw+=struct.pack('<12I',*geometry)
    prep.session_check(raw)
    return raw


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('board','input','initial','memory','electrical','supervision','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    for name in ('owner','authority','client','width','height'):
        parser.add_argument('--'+name,type=lambda value:int(value,0),required=True)
    for name in ('stride','w','h'):
        parser.add_argument('--'+name,type=lambda value:int(value,0))
    for name in ('dx','dy','sx','sy','color'):
        parser.add_argument('--'+name,type=lambda value:int(value,0),default=0)
    parser.add_argument('--operation',choices=('fill','copy','source-over-solid','scale2x'),required=True)
    args=parser.parse_args()
    values={name:getattr(args,name) for name in ('owner','authority','client','width','height','dx','dy','sx','sy','color')}
    values.update(stride=args.stride if args.stride is not None else args.width*4,
        w=args.w if args.w is not None else args.width,h=args.h if args.h is not None else args.height,
        operation={'fill':1,'copy':2,'source-over-solid':3,'scale2x':4}[args.operation])
    values.update({name:getattr(args,name).read_bytes() for name in ('initial','memory','electrical','supervision')})
    args.output.write_bytes(extend(args.board.read_bytes(),args.input.read_bytes(),**values))


if __name__=='__main__':main()
