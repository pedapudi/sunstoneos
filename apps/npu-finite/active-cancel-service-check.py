#!/usr/bin/env python3
"""Build and run the actual FreeRTOS cancellation worker with modeled device effects."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'checks'))
import harness

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-build',type=Path,required=True)
    parser.add_argument('--evidence',type=Path,required=True)
    parser.add_argument('--cases',default='0,1,2,3,4,5,6,7,8,9,10')
    parser.add_argument('--qemu',type=Path,default=harness.CACHE/'host/usr/bin/qemu-system-aarch64')
    args=parser.parse_args();args.evidence.mkdir(parents=True,exist_ok=False)
    entries=json.loads((args.baseline_build/'compile_commands.json').read_text())
    selected=[x for x in entries if 'npu-service-irq-model.dir/' in x['command']]
    assert selected
    old_root=next(x['file'].removesuffix('/drivers/npu/npu_service.c') for x in selected if x['file'].endswith('/drivers/npu/npu_service.c'))
    outputs={};commands=[]
    for index,entry in enumerate(selected):
        original=shlex.split(entry['command']);command=[x.replace(old_root,str(ROOT)) for x in original]
        obj=args.evidence/(str(index)+'.o');outputs[original[original.index('-o')+1]]=str(obj)
        command[command.index('-o')+1]=str(obj)
        if entry['file'].endswith('/checks/npu-interrupt-service.c'):
            command[command.index('-c')+1]=str(ROOT/'checks/npu-active-cancel-service.c')
        if entry['file'].endswith('/npu-service-port.c'):
            command+=['-Dnpu_service_task_privileged=npu_active_cancel_task_privileged']
        command+=['-DSUNSTONE_NPU_ACTIVE_CANCEL=1']
        commands.append(command)
        result=subprocess.run(command,cwd=entry['directory'],capture_output=True,text=True,env=harness.environment())
        (args.evidence/(str(index)+'.log')).write_text(result.stdout+result.stderr)
        assert result.returncode==0,result.stderr
    original=shlex.split((args.baseline_build/'CMakeFiles/npu-service-irq-model.dir/link.txt').read_text())
    elf=args.evidence/'fixture.elf'
    link=[outputs.get(x,x.replace(old_root,str(ROOT))) for x in original]
    link[link.index('-o')+1]=str(elf)
    link=[('-Wl,-Map,'+str(args.evidence/'fixture.map')) if x.startswith('-Wl,-Map,') else x for x in link]
    commands.append(link)
    result=subprocess.run(link,cwd=args.baseline_build,capture_output=True,text=True,env=harness.environment())
    (args.evidence/'link.log').write_text(result.stdout+result.stderr)
    assert result.returncode==0,result.stderr
    labels=('nominal-controller-recovery','active-proof-refused','ACK-without-idle','cancel-ACK-timeout',
            'stale-IRQ-refused','recovery-checkpoint-refused','missing-recovery-bootstrap-IRQ',
            'unreaped-output-retained','stale-cancel-ACK','fetch-outside-owned-command','expired-activity-budget')
    report=dict(physical_access=False,board_image=False,image_sha256=hashlib.sha256(elf.read_bytes()).hexdigest(),
        commands=commands,cases={},scope='Actual FreeRTOS worker, controller, native memory and cache callbacks. Device registers, counter progression and ISR notification delivery modeled; no physical GIC or numerical execution evidence.')
    for case in map(int,args.cases.split(',')):
        command=[str(args.qemu),'-machine','virt,gic-version=3,secure=off,virtualization=off','-cpu','cortex-a55',
            '-smp','2','-m','128M','-nodefaults','-display','none','-nographic','-serial','stdio','-monitor','none',
            '-semihosting-config','enable=on,target=native','-kernel',str(elf),'-device',f'loader,addr=0x47fff000,data={case},data-len=4']
        result=subprocess.run(command,capture_output=True,text=True,timeout=10)
        (args.evidence/(str(case)+'-'+labels[case]+'.log')).write_text(result.stdout+result.stderr)
        passed=result.returncode==0 and 'PASS active NPU cancel' in result.stdout
        report['cases'][labels[case]]=dict(passed=passed,exit=result.returncode,command=command)
        (args.evidence/'result.json').write_text(json.dumps(report,indent=2)+'\n')
        assert passed,(labels[case],result.stdout,result.stderr)
    print('PASS active cancellation FreeRTOS worker cases')

if __name__=='__main__':main()
