#!/usr/bin/env python3
"""Check the combined-report profile through the existing simulated FEL runner."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]

def module(name, path):
    spec=importlib.util.spec_from_file_location(name,path)
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

shared=module('shared_primary_host_check',ROOT/'checks/primary-publication-host.py')
host=module('identity_host_check',ROOT/'tools/secondary-identity-host.py')


def report_transform(data, dram, case):
    result=bytearray(data+bytes(0x900))
    epoch=struct.unpack_from('<Q',dram,0x1004000+24)[0]
    # The real A32 launcher fills the absolute deadline after upload. This
    # simulated FEL transport does not execute that already checked launcher.
    deadline=24000000
    header=[0x4137334944454e31,1,0x900,epoch,deadline,0xfc,2,7]+[0]*24
    struct.pack_into('<32Q',result,256,*header)
    for core in range(2,8):
        slot=dict.fromkeys(host.FIELDS,0)
        slot.update(epoch=epoch,core=core,midr=0x412fd050 if core<6 else 0x413fd0b0,
            mpidr=0x81000000|(core<<8),sctlr3=0x30c50830,sctlr1=0x30d00800+(core<<16),
            tcr1=(core<<32)|0x123400,clusteridr=0x41,clustercfr=0x073fc237,clidr=0x124,mmfr0=2,
            power_before=0x100,power_after=0x101,ready=1,request=epoch,ack=epoch,
            restored=1,owner_state=2,report_state=2)
        struct.pack_into('<32Q',result,512+core*256,*slot.values(),*([0]*5))
    if case=='invalid-report-restored':
        struct.pack_into('<Q',result,24,4)  # Valid primary; corrupt only the secondary report.
        struct.pack_into('<Q',result,512+2*256+136,0)
    return bytes(result)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('build','bundle','model','survey','fel','output'):
        p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--case',action='append')
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=bool(a.case))
    names=('complete-return','transport-failure-retained','invalid-report-restored',
           'changed-input-refused','incomplete-shutdown-refused')
    proof={'schema':'a733-secondary-identity-software-v1','physical_access':False,
        'manifest_sha256':host.digest(a.build/'manifest.json'),
        'host_sources':{n:host.digest(ROOT/n) for n in host.HOST_SOURCES},
        'instruction_evidence':{'path':str(a.model.resolve()),'sha256':host.digest(a.model)},
        'host_cases':dict.fromkeys(names,True)}
    fixture=a.output/'MODEL-never-deploy.json';fixture.write_text(json.dumps(proof,indent=2)+'\n')
    accepted_path=a.output/'accepted-cases.json'
    accepted=json.loads(accepted_path.read_text()) if accepted_path.exists() else {
        'manifest_sha256':proof['manifest_sha256'],'host_sources':proof['host_sources'],'cases':{}}
    assert accepted['manifest_sha256']==proof['manifest_sha256'] and accepted['host_sources']==proof['host_sources']
    for case in a.case or names:
        assert case in names
        if case=='changed-input-refused':
            manifest=json.loads((a.build/'manifest.json').read_text())
            manifest['reservations'][-1][2]-=8
            try:host.load(a.build,a.bundle/'image.elf',manifest)
            except ValueError:pass
            else:raise AssertionError('Changed identity backup reservation accepted')
        elif case=='incomplete-shutdown-refused':
            raw=(a.output/'complete-return/runtime/report.bin').read_bytes()
            nonce=bytes.fromhex(json.loads((a.output/'complete-return/runtime/session.json').read_text())['nonce'])
            for field,value in ((136,0),(184,8),(192,0)):
                bad=bytearray(raw);struct.pack_into('<Q',bad,512+2*256+field,value)
                try:host.decode(bad,nonce)
                except ValueError:pass
                else:raise AssertionError('Incomplete shutdown/restoration accepted')
        else:assert shared.run_case(a,case,fixture,report_transform)
        accepted['cases'][case]={'pass':True,'check_sources':{n:host.digest(ROOT/n) for n in (
            'checks/secondary-identity-host.py','checks/primary-publication-host.py')}}
        accepted_path.write_text(json.dumps(accepted,indent=2)+'\n');print('PASS '+case,flush=True)
    assert set(accepted['cases'])==set(names)
    proof['case_evidence']=accepted['cases']
    proof['limits']='Actual shared runner; modeled FEL, USB, queue authority and report. No hardware access.'
    (a.output/'software.json').write_text(json.dumps(proof,indent=2)+'\n')

if __name__=='__main__':main()
