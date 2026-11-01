#!/usr/bin/env python3
"""Check precise deadline and service health races on the host.

Optional --port-build reuses a configured cached-SMP build to link and execute
an instruction-only workload with its actual kernel, port, IRQ entry and task
migration. MMIO, architectural timer and GIC are modeled; this is no physical
latency or hardware qualification.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import shlex
import struct
import subprocess
from harness import CLANG, SANITIZE, environment, save
ROOT=Path(__file__).resolve().parents[1]


def native(build,out):
    subprocess.run(['cmake','--build',str(build),'--target','a733-smp-cached-diagnostic','-j2'],
                   check=True,env=environment(),stdout=(out/'port-build.log').open('w'),stderr=subprocess.STDOUT)
    commands=json.loads((build/'compile_commands.json').read_text())
    selected={}
    suffixes=('platform/a733-smp-cached/diagnostic.c','platform/a733-smp-cached/platform.c',
              'platform/a733-smp-cached/continue.c')
    for suffix in suffixes:
        matches=[c for c in commands if c['file'].endswith(suffix) and
                 'a733-smp-cached-diagnostic.dir' in c['command']]
        assert len(matches)==1,(suffix,len(matches));selected[suffix]=matches[0]
    replacements={}
    added=[]
    for suffix,c in selected.items():
        source=ROOT/'checks/deadline-port.c' if suffix.endswith('/diagnostic.c') else Path(c['file'])
        obj=out/(source.name+'.obj')
        args=shlex.split(c['command']);output=args.index('-o')+1
        replacements[args[output]]=str(obj);args[output]=str(obj)
        args[-1]=str(source);args[1:1]=['-DSUNSTONE_TASK_DEADLINES=1','-DINCLUDE_xTaskGetCurrentTaskHandle=1']
        subprocess.run(args,cwd=c['directory'],check=True,env=environment())
    c=selected[suffixes[0]]
    for name in ('deadline.c','deadline-a733.c'):
        obj=out/(name+'.obj');args=shlex.split(c['command']);args[args.index('-o')+1]=str(obj)
        args[-1]=str(ROOT/'runtime'/name)
        args[1:1]=['-DSUNSTONE_TASK_DEADLINES=1','-DINCLUDE_xTaskGetCurrentTaskHandle=1']
        subprocess.run(args,cwd=c['directory'],check=True,env=environment());added.append(str(obj))
    args=shlex.split((build/'CMakeFiles/a733-smp-cached-diagnostic.dir/link.txt').read_text())
    args=[replacements.get(arg,arg) for arg in args]
    args[args.index('-o')+1]=str(out/'deadline-port.elf');args+=added
    subprocess.run(args,cwd=build,check=True,env=environment())
    spec=importlib.util.spec_from_file_location('deadline_smp_model',ROOT/'checks/a733-smp-cached.py')
    model=importlib.util.module_from_spec(spec);spec.loader.exec_module(model)
    class Machine(model.Machine):
        def timer_level(self,c):
            super().timer_level(c)
            r=self.regs[c]
            if r.get('cntv_ctl_el0',0)&3==1 and self.clock>=r.get('cntv_cval_el0',model.MASK):self.pending[c].add(27)
            else:self.pending[c].discard(27)
    m=Machine(out/'deadline-port.elf',count=True)
    result=m.run()
    result['deadline_records']=[list(struct.unpack('<16Q',m.read(m.symbols['a733_cached_records']+c*128,128))) for c in range(8)]
    result['deadline_ready']=int.from_bytes(m.read(m.symbols['ready'],4),'little')
    (out/'port-state.json').write_text(json.dumps(result,indent=2)+'\n')
    assert result['report'][4]==0,result
    assert result['task_cores']==list(range(8)),result
    complete=m.symbols['complete']
    assert int.from_bytes(m.read(complete,4),'little')==65535
    return {'passed':True,'completed_owners':16,'modeled_counter_ticks':m.clock,
            'limits':'Actual selected SMP port, tasks, notifications and migration; modeled timer/GIC/shared RAM; no physical timing.'}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--port-build',type=Path)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    exe=args.output/'deadline-host'
    subprocess.run([str(CLANG),'-std=c11','-Wall','-Wextra','-Werror',*SANITIZE,
                    '-I'+str(ROOT/'checks/deadline-host'),'-I'+str(ROOT/'include'),
                    str(ROOT/'checks/deadline-host.c'),'-o',str(exe)],check=True,env=environment())
    host=subprocess.check_output([str(exe)],text=True,env=environment())
    result={'passed':True,'host':host,'limits':'Deterministic owner/notification model; actual service supervisor and common deadline C.'}
    if args.port_build:result['selected_port']=native(args.port_build,args.output)
    save(args.output/'result.json',result);print(host,end='')


if __name__=='__main__':main()
