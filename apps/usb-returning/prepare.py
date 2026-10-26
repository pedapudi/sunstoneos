#!/usr/bin/env python3
"""Copy the existing board configuration fields into the finite USB input."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

def _vendor_mbus():
    import importlib.util as u
    s=u.spec_from_file_location('vendor_mbus',Path(__file__).resolve().parents[2]/'tools/vendor-mbus.py')
    m=u.module_from_spec(s);s.loader.exec_module(m);return m

def input_bytes(config,generation):
    fields=[]
    for name,count in [('fabric',3),('fabric_hz',3),('usb_supply_uv',4)]:
        values=config[name]
        if len(values)!=count or any(type(v) is not int or not 0<=v<2**32 for v in values):
            raise ValueError('Invalid '+name)
        fields+=values
    # MBUS as the restored image finds it (tools/vendor-mbus.py).
    words,rates=_vendor_mbus().fabric(fields[0:3],fields[3:6]);fields[0:6]=words+rates
    pins=config['pmic_expected']
    if len(pins)!=2 or any(len(pin)!=3 for pin in pins):raise ValueError('Invalid PMIC pin extent')
    if any(type(v) is not int or not 0<=v<=15 for pin in pins for v in pin):raise ValueError('Invalid PMIC pin value')
    fields += [v for pin in pins for v in pin]
    if type(generation) is not int or not 0<generation<2**64:raise ValueError('Invalid generation')
    return struct.pack('<4Q16I',0x31504e4953425355,1,96,generation,*fields)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config',type=Path,required=True)
    parser.add_argument('--generation',type=int,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();raw=args.config.read_bytes();data=input_bytes(json.loads(raw),args.generation)
    args.output.write_bytes(data)
    args.output.with_suffix('.json').write_text(json.dumps({'input_sha256':hashlib.sha256(data).hexdigest(),'config_sha256':hashlib.sha256(raw).hexdigest(),'producer_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'generation':args.generation,'bytes':len(data),'authority':'Diagnostic configuration only. Coordinator physical ownership and return admission required.'},indent=2)+'\n')
if __name__=='__main__':main()
