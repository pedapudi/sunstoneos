#!/usr/bin/env python3
"""Bluetooth first-readiness and HCI startup deadlines only."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import sys
from harness import CLANG, SANITIZE, Steps

ROOT=Path(__file__).resolve().parents[1]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for key in ('output','native-recipe','hci-reference','native-build'):p.add_argument('--'+key,type=Path,required=True)
    p.add_argument('--case',action='append',help='Run only these changed cases')
    p.add_argument('--resume',action='store_true');a=p.parse_args()
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=a.resume);steps=Steps(out,'commands.json')
    (out/'invocation.json').write_text(json.dumps([sys.executable,*sys.argv],indent=2)+'\n')
    recipe=json.loads(a.native_recipe.read_text())[0]
    common=[x for x in recipe[1:-2] if not x.endswith('.c') and not x.startswith('-Wl,')]
    common+=['-I'+str(a.hci_reference),'-fno-omit-frame-pointer']
    objects={}
    for name,source,extra in [('headset','checks/bluetooth-startup-deadline.c',['-DCHECK_HEADSET=1','-DSUNSTONE_BLUETOOTH_HEADSET=1','-I'+str(a.native_build/'os-image/bluetooth')]),
                              ('bluetooth','checks/bluetooth-startup-deadline.c',[]),
                              ('digest','drivers/storage/storage_digest.c',[]),
                              ('uart','drivers/uart/a733_uart.c',[]),
                              ('h4','drivers/bluetooth/bt_h4.c',[]),('hci','drivers/bluetooth/bt_hci.c',[])]:
        obj=out/(name+'.o');objects[name]=obj
        steps.command(name+'-compile',[CLANG,*common,*extra,'-MMD','-MF',out/(name+'.d'),'-c',source,'-o',obj])
    groups={'headset':['headset'],'bluetooth':['bluetooth','digest','uart','h4','hci']}
    cases={'headset':['schedule'],'bluetooth':['pending-before','pending-at','ready-before','ready-at',
           'ready-after','ready-cross','clock-failure','zero-generation','resident-early','resident-fixed',
           'uart-wait','wake-wait','module-wait','hci-wait','generation-change','own-late','resident-pending']}
    if a.case:
        assert set(a.case)<=set(sum(cases.values(),[]))
        cases={mode:[name for name in names if name in a.case] for mode,names in cases.items()}
    for mode,names in groups.items():
        if not cases[mode]:continue
        steps.command(mode+'-link',[CLANG,*SANITIZE,'-Wl,--gc-sections',*[objects[n] for n in names],'-o',out/mode])
        for name in cases[mode]:steps.command(mode+'-'+name,[out/mode,name])
    inputs={Path(__file__).resolve(),a.native_recipe.resolve(),CLANG.resolve(),ROOT/'checks/harness.py'}
    for dep in out.glob('*.d'):
        inputs.update(Path(x).resolve() for x in shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1]))
    (out/'inputs.json').write_text(json.dumps({str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in sorted(inputs)},indent=2)+'\n')
    result={'passed':True,'physical_access':False,'cases':cases,
            'scope':'Actual Bluetooth startup and headset deadline code, UART/H4/HCI; module providers/readiness modeled. Earlier protocol bodies not executed.'}
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))

if __name__=='__main__':main()
