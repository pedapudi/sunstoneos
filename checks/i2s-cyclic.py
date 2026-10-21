#!/usr/bin/env python3
"""Run selected hardware-linked cyclic I2S/provider ownership cases in fresh QEMU processes."""
import argparse,hashlib,json,subprocess
from pathlib import Path
from elftools.elf.elffile import ELFFile
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--elf',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
p.add_argument('--cases',default='0,3,4,5,6,7,8,9')
p.add_argument('--qemu',type=Path,default=Path.home()/'.cache/sunstoneos/host/usr/bin/qemu-system-aarch64')
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True);result=a.output/'cyclic-result.json'
values=json.loads(result.read_text()) if result.exists() else {'physical_access':False,'cases':{}}
image_sha256=hashlib.sha256(a.elf.read_bytes()).hexdigest()
for name, saved in values['cases'].items():
    if saved.get('passed') and saved.get('image_sha256') != image_sha256:
        p.error(f'existing passed case {name} belongs to another image; use a separate output directory')
labels={0:'duplex-rings-three-wraps-final-drain-count',3:'RX-overrun-before-copy',4:'RX-reused-during-first-invalidate',5:'RX-reused-after-copy-before-postcheck',6:'cursor-transition-defers-copy',7:'held-staging-survives-raw-overrun',8:'cancel-priming-preserves-client-lease',9:'TX-coalesced-multiple-wrap-progress',10:'untrusted-initial-counter-retained',11:'start-demand-timestamp-precedes-delayed-readback',12:'unobserved-counter-history-retained'}
with a.elf.open('rb') as stream:
    elf=ELFFile(stream)
    assert not any(s['p_vaddr']<=0x47fff000<s['p_vaddr']+s['p_memsz'] for s in elf.iter_segments() if s['p_type']=='PT_LOAD')
for case in map(int,a.cases.split(',')):
    name=labels[case]
    if values['cases'].get(name,{}).get('passed'):continue
    cmd=[str(a.qemu),'-machine','virt,gic-version=3,secure=off,virtualization=off','-cpu','cortex-a55',
         '-smp','2','-m','128M','-nographic','-monitor','none','-semihosting-config','enable=on,target=native',
         '-kernel',str(a.elf),'-device',f'loader,addr=0x47fff000,data={case},data-len=4']
    run=subprocess.run(cmd,capture_output=True,text=True,timeout=15)
    log=a.output/f'{case}-{name}.log';log.write_text(run.stdout+run.stderr)
    values['cases'][name]={'passed':run.returncode==0 and 'PASS cyclic I2S' in run.stdout,'exit':run.returncode,
        'command':cmd,'log':log.name,'image_sha256':image_sha256}
    result.write_text(json.dumps(values,indent=2)+'\n')
    assert values['cases'][name]['passed'],(name,run.stdout,run.stderr)
print('PASS selected actual FreeRTOS cyclic I2S/provider cases; device/DMA effects modeled')
