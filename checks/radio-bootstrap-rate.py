#!/usr/bin/env python3
"""Changed firmware-owner construction and bounded Bluetooth polling only."""
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
    for key in ('output','native-recipe','hci-reference'):p.add_argument('--'+key,type=Path,required=True)
    p.add_argument('--resume',action='store_true');a=p.parse_args()
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=a.resume);steps=Steps(out,'commands.json')
    (out/'invocation.json').write_text(json.dumps([sys.executable,*sys.argv],indent=2)+'\n')
    recipe=json.loads(a.native_recipe.read_text())[0]
    common=[x for x in recipe[1:-2] if not x.endswith('.c') and not x.startswith('-Wl,')]
    common+=['-I'+str(a.hci_reference),'-fno-omit-frame-pointer']
    objects={}
    for name,source,extra in [('board','checks/radio-bootstrap-rate.c',['-DCHECK_BOARD=1']),
                              ('bluetooth','checks/radio-bootstrap-rate.c',[]),
                              ('digest','drivers/storage/storage_digest.c',[]),
                              ('uart','drivers/uart/a733_uart.c',[]),
                              ('h4','drivers/bluetooth/bt_h4.c',[]),('hci','drivers/bluetooth/bt_hci.c',[])]:
        obj=out/(name+'.o');objects[name]=obj
        steps.command(name+'-compile',[CLANG,*common,*extra,'-MMD','-MF',out/(name+'.d'),'-c',source,'-o',obj])
    groups={'board':['board','digest'],'bluetooth':['bluetooth','digest','uart','h4','hci']}
    cases={'board':['requested','refused'],'bluetooth':['constructor','constructor-refused','waiting',
           'progress-0','progress-1','progress-2','progress-3','busy','retained','invalid','clock-failure',
           'deadline','ready-boundary','runtime']}
    for mode,names in groups.items():
        steps.command(mode+'-link',[CLANG,*SANITIZE,'-Wl,--gc-sections',*[objects[n] for n in names],'-o',out/mode])
        for name in cases[mode]:steps.command(mode+'-'+name,[out/mode,name])
    inputs={Path(__file__).resolve(),a.native_recipe.resolve(),CLANG.resolve(),ROOT/'checks/harness.py'}
    for dep in out.glob('*.d'):
        inputs.update(Path(x).resolve() for x in shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1]))
    (out/'inputs.json').write_text(json.dumps({str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in sorted(inputs)},indent=2)+'\n')
    result={'passed':True,'physical_access':False,'cases':cases,
            'scope':'Actual constructor and bootstrap polling code; native providers and session outcomes modeled. Earlier application bodies not executed.'}
    (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))

if __name__=='__main__':main()
