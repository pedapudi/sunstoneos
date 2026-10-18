#!/usr/bin/env python3
"""Check local reports and their enlarged backup through the existing runner."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
old=module('identity_check',ROOT/'checks/secondary-identity-host.py')
host=module('local_host',ROOT/'tools/secondary-local-host.py')


def transform(model_records):
    def report(data,dram,case):
        raw=bytearray(old.report_transform(data,dram,'complete-return')+bytes(0x1200))
        # The shared transport intentionally corrupts primary state for this case;
        # keep primary valid and test a missing local restoration instead.
        struct.pack_into('<Q',raw,24,4)
        epoch=struct.unpack_from('<Q',raw,280)[0]
        struct.pack_into('<Q',raw,256,0x4137334c4f434131)
        struct.pack_into('<Q',raw,272,0x1b00)
        struct.pack_into('<Q',raw,296,0xfe)
        raw[768:1024]=raw[1024:1280]
        struct.pack_into('<Q',raw,776,1);struct.pack_into('<Q',raw,792,0x81000100)
        for core,record in enumerate(model_records,1):
            values=list(record);values[1]=epoch
            struct.pack_into('<64Q',raw,0xa00+core*512,*values)
        struct.pack_into('<5Q',raw,0x1a00,4,0x37,0x37,4,24000000)
        enables=[0]+[0x10101*i for i in range(1,4)]+[0]*28
        struct.pack_into('<32I',raw,0x1a28,*enables);struct.pack_into('<32I',raw,0x1aa8,*enables)
        if case=='invalid-report-restored':struct.pack_into('<Q',raw,0xa00+6*512+27*8,1)
        return bytes(raw)
    return report


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('build','bundle','model','survey','fel','output'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--case',action='append')
    a=parser.parse_args();a.output.mkdir(parents=True,exist_ok=bool(a.case))
    cases=('complete-return','transport-failure-retained','invalid-report-restored','changed-input-refused',
           'incomplete-local-refused','restored-refusal-distinct')
    proof={'schema':'a733-secondary-local-software-v1','physical_access':False,
        'manifest_sha256':host.digest(a.build/'manifest.json'),'host_sources':{n:host.digest(ROOT/n) for n in host.HOST_SOURCES},
        'instruction_evidence':{'path':str(a.model.resolve()),'sha256':host.digest(a.model)},'host_cases':dict.fromkeys(cases,True)}
    fixture=a.output/'MODEL-never-deploy.json';fixture.write_text(json.dumps(proof,indent=2)+'\n')
    records={c['case']:c['local_reports'] for c in json.loads(a.model.read_text())['cases']}
    accepted_path=a.output/'accepted-cases.json'
    accepted=json.loads(accepted_path.read_text()) if accepted_path.exists() else {'manifest_sha256':proof['manifest_sha256'],'host_sources':proof['host_sources'],'cases':{}}
    assert accepted['manifest_sha256']==proof['manifest_sha256'] and accepted['host_sources']==proof['host_sources']
    for case in a.case or cases:
        if case in cases[:3]:assert old.shared.run_case(a,case,fixture,transform(records['success']))
        elif case=='changed-input-refused':
            m=json.loads((a.build/'manifest.json').read_text());m['reservations'][-1][2]-=8
            try:host.load(a.build,a.bundle/'image.elf',m)
            except ValueError:pass
            else:raise AssertionError('Changed backup reservation accepted')
        else:
            raw=(a.output/'complete-return/runtime/report.bin').read_bytes()
            nonce=bytes.fromhex(json.loads((a.output/'complete-return/runtime/session.json').read_text())['nonce'])
            assert host.decode(raw,nonce)['local_initialization_completed']
            if case=='incomplete-local-refused':
                for offset,value in ((0xa00+6*512,2),(0xa00+6*512+27*8,1),(0x1a10,0),(512+6*256+136,0)):
                    bad=bytearray(raw);struct.pack_into('<Q',bad,offset,value)
                    try:host.decode(bad,nonce)
                    except ValueError:pass
                    else:raise AssertionError('Incomplete local/global/OFF restoration accepted')
            else:
                bad=bytearray(raw);r=list(records['group-refusal'][5]);r[1]=struct.unpack_from('<Q',raw,280)[0]
                struct.pack_into('<64Q',bad,0xa00+6*512,*r)
                decoded=host.decode(bad,nonce)
                assert not decoded['local_initialization_completed'] and decoded['local_results'][5]['refused_and_restored']
        accepted['cases'][case]={'pass':True,'check_sha256':host.digest(Path(__file__))}
        accepted_path.write_text(json.dumps(accepted,indent=2)+'\n');print('PASS',case,flush=True)
    assert set(accepted['cases'])==set(cases)
    proof['case_evidence']=accepted['cases'];proof['limits']='Actual host runner; modeled FEL, USB, queue and reports. No physical access.'
    (a.output/'software.json').write_text(json.dumps(proof,indent=2)+'\n')
if __name__=='__main__':main()
