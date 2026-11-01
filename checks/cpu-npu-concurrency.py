#!/usr/bin/env python3
"""Run the actual GEMM/application/OS observer adapter with host task/device effects.

The hardware busy duration, IRQ effects and SDK transport are modeled. The
linked integration check separately executes actual completion and terminal
owners. Receipt mutations test host refusal without repeating completed work.
"""
import argparse
import importlib.util
import json
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
from harness import environment, save
CHECK_INPUTS = ['apps/cpu-npu-concurrency/main.c', 'apps/cpu-npu-concurrency/gemm.h', 'apps/cpu-npu-concurrency/receipt.h', 'apps/cpu-npu-concurrency/host.py', 'apps/cpu-npu-concurrency/vectors.py', 'apps/os/npu-observe.c', 'apps/os/application.c', 'apps/os/cpu-return.c', 'apps/os/npu.c', 'apps/os/npu-irq.c', 'apps/os/cpu-return/terminal.patch', 'apps/os/cpu-return/thermal.patch', 'apps/os/cpu-return-host.py', 'drivers/npu/npu_service.c', 'drivers/npu/a733_npu_runtime.c', 'platform/a733/npu_irq.c', 'include/sunstone_app.h', 'include/npu_service.h', 'include/a733_npu_irq.h', 'cmake/sunstone-os-core.cmake', 'cmake/a733-os-cpu-return.cmake', 'checks/cpu-npu-concurrency/producer.c', 'checks/cpu-npu-concurrency/FreeRTOS.h', 'checks/cpu-npu-concurrency/task.h', 'checks/cpu-npu-concurrency/npu.h', 'checks/cpu-npu-concurrency/a733_npu_irq.h']
ROOT = Path(__file__).resolve().parents[1]
APP = ROOT/'apps/cpu-npu-concurrency'
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--cc',default='/usr/bin/cc')
p.add_argument('--disabled-only',action='store_true')
p.add_argument('--poison-only',action='store_true')
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
spec=importlib.util.spec_from_file_location('cpu_npu_receipt',APP/'host.py')
host=importlib.util.module_from_spec(spec);spec.loader.exec_module(host)
commands=[];results=[]
def run(command,expected=0):
    commands.append(list(map(str,command)))
    r=subprocess.run(command,capture_output=True,text=True,env=environment(),timeout=20)
    assert r.returncode==expected,(command,r.returncode,r.stdout,r.stderr)
    return r
adapter=a.out/'npu-observe.c';shutil.copyfile(ROOT/'apps/os/npu-observe.c',adapter)
disabled=a.out/'observer-disabled.c'
disabled.write_text('#include "sunstone_app.h"\nstatic int callback(void *c,unsigned e,const struct ss_npu_observation *s){(void)c;(void)e;(void)s;return 1;}\nint main(void){if(ss_npu_observe(callback,0)!=0)return 1;ss_npu_observer_kick();return 0;}\n')
run([a.cc,'-std=c11','-O2','-I'+str(ROOT/'checks/cpu-npu-concurrency'),'-I'+str(ROOT/'include'),
     str(disabled),str(adapter),'-o',str(a.out/'observer-disabled')])
run([str(a.out/'observer-disabled')])
results.append(dict(case='observer-disabled-unsupported-no-register-or-task-access',passed=True))
def build(label,mode,source=APP/'main.c'):
    output=a.out/label
    command=[a.cc,'-std=c11','-O2','-ffp-contract=off','-pthread',
        '-I'+str(ROOT/'checks/cpu-npu-concurrency'),'-I'+str(ROOT/'include'),'-I'+str(APP)]
    if mode:command+=['-DSUNSTONE_A733_OS_NPU=1','-DSUNSTONE_A733_OS_NPU_OBSERVE=1']
    run([*command,str(source),*([str(adapter)] if mode else []),str(ROOT/'checks/cpu-npu-concurrency/producer.c'),'-o',str(output)])
    return output
