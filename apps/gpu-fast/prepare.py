#!/usr/bin/env python3
"""Prepare the GPU benchmark's typed input and complete binary expectation."""
import argparse
import json
from pathlib import Path
import struct


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--jobs',type=int,choices=(0,10000),required=True)
    p.add_argument('--cache',choices=('writeback','uncached'),default='writeback')
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    a.output.mkdir(parents=True,exist_ok=True)
    cache=int(a.cache=='uncached')
    (a.output/'input.bin').write_bytes(struct.pack('<4I',0x31464750,a.jobs,cache,0))
    timing={'min':0,'max':4799999} if a.jobs else 0
    words=[0x31464750,2,cache,a.jobs,24000000,a.jobs,a.jobs,
           {'min':0,'max':a.jobs|(a.jobs<<16)} if a.jobs else 0]+[timing]*9+[
           {'min':1,'max':720000000} if a.jobs else 0,
           {'min':0,'max':720000000} if a.jobs else 0,4 if a.jobs else 0,64 if a.jobs else 0]+[timing]*3
    assert len(words)==24
    (a.output/'expect.json').write_text(json.dumps({'schema':'sunstone-board-expect-u32-v1','words':words},indent=2)+'\n')


if __name__=='__main__':
    main()
