#!/usr/bin/env python3
"""One complete compound composition with explicit model authority/transport.

The actual frozen CPU bundle, helper, survey build, SRAM capture functions,
receipt parsers, reference comparator and read-only SQL admission execute.
USB access, FEL and helper execution are synthetic. Initial RAM-continuity
validation is an API fixture because the source bundle deliberately names a
nonphysical target. No real queue, USB/sysfs or FEL executable is opened.
"""
import argparse
from contextlib import contextmanager
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sqlite3
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
from harness import refused, save
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('compound_integration',ROOT/'apps/os/cpu-return-followup.py')
f=importlib.util.module_from_spec(spec);spec.loader.exec_module(f)
spec=importlib.util.spec_from_file_location('compound_fixture_helpers',ROOT/'checks/os-cpu-return-followup.py')
fixtures=importlib.util.module_from_spec(spec);spec.loader.exec_module(fixtures)
E=Path.home()/'.cache/sunstoneos/evidence/sol-cpu-normal-return-1028'
L=Path.home()/'.cache/sunstoneos/evidence/sol-resume-20261028/port-3-1'


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();args.out.mkdir(parents=True,exist_ok=False)
    bundle=E/'compatibility-model/MODEL-NOT-DEPLOYABLE-bundle';p=f.cpu.validate_bundle(bundle)
    command=json.loads((L/'cpu-recognition-survey-setup/command.json').read_text())
    survey_build=Path(command[command.index('--build')+1]);survey_software=Path(command[command.index('--software')+1])
    rows=[];actual_subprocess=f.subprocess;actual_module=f.module
    actual_kernel=f.cpu.cpu.kernel.main;actual_continuity=f.cpu.cpu.kernel.validate_platform_admission
    actual_stop=f.base.stop_after_failure; actual_validate=f.cpu.validate_bundle
    previous_guard=sys.modules.get('usb_guard');previous_port=os.environ.get('SUNSTONE_USB_PORT')
    # Only the pre-entry DDR/survey contract is modeled. The new successor SRAM
    # survey is decoded below and checked again by the modeled helper entry.
    f.cpu.cpu.kernel.validate_platform_admission=lambda *a,**k: None
    sys.modules['usb_guard']=SimpleNamespace(check_health=lambda:None)
    try:
        for variant in ('success','bad-CPU-report','changed-helper-artifact','wrong-live-successor',
                        'halt-before-survey','transport-loss','helper-nonzero','gpu-profile-refused','wrong-database'):
            with tempfile.TemporaryDirectory(prefix='MODEL-compound-full-') as temporary:
                root=Path(temporary);saved=root/'MODEL-CPU-return';saved.mkdir()
                def ref(path):return {'path':str(Path(path).resolve()),'sha256':f.digest(path)}
                def put(name,value):
                    path=root/name;path.write_text(json.dumps(value,indent=2)+'\n');return ref(path)
                receipt=copy.deepcopy(json.loads((E/'compatibility-host/MODEL-return-receipt.json').read_text())['model_record'])
                receipt.update(port=p['_review']['port'],image_sha256=p['image_sha256'],initial_node='/dev/bus/usb/099/001',
                    initial_identity=p['_review']['identity'],returned_node='/dev/bus/usb/099/002',returned_identity=[0,0,2,0,0],
                    host_source_sha256=f.digest(Path(f.cpu.__file__)), automatic_access=True,
                    tool_sha256=f.base.FEL_HASH)
                report=(E/'entry-terminal-success/return-report.bin').read_bytes();(saved/'return-report.bin').write_bytes(report)
                text=('RESULT feat_crc=ac657a3b lp_crc=a7ade80e frames=90\nTEXT there was silence\nMARKS 002\nCAPS 000\nFORMATTED There was silence.\nPASS mimus: MODEL ONLY fixture\n')
                encoded=text.encode();output=struct.pack('<6Q',f.cpu.OUTPUT_MAGIC,1,f.cpu.OUTPUT_BYTES,p['epoch'],p['generation'],len(encoded))+encoded+bytes(16384-len(encoded))
                (saved/'recorded-output.bin').write_bytes(output);(saved/'transcript.txt').write_text(text)
                receipt.update(report_sha256=hashlib.sha256(report).hexdigest(),output_sha256=hashlib.sha256(output).hexdigest())
                if variant=='bad-CPU-report':receipt['report_sha256']='0'*64
                (saved/'session.json').write_text(json.dumps(receipt,indent=2)+'\n')
                placeholder=put('MODEL-pre-entry-continuity.json',{'MODEL-NOT-PHYSICAL':True})
                review={'schema':f.SCHEMA,'reviewed':True,'execution_profile':f.cpu.PROFILE,
                    'claim':receipt['root_owned_claim'],'owner':'MODEL-COORDINATOR','port':receipt['port'],
                    'database':str(root/'MODEL-queue.sqlite3'),
                    'phases':list(f.PHASES),'one_attempt':True,'failure_policy':'retain-without-retry',
                    'successor_authority':'validated-cpu-return-receipt-only',
                    'helper_ram':{'range':[0x41000000,0x42200000],'exclusive':True,'disposition':'retired-overwritten-no-preserved-bytes'},
                    'no_unrelated_successor':True,'sources':[ref(ROOT/n) for n in f.SOURCES],
                    'cpu_bundle':str(bundle),'cpu_prepared':ref(bundle/'prepared.json'),
                    'image_sha256':p['image_sha256'],'build_id':p['build_id'],
                    **{k:p[k] for k in ('epoch','generation','lease')},
                    'initial_identity':p['_review']['identity'],'initial_node':receipt['initial_node'],
                    'cpu_return_review':ref(bundle/'return-review.json'),'return_session':str(saved/'session.json'),
                    'outputs':{'root':str(root/'followup')},
                    'cache_off':ref(Path.home()/'.cache/sunstoneos/evidence/all-core-first-task-20261018/warm-010/cache-off/cache-off.bin'),
                    'survey':{'manifest':ref(survey_build/'manifest.json'),'binary':ref(survey_build/'launch-survey.bin'),'software':ref(survey_software)},
                    'helper':{'image':ref(E/'rails-800/build/probe.elf'),'binary':ref(E/'rails-800/probe.bin'),'manifest':ref(E/'rails-800/input-identities.json')},
                    'reference':ref(E/'first-port-3-1-guarded/reference/001.json'),
                    'ddr':placeholder,'initial_survey':placeholder,'warm_returns':[]}
                if variant=='changed-helper-artifact':review['helper']['binary']['sha256']='0'*64
                if variant=='wrong-database':review['database']=str(root/'OTHER-QUEUE.sqlite3')
                f.cpu.validate_bundle=actual_validate
                if variant=='gpu-profile-refused':
                    def gpu_bundle(path):
                        v=actual_validate(path);v['qualification']['gpu']={'MODEL-ONLY':True};return v
                    f.cpu.validate_bundle=gpu_bundle
                review_ref=put('MODEL-compound-authority.json',review)
                dbpath=root/'MODEL-queue.sqlite3';db=sqlite3.connect(dbpath)
                db.executescript('CREATE TABLE settings(key TEXT,value);CREATE TABLE tests(id,state,owner,submission,review);')
                db.executemany('INSERT INTO settings VALUES(?,?)',[('port',review['port']),('halted',0)])
                submission={'prerequisites':[f.PREREQUISITE]};qr={'prerequisites':{f.PREREQUISITE:review_ref}}
                db.execute('INSERT INTO tests VALUES(?,?,?,?,?)',(review['claim'],'claimed',review['owner'],json.dumps(submission),json.dumps(qr)));db.commit()
                run_args=SimpleNamespace(database=dbpath,claim=review['claim'],owner=review['owner'],review=root/'MODEL-compound-authority.json',
                    evidence=root/'followup',fel=Path.home()/'.cache/sunstoneos/build/sunxi-a733/sunxi-fel')
                os.environ['SUNSTONE_USB_PORT']=review['port'];requests=[];helper_calls=[];memory={}
                @contextmanager
                def locked():yield Path(receipt['returned_node'])
                def live_identity(node):return [0,0,99,0,0] if variant=='wrong-live-successor' else receipt['returned_identity']
                access=SimpleNamespace(board_lock=locked,require_session=lambda n:None,require_rom=lambda n:None,
                    identity=live_identity,target=lambda n:SimpleNamespace(name=review['port']),quarantine_instance=lambda *a:None)
                def imported(name,path):
                    if str(path)=='tools/usb-access.py':return access
                    return actual_module(name,path)
                f.module=imported
                def run_command(command,**kw):
                    command=list(map(str,command))
                    if command[0]==str(run_args.fel):
                        assert command[1:3]==['-d','99:2'];requests.append(command)
                        operations=command[3:]
                        if operations==['version']:return SimpleNamespace(returncode=0,stdout='soc=00001903(A733)',stderr='')
                        if variant=='transport-loss' and operations[:2]==['read','0x86000'] and len(requests)>8:
                            raise OSError('MODEL ONLY transport failure')
                        tx,_=fixtures.memory_tx(memory);tx(*operations,pass_fds=kw.get('pass_fds',()))
                        if variant=='halt-before-survey' and operations[:2]==['write','0x73000'] and memory.get(0x73000)==bytes([0xa5])*40:
                            db.execute('UPDATE settings SET value=1 WHERE key="halted"');db.commit()
                        return SimpleNamespace(returncode=0,stdout='',stderr='')
                    assert command[0]==sys.executable and command[1]==str(ROOT/'checks/mimus-parity.py'),command
                    return actual_subprocess.run(command,**kw)
                f.subprocess=SimpleNamespace(run=run_command)
                def kernel_main(argv):
                    helper_calls.append(argv);assert '--controlled' not in argv
                    assert argv[argv.index('--image')+1]==review['helper']['binary']['path']
                    current=json.loads(Path(argv[argv.index('--platform-survey')+1]).read_text())
                    assert current['identity']==receipt['returned_identity'] and current['node']==receipt['returned_node']
                    assert all(current['result']['launcher_gates'].values()) and argv[-2:]==['--warm-return',str(saved/'session.json')]
                    # Exercise the actual shared successor-RAM validator with a
                    # typed continuity fixture. The original preparation model
                    # deliberately uses a nonphysical string identity.
                    typed=copy.deepcopy(receipt);typed['initial_identity']=[0,0,1,0,0]
                    ddr={'complete':True,'device':typed['initial_node'],'node_identity':typed['initial_identity'],
                        'tool_sha256':f.base.FEL_HASH,'result':{'magic':0x53524444,'stage':5,'board_result':0,'dram_mib':12288}}
                    actual_continuity((json.dumps(ddr).encode(),json.dumps(current).encode()),
                        Path(typed['returned_node']),typed['returned_identity'],typed['port'],
                        f.base.FEL_HASH,f.base.FEL_HASH,(json.dumps(typed).encode(),))
                    out=Path(argv[argv.index('--evidence')+1]);out.mkdir()
                    hr={'complete':True,'transport_failed':False,'automatic_access':True,'mode':'kernel',
                        'result':{'magic':0x534b4552,'stage':5,'status':2 if variant=='helper-nonzero' else 0},
                        'initial_node':receipt['returned_node'],'initial_identity':receipt['returned_identity'],
                        'port':receipt['port'],'image_sha256':review['helper']['binary']['sha256'],
                        'returned_node':'/dev/bus/usb/099/003','returned_identity':[0,0,3,0,0]}
                    (out/'session.json').write_text(json.dumps(hr));(out/'kernel.log').write_text(fixtures.helper_log());return 0
                f.cpu.cpu.kernel.main=kernel_main
                f.base.stop_after_failure=lambda record,save,quarantine,error,**kw:(save(),quarantine())
                if variant=='success':
                    f.authority(run_args,{'port':review['port']})
                    assert not run_args.evidence.exists() and not requests and not helper_calls
                    result=f.run(run_args);assert result['complete'] and result['ownership_release_permitted']
                    assert not result['cpu_rail_restoration_pending'] and result['helper_ram_retired'] and not result['helper_dram_restored']
                    assert [x['name'] for x in result['phases']]==list(f.PHASES) and all(x['complete'] for x in result['phases'])
                    assert len(helper_calls)==1
                    refused(lambda:f.run(run_args),'File exists',FileExistsError)
                else:
                    reasons={'bad-CPU-report':'Copied CPU report/output','changed-helper-artifact':'Compound artifact changed',
                        'wrong-live-successor':'Successor differs','halt-before-survey':'halted','transport-loss':'MODEL ONLY transport',
                        'helper-nonzero':'complete normally','gpu-profile-refused':'CPU-only profile','wrong-database':'bounded compound authority'}
                    refused(lambda:f.run(run_args),reasons[variant],(ValueError,OSError))
                    if run_args.evidence.exists():
                        state=json.loads((run_args.evidence/'session.json').read_text())
                        assert not state['complete'] and state['cpu_rail_restoration_pending'] and not state['ownership_release_permitted']
                    if variant in ('bad-CPU-report','changed-helper-artifact','wrong-live-successor','gpu-profile-refused','wrong-database'):assert not requests
                    if variant!='helper-nonzero':assert not helper_calls
                assert db.execute('SELECT state FROM tests').fetchone()[0]=='claimed';db.close()
                rows.append({'name':variant,'passed':True,'modeled_FEL_calls':len(requests),'modeled_helper_calls':len(helper_calls),'actual_successor_RAM_validator_executed':bool(helper_calls)})
    finally:
        f.subprocess=actual_subprocess;f.module=actual_module;f.cpu.cpu.kernel.main=actual_kernel
        f.cpu.cpu.kernel.validate_platform_admission=actual_continuity;f.base.stop_after_failure=actual_stop;f.cpu.validate_bundle=actual_validate
        if previous_guard is None:sys.modules.pop('usb_guard',None)
        else:sys.modules['usb_guard']=previous_guard
        if previous_port is None:os.environ.pop('SUNSTONE_USB_PORT',None)
        else:os.environ['SUNSTONE_USB_PORT']=previous_port
    save(args.out/'results.json',{'physical_access':False,'scope':__doc__,'cases':rows})
    print('PASS',len(rows),'modeled compound compositions')


if __name__=='__main__':main()
