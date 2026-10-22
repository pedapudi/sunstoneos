#!/usr/bin/env python3
"""Seal fixed combined-field NV12 input for DI301 WEAVE, motion or TNR, with optional repeated jobs."""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import struct


def preparation():
    spec=importlib.util.spec_from_file_location('deinterlace_producer_preparation',Path(__file__).with_name('prepare.py'))
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value


def field_pattern(mode=1,repeats=0):
    """Deterministic Y/UV lines distinguish both fields and every plane row.

    Each later frame shifts the luma and chroma values, so every job of a
    sequence reads different frames."""
    if mode not in (1,2,3) or not 0<=repeats<=3:raise ValueError('Unsupported DI301 mode or repeats.')
    y=bytes(16+((x//16+row//2*3+(row&1)*71)%220)
        for row in range(480) for x in range(640))
    uv=bytes((32+((x//2+row//2*5+(row&1)*83)%192))
        for row in range(240) for x in range(640))
    frames=repeats+(1 if mode==1 else 2)
    return b''.join(bytes(16+(value-16+23*k)%220 for value in y)+bytes(32+(value-32+17*k)%192 for value in uv)
        for k in range(frames))


STRENGTHS=('fixed','low','middle','high')


def extend(board,source,*,owner,authority,client,initial,memory,electrical,supervision,mode=1,strength=0,repeats=0):
    prep=preparation()
    if len(board)!=592 or mode not in (1,2,3) or not 0<=repeats<=3 or len(source)!=460800*(repeats+(1 if mode==1 else 2)) or not (0<owner<2**64 and 0<authority<2**64 and 0<=client<16):
        raise ValueError('Deinterlace requires board592, nonzero owner/authority and exact mode-specific NV12 bytes.')
    facts=b''.join(hashlib.sha256(data).digest() for data in (initial,memory,electrical,supervision))
    raw=board+struct.pack('<6Q',0x4153373344494131,1,888,owner,authority,client)+facts
    raw+=hashlib.sha256(source).digest()+bytes(32)
    raw+=struct.pack('<6I',1,300000000,0x10007,0x10007,800000,1)
    raw+=struct.pack('<7I2H',640,480,640,640,0x3231564e,mode,1,strength,repeats)
    prep.session_check(raw)
    return raw


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode',choices=('weave','motion','tnr'),default='weave')
    parser.add_argument('--strength',choices=STRENGTHS,default='fixed',
        help='TNR strength: the source fixed value or an adaptive initial gain level.')
    parser.add_argument('--repeats',type=int,choices=range(4),default=0,
        help='Jobs after the first; each consumes one more frame and produces one more output.')
    parser.add_argument('--pattern-output',type=Path,
        help='Write the frames of every job, then exit.')
    for name in ('board','input','initial','memory','electrical','supervision','output'):
        parser.add_argument('--'+name,type=Path)
    for name in ('owner','authority','client'):
        parser.add_argument('--'+name,type=lambda value:int(value,0))
    args=parser.parse_args()
    if args.pattern_output:
        args.pattern_output.write_bytes(field_pattern(dict(weave=1,motion=2,tnr=3)[args.mode],args.repeats));return
    names=('board','input','initial','memory','electrical','supervision','output','owner','authority','client')
    if any(getattr(args,name) is None for name in names):parser.error('All session inputs are required.')
    facts={name:getattr(args,name).read_bytes() for name in ('initial','memory','electrical','supervision')}
    args.output.write_bytes(extend(args.board.read_bytes(),args.input.read_bytes(),owner=args.owner,
        authority=args.authority,client=args.client,mode=dict(weave=1,motion=2,tnr=3)[args.mode],
        strength=STRENGTHS.index(args.strength),repeats=args.repeats,**facts))


if __name__=='__main__':main()
