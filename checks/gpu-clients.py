#!/usr/bin/env python3
"""Compile actual GPU client API with modeled task, queue and service effects."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import host_rtos
from harness import environment
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--baseline',default='9f7c1b00')
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
link=['-Wl,--wrap=ulTaskGenericNotifyTake']
def build(source,name,baseline=False):
    target=host_rtos.build(a.output,['checks/gpu-clients.c',source,'apps/os/gpu-share.c'],name,
        defines=['GPU_CLIENT_BASELINE=1'] if baseline else [],includes=[ROOT/'apps/os'],link=link,
        flags=['-w'] if source!=ROOT/'drivers/gpu/gpu_queue.c' else [])
    return host_rtos.run(target)
old=subprocess.check_output(['git','show',a.baseline+':drivers/gpu/gpu_queue.c'],cwd=ROOT)
(a.output/'baseline-client.c').write_bytes(old)
failed=build(a.output/'baseline-client.c','baseline',True)
assert failed.returncode!=0 and 'q.final_close' in failed.stderr,failed.stderr
fixed=build(ROOT/'drivers/gpu/gpu_queue.c','clients')
assert fixed.returncode==0,fixed.stdout+fixed.stderr
controls={}
for name,before,after in (('foreign-task-joins','return expected && expected==xTaskGetCurrentTaskHandle();','return expected != 0;'),):
    mutant=build(host_rtos.mutated(a.output,'drivers/gpu/gpu_queue.c',before,after,name+'.c'),'control-'+name)
    assert mutant.returncode!=0,('control passed',name);controls[name]='fails'
(a.output/'result.json').write_text(json.dumps({'passed':True,'baseline_revision':a.baseline,
    'baseline_sha256':hashlib.sha256(old).hexdigest(),'baseline_failure':failed.stderr.strip(),
    'negative_controls':controls,'client_sha256':hashlib.sha256((ROOT/'drivers/gpu/gpu_queue.c').read_bytes()).hexdigest(),
    'checks':fixed.stdout.strip(),'physical_access':False,
    'scope':'Actual client API and pure app admission, modeled scheduler/service/worker; no GPU execution or worker scheduling claim.'},indent=2)+'\n')
print(fixed.stdout,end='')
