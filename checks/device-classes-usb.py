#!/usr/bin/env python3
"""Check the shared UVC camera interface and existing protocol with sanitizers; no hardware."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from harness import CLANG, SANITIZE, environment

CHECK_INPUTS = ['include/sunstone_camera.h', 'include/sunstone_usb_uvc.h',
                'include/sunstone_uvc_camera.h', 'drivers/usb/host/uvc.c',
                'drivers/usb/host/uvc-camera.c']

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[1]
    out=args.output.resolve()
    out.mkdir(parents=True,exist_ok=False)
    flags=[str(CLANG),'-std=c11','-O1','-g','-Wall','-Wextra','-Werror',*SANITIZE,
           '-I'+str(root/'include'),'-I'+str(root/'checks')]
    steps=[]
    for name,units in [('protocol',['checks/usb-uvc.c','drivers/usb/host/uvc.c']),
                       ('interface',['checks/device-classes-usb.c',*CHECK_INPUTS[-2:]])]:
        commands=[flags+[str(root/unit) for unit in units]+['-o',str(out/name)],
                  [str(out/name)]]
        for command in commands:
            result=subprocess.run(command,capture_output=True,text=True,env=environment())
            steps.append({'command':command,'exit':result.returncode,'output':result.stdout+result.stderr})
            (out/'steps.json').write_text(json.dumps(steps,indent=2)+'\n')
            if result.returncode:
                raise SystemExit(result.stdout+result.stderr)
    paths=[*CHECK_INPUTS,'checks/device-classes-usb.c','checks/usb-uvc.c',
           'checks/usb-uvc-c270.h','checks/usb-uvc-capture.h','docs/usb-uvc-sources.json',
           'checks/device-classes-usb.py']
    record={'result':'PASS','hardware_access':False,
            'inputs':{path:hashlib.sha256((root/path).read_bytes()).hexdigest() for path in paths},
            'scope':'Shared camera lifecycle and retained UVC spans; existing UVC protocol and public descriptor facts'}
    (out/'result.json').write_text(json.dumps(record,indent=2)+'\n')
    print(record['result']+': '+record['scope'])

if __name__=='__main__':
    main()
