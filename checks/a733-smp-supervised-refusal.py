#!/usr/bin/env python3
"""Check only changed ROM-frequency admission and clean-refusal continuation.

Reuses the preserved native image and prior successful component evidence.
All transactions are software fixtures; this script never discovers USB devices.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--prior-evidence',type=Path,required=True)
    p.add_argument('--evidence',type=Path,required=True)
    p.add_argument('--clang',default='/usr/lib/llvm-21/bin/clang')
    p.add_argument('--lld',type=Path,default=Path.home()/'.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld')
    a=p.parse_args();e=a.evidence.resolve();e.mkdir(parents=True,exist_ok=True)
    spec=importlib.util.spec_from_file_location('supervised_host',ROOT/'tools/check-a733-smp-supervised.py')
    host=importlib.util.module_from_spec(spec);spec.loader.exec_module(host)
    prior=a.prior_evidence.resolve();prior_result=prior/'checks/results.json'
    previous=json.loads(prior_result.read_text());assert previous['result'].startswith('PASS:')
    image=prior/'build/a733-smp-supervised.elf';assert host.digest(image)==previous['image_sha256']
    state_path=e/'results.json'
    state=json.loads(state_path.read_text()) if state_path.exists() else {'physical_access':False,'steps':{},'cases':{}}
    def ref(path):return {'path':str(path.resolve()),'sha256':host.digest(path)}
    sources=[Path(__file__),ROOT/'platform/a733-smp-supervised/launch.c',ROOT/'tools/check-a733-smp-supervised.py',
        ROOT/'checks/a733-smp-supervised-launch.py',ROOT/'tools/check-a733-smp.py',ROOT/'tools/check-a733-smp-observe.py']
    identity={str(path):host.digest(path) for path in sources}
    state.setdefault('invocations',[]).append(identity)
    snapshots=e/f'inputs-{len(state["invocations"])}'
    for path in sources:
        destination=snapshots/path.relative_to(ROOT);destination.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,destination)
    state['reused_evidence']=ref(prior_result);state['native_image']=ref(image)
    def save():host.save_json(state_path,state)
    def run(command,name):
        command=list(map(str,command));result=subprocess.run(command,capture_output=True,text=True)
        ordinal=len(state.setdefault('commands',[]));(e/f'{ordinal:02}-{name}.log').write_text(result.stdout+result.stderr)
        state['commands'].append(command);save()
        if result.returncode:raise RuntimeError('Failed '+name+'; preserve completed steps and resume only unfinished work.')
    bundle=e/'fixture-bundle'
    if 'launcher' not in state['steps']:
        if not bundle.exists():shutil.copytree(prior/'checks/fixture-bundle',bundle)
        shutil.copy2(ROOT/'platform/a733-smp-supervised/launch.c',bundle/'launch.c')
        flags=['--target=arm-none-eabi','-march=armv8-a','-marm','-mfloat-abi=soft','-ffreestanding',
            '-fno-builtin','-fno-stack-protector','-fno-unwind-tables','-fno-asynchronous-unwind-tables',
            '-Os','-g','-Wall','-Wextra','-Werror','-DA733_SMP_CONTRACT_MAGIC=0x4153373353554333ULL','-I'+str(bundle)]
        # Only the SRAM launcher is rebuilt. The exact AArch64 ELF stays intact.
        for src,obj in [('launch.S','entry.o'),('launch.c','launch.o')]:
            run([a.clang,*flags,'-c',bundle/src,'-o',bundle/obj],obj)
        run([a.lld,'-T',bundle/'launch.ld',bundle/'entry.o',bundle/'launch.o','-o',bundle/'launch.elf'],'launcher-link')
        run(['/usr/lib/llvm-21/bin/llvm-objcopy','-O','binary',bundle/'launch.elf',bundle/'launch.bin'],'launcher-binary')
        run([sys.executable,ROOT/'checks/a733-smp-supervised-launch.py','--bundle',bundle,
             '--frequency-only','--output',e/'frequency-cases.json'],'frequency-cases')
        state['steps']['launcher']={'result':'pass','cases':2,'launcher':ref(bundle/'launch.bin'),
            'source':ref(ROOT/'platform/a733-smp-supervised/launch.c')};save()
    else:
        assert state['steps']['launcher']['source']['sha256']==host.digest(ROOT/'platform/a733-smp-supervised/launch.c')
        assert state['steps']['launcher']['launcher']['sha256']==host.digest(bundle/'launch.bin')
    # Execute the actual existing report validator and sealed restoration helper.
    # Source fixtures model one original-ROM instance, never a native/USB device.
    def refusal_case(name,*,report_reason=13,stale=False,watchdog=0,error_at=None,same=True,failed=False,repeat=False):
        if name in state['cases']:return
        case=e/name;case.mkdir(exist_ok=True)
        prepared={'epoch':31,'generation':37,'lease':41,'_backup_hashes':{}}
        for label,address,length in host.base.REGIONS:
            path=case/f'before-{label}.bin';path.write_bytes(bytes([length%251])*length)
            prepared['_backup_hashes'][label]=host.digest(path)
        words=[report_reason,0,0,30 if stale else 31,37,41,0x4153373352454631]+[0]*9
        report=struct.pack('<16Q',*words);events=[]
        record={'launch_attempted':True,'scheduler_result':'unobserved','complete':False,'transport_failed':False}
        def transaction(*arguments,pass_fds=()):
            events.append(tuple(map(str,arguments)))
            kind=arguments[0]
            if error_at==kind:
                record['transport_failed']=True;raise OSError('Modeled transport failure at '+kind)
            if kind=='read':
                assert arguments[1:3]==('0x52d00',128);Path(arguments[3]).write_bytes(report);return ''
            if kind=='readl':assert arguments==('readl','0x02050014');return hex(watchdog)
            assert kind=='write' and pass_fds and arguments[3]=='read'
            source=Path(arguments[2]).read_bytes();assert len(source)==arguments[5]
            Path(arguments[6]).write_bytes(source);return ''
        expected_clean=same and not failed and not stale and report_reason==13 and not watchdog and error_at is None
        try:
            host.probe_clean_refusal(transaction,case,prepared,record,lambda:None,same_instance=same,transport_failed=failed)
        except (OSError,ValueError) as error:
            assert not expected_clean,(name,error)
            assert not record.get('clean_refusal') and not record.get('ownership_release_permitted')
            retained=[]
            host.base.stop_after_failure(record,lambda:None,lambda:retained.append('quarantine'),error,hold=lambda:retained.append('hold'))
            assert retained==['quarantine','hold']
        else:
            assert expected_clean and record.get('sram_restored') and record.get('rom_usable')
            retained=[]
            host.finish_after_observation(record,lambda:None,lambda:retained.append('quarantine'),lambda:retained.append('hold'))
            assert not retained and record['ownership_release_permitted'] and not record['manual_reconnect_required']
            assert len([v for v in events if v[0]=='write'])==len(host.base.REGIONS)
            if repeat:
                before=len(events)
                try:host.probe_clean_refusal(transaction,case,prepared,record,lambda:None,same_instance=True,transport_failed=False)
                except ValueError:pass
                else:raise AssertionError('Repeated report probe accepted')
                assert len(events)==before
        assert sum(v[0]=='read' for v in events)==(1 if same and not failed else 0)
        if stale or report_reason==12 or error_at=='read':assert len(events)==1
        if not same or failed:assert not events
        state['cases'][name]={'result':'pass','clean_refusal':bool(record.get('clean_refusal')),'events':events};save()
    refusal_case('clean-refusal-restores-before-release',repeat=True)
    refusal_case('stale-session-report-refused',stale=True)
    refusal_case('post-publication-report12-retains',report_reason=12)
    refusal_case('enabled-watchdog-retains',watchdog=1)
    refusal_case('report-transport-failure-retains',error_at='read')
    refusal_case('restoration-transport-failure-retains',error_at='write')
    refusal_case('changed-instance-forbids-probe',same=False)
    refusal_case('prior-transport-failure-forbids-probe',failed=True)
    if 'native-success' not in state['cases']:
        events=[];record={'launch_attempted':True,'scheduler_result':'observed-success'}
        host.finish_after_observation(record,lambda:events.append('save'),lambda:events.append('quarantine'),lambda:events.append('hold'))
        assert events==['save','quarantine','hold'] and record['manual_reconnect_required']
        state['cases']['native-success']={'result':'pass','events':events};save()
    state['result']='PASS: two changed frequency cases and clean-refusal continuation/retention';save()
    preserved={}
    for name,path in [('host_tool',Path(host.__file__)),('base_host_tool',Path(host.base.__file__)),
                      ('observer_host_tool',Path(host.observer.__file__))]:
        destination=e/(name+'.py');shutil.copy2(path,destination);preserved[name]=ref(destination)
    host.save_json(e/'software.json',{'schema':'a733-smp-supervised-software-v1','physical_access':False,
        'admission_cases':True,'launcher_cases':True,'host_lifecycle':True,
        'image':ref(image),'launcher':ref(bundle/'launch.bin'),**preserved,
        'evidence':[ref(prior_result),ref(state_path)]})
    print(state['result'])

if __name__=='__main__':main()
