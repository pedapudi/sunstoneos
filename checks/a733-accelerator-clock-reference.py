#!/usr/bin/env python3
"""Run only GPU/NPU actual-provider cases for detected DCXO and REF24."""
import argparse, hashlib, json, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--providers',nargs='+',choices=['gpu','npu-platform','npu-pll'],default=['gpu','npu-platform','npu-pll'])
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
fixture=Path.home()/'.cache/sunstoneos/evidence/npu-parent-gates-20261016/npu-platform-reference.h'
def ref(path):return {'path':str(path),'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
plans={'gpu':['drivers/gpu/a733_gpu_resources.c','checks/a733-gpu-clock-reference.c'], 'npu-platform':['drivers/clock/a733_clock_leases.c','drivers/npu/a733_npu_platform.c','checks/a733-npu-platform-clock-reference.c'], 'npu-pll':['drivers/clock/a733_clock_leases.c','drivers/npu/a733_npu_pll.c','checks/a733-npu-pll-clock-reference.c']}
results=[]
for name in a.providers:
 sources=plans[name]
 command=['/usr/bin/clang-21','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-pthread','-Iinclude','-Ichecks','-I'+str(fixture.parent)]+sources+['-o',str(a.output/name)]
 for phase,cmd in [('build',command),('run',[str(a.output/name)])]:
  result=subprocess.run(cmd,cwd=ROOT,capture_output=True,text=True)
  (a.output/(name+'-'+phase+'.log')).write_text(result.stdout+result.stderr)
  assert result.returncode==0,(name,phase,result.stdout,result.stderr)
 results.append({'provider':name,'passed':True,'command':command,'sources':[ref(ROOT/s) for s in sources]})
 (a.output/'completed.json').write_text(json.dumps(results,indent=2)+'\n')
 print((a.output/(name+'-run.log')).read_text(),end='')
(a.output/'result.json').write_text(json.dumps({'passed':True,'physical_access':False,'providers':results,'reference_header':ref(ROOT/'include/a733_clock_reference.h'),'fixture':ref(fixture),'scope':'Only changed normalized clock cases using actual providers and established modeled callbacks; preserved fixture mains compile but never execute. No native image build or SD replay.'},indent=2)+'\n')
