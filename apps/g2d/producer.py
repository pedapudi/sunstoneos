#!/usr/bin/env python3
"""Seal one caller-selected 32-bit RGB operation and its exact source bytes."""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import struct


# Pinned sunxi-g2d.h order; the names give channel order from the most significant byte.
FORMATS=('ARGB8888','ABGR8888','RGBA8888','BGRA8888','XRGB8888','XBGR8888','RGBX8888','BGRX8888')
OPERATIONS={'fill':1,'copy':2,'source-over-solid':3,'scale2x':4,'scale':5,'source-over-image':6}


def preparation():
    spec=importlib.util.spec_from_file_location('g2d_producer_preparation',Path(__file__).with_name('prepare.py'))
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value


def extend(board,source,*,owner,authority,client,initial,memory,electrical,supervision,
           width,height,stride,operation,dx,dy,w,h,sx=0,sy=0,color=0,
           sw=0,sh=0,bx=0,by=0,alpha=0,source_format=0,output_format=0):
    prep=preparation()
    geometry=(width,height,stride,source_format,operation,dx,dy,w,h,sx,sy,color)
    extension=(sw,sh,bx,by,alpha,output_format,0,0)
    size=prep.geometry_check(geometry,extension)
    if len(board)!=592 or len(source)!=size or not (0<owner<2**64 and 0<authority<2**64 and 0<=client<16):
        raise ValueError('G2D requires board592, nonzero owner/authority and exact stride*height source bytes.')
    facts=b''.join(hashlib.sha256(data).digest() for data in (initial,memory,electrical,supervision))
    raw=board+struct.pack('<6Q',0x4153373347324131,1,904,owner,authority,client)+facts
    raw+=hashlib.sha256(source).digest()+struct.pack('<8I',*extension)
    raw+=struct.pack('<6I',0,300000000,0x10007,0x10007,800000,1)
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
    for name in ('dx','dy','sx','sy','color','sw','sh','bx','by','alpha'):
        parser.add_argument('--'+name,type=lambda value:int(value,0),default=0)
    for name in ('source-format','output-format'):
        parser.add_argument('--'+name,choices=FORMATS,default='ARGB8888')
    parser.add_argument('--operation',choices=tuple(OPERATIONS),required=True)
    args=parser.parse_args()
    values={name:getattr(args,name) for name in ('owner','authority','client','width','height','dx','dy','sx','sy','color','sw','sh','bx','by','alpha')}
    values.update(stride=args.stride if args.stride is not None else args.width*4,
        w=args.w if args.w is not None else args.width,h=args.h if args.h is not None else args.height,
        operation=OPERATIONS[args.operation],source_format=FORMATS.index(args.source_format),
        output_format=FORMATS.index(args.output_format))
    values.update({name:getattr(args,name).read_bytes() for name in ('initial','memory','electrical','supervision')})
    args.output.write_bytes(extend(args.board.read_bytes(),args.input.read_bytes(),**values))


if __name__=='__main__':main()