if not a.disabled_only:
 if not a.poison_only:
  for mode,label in [(0,'control'),(1,'combined')]:
      executable=build(label,mode);payload=a.out/(label+'.bin')
      run([str(executable),'actual',str(payload)])
      result=host.decode(payload.read_bytes(),mode=mode,model_index=0 if mode else None,reference=b'\0\xff' if mode else None)
      results.append(dict(case=label,passed=True,receipt=result))
      if not mode:
          run([str(executable),'startup-throttle',str(a.out/'startup-throttle.bin')])
          host.decode((a.out/'startup-throttle.bin').read_bytes(),mode=0)
          results.append(dict(case='throttle-before-start-is-not-a-fault',passed=True))
          run([str(executable),'throttle',str(a.out/'control-throttle.bin')])
          assert host.decode_failure((a.out/'control-throttle.bin').read_bytes())['stage']==0x99
          results.append(dict(case='control-throttle-failure-returns',passed=True))
      if mode:
          for fault in ['early-complete','changed-ticket','changed-token','frozen-cpu1','wrong-core','throttle']:
              run([str(executable),fault,str(a.out/(fault+'.bin'))])
              record=host.decode_failure((a.out/(fault+'.bin')).read_bytes())
              assert record['ack_mask']==255 and record['sdk_closed']==1,(fault,record)
              if fault=='wrong-core':assert (record['stage'],record['core'],record['observed'])==(0x9a,3,2),record
              if fault=='throttle':assert record['stage']==0x99 and record['thermal']['causes']==['publication is stale'] and record['thermal']['millicelsius'][2]==-3000 and record['thermal']['freshness_margin_us']==-1000,record
              try:host.decode((a.out/(fault+'.bin')).read_bytes(),mode=1,model_index=0,reference=b'\0\xff')
              except ValueError as error:assert str(error).startswith('Check failed: ')
              else:raise AssertionError('failure record accepted as a receipt')
              results.append(dict(case=fault,passed=True,failure=record))
          valid=bytearray(payload.read_bytes())
          mutations=[('wrong-id',0x80,99),('wrong-digest',0x90,1),('zero-work',0x88,0),
              ('missing-ack',0x80+56,0),('poisoned-output',0x280+264,0xa5a5),
              ('no-arithmetic-counter',0x280+22*8,2**40),('mailbox-only',0x68,0),
              ('early-complete-delayed-irq',0x280+8*8,0),('wrong-model',0x280+3*8,9),
              ('backward-next-trigger',0x280+272+4*8,1),('backward-next-counter',0x280+272+14*8,0)]
          for name,offset,value in mutations:
              raw=bytearray(valid);struct.pack_into('<Q',raw,offset,value)
              try:host.decode(raw,mode=1,model_index=0,reference=b'\0\xff')
              except ValueError as error:results.append(dict(case=name,passed=True,refusal=str(error)))
              else:raise AssertionError(('accepted malformed receipt',name))
          for name,raw in [('short',valid[:-1]),('long',valid+b'\0')]:
              try:host.decode(raw,mode=1,model_index=0,reference=b'\0\xff')
              except ValueError:results.append(dict(case=name,passed=True))
              else:raise AssertionError(name)

# Mutate only the compiled test copy. Skipping output computation leaves the
 # real poison and real cell validator in place, which must fail before count.
 for label,change in [('poison',lambda s:s.replace('m->c[r][s]=sum;','(void)sum;')),
                      ('bad-cell',lambda s:s.replace('sum=sum+m->a[r][k]*m->b[k][s]','sum=sum+m->a[r][k]*m->b[k][s]+1.0f'))]:
     if a.poison_only and label!='poison':continue
     directory=a.out/label;directory.mkdir()
     source=directory/'main.c';source.write_text((APP/'main.c').read_text())
     (directory/'gemm.h').write_text(change((APP/'gemm.h').read_text()))
     executable=build(label+'-producer',0,source)
     run([str(executable),'actual',str(a.out/(label+'.bin'))])
     record=host.decode_failure((a.out/(label+'.bin')).read_bytes())
     assert record['stage']==0x82 and record['sdk_closed']==0,record
     results.append(dict(case=label+'-native-cell-check',passed=True,failure=record))
save(a.out/'results.json',dict(physical_access=False,model_boundary=__doc__,cases=results,commands=commands,
 sources={name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in CHECK_INPUTS}))
print('PASS',len(results),'CPU/NPU producer, observer and receipt cases')
