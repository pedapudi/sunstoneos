#!/usr/bin/env python3
"""Exercise cached CPU reports and enlarged backup using the actual host runner."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
shared=module('primary_check',ROOT/'checks/primary-publication-host.py')
host=module('cache_host',ROOT/'tools/secondary-cache-host.py')


def transform(model_report):
    def report(data,dram,case):
        raw=bytearray(model_report)
        raw[:256]=data
        struct.pack_into('<Q',raw,24,4)  # Keep the primary valid even in the invalid cache case.
        epoch=struct.unpack_from('<Q',dram,0x1004000+24)[0]
        struct.pack_into('<Q',raw,280,epoch)
        for core in range(1,8):
            for offset in (512+core*256,512+core*256+128,512+core*256+136,
                           0xa00+core*512+8,0x2000+core*256+8,0x2800+core*256+8):
                struct.pack_into('<Q',raw,offset,epoch)
        if case=='invalid-report-restored':struct.pack_into('<Q',raw,0x2800+6*256+24*8,0)
        return bytes(raw)
    return report


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('build','bundle','model','survey','fel','output','local-model'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--case',action='append')
    a=parser.parse_args();a.output.mkdir(parents=True,exist_ok=bool(a.case))
    cases=('complete-return','transport-failure-retained','invalid-report-restored','changed-input-refused',
           'incomplete-cache-refused','restored-refusal-distinct')
    proof={'schema':'a733-secondary-cache-software-v1','physical_access':False,
        'manifest_sha256':host.digest(a.build/'manifest.json'),'host_sources':{n:host.digest(ROOT/n) for n in host.HOST_SOURCES},
        'instruction_evidence':{'path':str(a.model.resolve()),'sha256':host.digest(a.model)},'host_cases':dict.fromkeys(cases,True)}
    fixture=a.output/'MODEL-never-deploy.json';fixture.write_text(json.dumps(proof,indent=2)+'\n')
    instruction=json.loads(a.model.read_text())
    model_report=a.model.parent/'report-success.bin'
    assert host.digest(model_report)==next(c['report_sha256'] for c in instruction['cases'] if c['case']=='success')
    records={c['case']:c['local_reports'] for c in json.loads(a.local_model.read_text())['cases']}
    manifest=json.loads((a.build/'manifest.json').read_text());smc_return=manifest['hooks']['el1_smc_return']
    accepted_path=a.output/'accepted-cases.json'
    accepted=json.loads(accepted_path.read_text()) if accepted_path.exists() else {'manifest_sha256':proof['manifest_sha256'],'host_sources':proof['host_sources'],'cases':{}}
    assert accepted['manifest_sha256']==proof['manifest_sha256'] and accepted['host_sources']==proof['host_sources']
    for case in a.case or cases:
        assert case in cases
        if case in cases[:3]:assert shared.run_case(a,case,fixture,transform(model_report.read_bytes()))
        elif case=='changed-input-refused':
            m=json.loads((a.build/'manifest.json').read_text());m['reservations'][-1][2]-=8
            try:host.load(a.build,a.bundle/'image.elf',m)
            except ValueError:pass
            else:raise AssertionError('Changed backup reservation accepted')
        else:
            raw=(a.output/'complete-return/runtime/report.bin').read_bytes()
            nonce=bytes.fromhex(json.loads((a.output/'complete-return/runtime/session.json').read_text())['nonce'])
            assert host.decode(raw,nonce,smc_return)['cached_entry_completed']
            if case=='incomplete-cache-refused':
                for offset,value in ((0x2000+6*256,2),(0x2000+6*256+14*8,0),
                                     (0x2800+6*256,2),(0x2800+6*256+24*8,0),
                                     (0x2800+6*256+17*8,1),(0x2800+6*256+22*8,smc_return+4),
                                     (0x2000+160,1),(0x1c00,1)):
                    bad=bytearray(raw);struct.pack_into('<Q',bad,offset,value)
                    try:host.decode(bad,nonce,smc_return)
                    except ValueError:pass
                    else:raise AssertionError('Incomplete or mismatched cache report accepted')
            else:
                bad=bytearray(raw);r=list(records['group-refusal'][5]);r[1]=struct.unpack_from('<Q',raw,280)[0]
                struct.pack_into('<64Q',bad,0xa00+6*512,*r)
                for base in (0x2000,0x2800):bad[base+6*256:base+7*256]=bytes(256)
                decoded=host.decode(bad,nonce,smc_return)
                assert not decoded['cached_entry_completed'] and decoded['cache_results'][5]['refused_before_cache']
        accepted['cases'][case]={'pass':True,'check_sha256':host.digest(Path(__file__))}
        accepted_path.write_text(json.dumps(accepted,indent=2)+'\n');print('PASS',case,flush=True)
    assert set(accepted['cases'])==set(cases)
    proof['case_evidence']=accepted['cases'];proof['limits']='Actual host runner; modeled FEL, USB, queue and reports. No physical access.'
    (a.output/'software.json').write_text(json.dumps(proof,indent=2)+'\n')
if __name__=='__main__':main()
