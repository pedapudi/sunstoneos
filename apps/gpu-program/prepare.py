#!/usr/bin/env python3
"""Create a GPU-only session envelope; never access a device."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

def envelope(prefix,package,admission=None):
    if len(prefix)!=592 or len(package)!=8288:raise ValueError('Invalid GPU input lengths')
    return prefix+struct.pack('<4Q',0x4153373347505031,1,8944,8288)+hashlib.sha256(package).digest()+package

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for x in ('base-session','package','output'):p.add_argument('--'+x,type=Path,required=True)
    p.add_argument('--program-admission',type=Path)
    a=p.parse_args()
    a.output.write_bytes(envelope(a.base_session.read_bytes(),a.package.read_bytes(),None))
