#!/usr/bin/env python3
"""Run USB serial protocol, ownership and native broker host checks without hardware."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from harness import CLANG, SANITIZE, environment

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--tinyusb',type=Path,default=Path.home()/'.cache/sunstoneos/sources/tinyusb-d668b0abb7219b9ce324fa88d46604bb9e869973/src')
    p.add_argument('--services',type=Path)
    p.add_argument('--transport',type=Path)
    a=p.parse_args(); root=Path(__file__).resolve().parents[1]; out=a.output.resolve(); out.mkdir(parents=True,exist_ok=True)
    includes=[root/'checks/usb-serial',root/'include',out,a.tinyusb,a.tinyusb/'class/cdc']
    if a.services: includes.append(a.services/'include')
    if a.transport: includes.append(a.transport/'include')
    flags=[CLANG,'-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-g','-O1',*SANITIZE]+['-I'+str(x) for x in includes]
    steps=[]
    def run(command):
        r=subprocess.run([str(x) for x in command],capture_output=True,text=True,env=environment())
        steps.append({'command':[str(x) for x in command],'exit':r.returncode,'output':r.stdout+r.stderr})
        (out/'steps.json').write_text(json.dumps(steps,indent=2)+'\n')
        if r.returncode: raise SystemExit(r.stdout+r.stderr)
    run([sys.executable,root/'drivers/usb/host/serial-source.py','--source',a.tinyusb/'class/cdc/cdc_host.c','--output',out/'cdc-native.c'])
    run(flags+[root/'drivers/usb/host/serial.c',root/'checks/usb-serial/lifetime.c','-o',out/'lifetime'])
    run([out/'lifetime'])
    run(flags+['-ffunction-sections','-fdata-sections',root/'checks/usb-serial/protocol.c',root/'drivers/usb/host/serial.c',a.tinyusb/'common/tusb_fifo.c','-Wl,--gc-sections','-o',out/'protocol'])
    run([out/'protocol'])
    run(flags+[root/'checks/usb-serial/native.c',root/'drivers/usb/host/serial.c',root/'drivers/usb/host/serial-native.c','-o',out/'native'])
    run([out/'native'])
    inputs={str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest() for folder in ['checks/usb-serial','drivers/usb/host'] for f in (root/folder).glob('*') if f.is_file() and (folder.startswith('checks') or f.name.startswith('serial'))}
    inputs['include/sunstone_usb_serial.h']=hashlib.sha256((root/'include/sunstone_usb_serial.h').read_bytes()).hexdigest()
    for f in [Path(__file__),root/'docs/usb-serial-sources.json',a.tinyusb/'class/cdc/cdc_host.c',*[d/'sunstone_usb_service.h' for d in includes if (d/'sunstone_usb_service.h').exists()],*[d/'sunstone_usb_transport.h' for d in includes if (d/'sunstone_usb_transport.h').exists()]]:
        inputs[str(f)]=hashlib.sha256(f.read_bytes()).hexdigest()
    result={'result':'PASS','hardware_access':False,'inputs':inputs,'checks':['upstream descriptor open, baud and flow wire values','packet views and lifetime','native broker borrowed echo and quiescence']}
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n'); print(json.dumps(result,indent=2))
if __name__=='__main__': main()
