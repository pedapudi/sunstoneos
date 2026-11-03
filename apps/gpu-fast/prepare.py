#!/usr/bin/env python3
"""Prepare the GPU benchmark's typed input."""
import argparse
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


if __name__=='__main__':
    main()
