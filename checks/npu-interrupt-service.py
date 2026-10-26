#!/usr/bin/env python3
"""Run selected changed IRQ service/native-memory cases in fresh QEMU processes."""
import argparse,hashlib,json,subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--elf',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
p.add_argument('--cases',default=','.join(map(str,range(20))))
p.add_argument('--qemu',type=Path,default=Path.home()/'.cache/sunstoneos/host/usr/bin/qemu-system-aarch64')
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True);result=a.output/'service-result.json'
values=json.loads(result.read_text()) if result.exists() else {'physical_access':False,'cases':{}}
labels=['notified-bootstrap-two-jobs-deferred-stop','event-without-notification-times-out','ISR-mask-failure-retained',
        'notified-device-error-retained','task-mask-failure-retained','rearm-failure-retained',
        'stop-failure-keeps-owner','deferred-mask-still-expires','cancel-without-event-IRQ',
        'route-and-worker-on-core-1','initial-mask-pending-expires','rearm-pending-expires',
        'stop-pending-expires','shared-stop-pending-expires','runtime-stop-pending-expires',
        'independent-transient-rearm-waits','stationary-backend-clock-mask-expires',
        'runtime-deferred-stop-completes','shared-deferred-stop-completes',
        'notification-ends-pending-rearm-episode']
with a.elf.open('rb') as stream:
    elf=ELFFile(stream)
    assert not any(s['p_vaddr']<=0x47fff000<s['p_vaddr']+s['p_memsz'] for s in elf.iter_segments() if s['p_type']=='PT_LOAD')
for case in map(int,a.cases.split(',')):
    name=labels[case]
    if values['cases'].get(name,{}).get('passed'):continue
    cmd=[str(a.qemu),'-machine','virt,gic-version=3,secure=off,virtualization=off','-cpu','cortex-a55',
         '-smp','2','-m','128M','-nographic','-monitor','none','-semihosting-config','enable=on,target=native',
         '-kernel',str(a.elf),'-device',f'loader,addr=0x47fff000,data={case},data-len=4']
    run=subprocess.run(cmd,capture_output=True,text=True,timeout=10)
    log=a.output/f'{case}-{name}.log';log.write_text(run.stdout+run.stderr)
    values['cases'][name]={'passed':run.returncode==0 and 'PASS NPU IRQ' in run.stdout,'exit':run.returncode,
        'command':cmd,'log':log.name,'image_sha256':hashlib.sha256(a.elf.read_bytes()).hexdigest()}
    result.write_text(json.dumps(values,indent=2)+'\n')
    assert values['cases'][name]['passed'],(name,run.stdout,run.stderr)
print('PASS selected actual FreeRTOS IRQ service/native memory cases; device events and IRQ delivery modeled')
