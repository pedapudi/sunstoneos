#!/usr/bin/env python3
"""Create a GPU-only session envelope; never access a device."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

def envelope(prefix,package,admission):
    if len(prefix)!=592 or len(package)!=8288:raise ValueError('Invalid GPU input lengths')
    record=json.loads(admission)
    if record.get('schema')!='sunstone-gpu-trusted-program-v1' or record.get('source_reviewed') is not True:
        raise ValueError('Explicit trusted source review required')
    if record.get('package_sha256')!=hashlib.sha256(package).hexdigest():raise ValueError('Admitted package differs')
    if record.get('source_sha256')!=package[80:112].hex() or record.get('compiler_sha256')!=package[112:144].hex():
        raise ValueError('Program provenance differs')
    return prefix+struct.pack('<4Q',0x4153373347505031,1,8944,8288)+hashlib.sha256(package).digest()+package

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for x in ('base-session','package','program-admission','output'):p.add_argument('--'+x,type=Path,required=True)
    a=p.parse_args()
    a.output.write_bytes(envelope(a.base_session.read_bytes(),a.package.read_bytes(),a.program_admission.read_text()))
