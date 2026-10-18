#!/usr/bin/env python3
"""Check interrupt delivery and shutdown reports through the actual returning host runner."""
import argparse,importlib.util,json,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
shared=module('primary_check',ROOT/'checks/primary-publication-host.py')
host=module('interrupt_host',ROOT/'tools/secondary-interrupt-host.py')


def transform(model_report):
    def report(data,dram,case):
        raw=bytearray(model_report);raw[:256]=data
        struct.pack_into('<Q',raw,24,4)  # Keep the primary valid in the invalid interrupt case.
        epoch=struct.unpack_from('<Q',dram,0x1004000+24)[0]
        struct.pack_into('<Q',raw,280,epoch)
        for core in range(1,8):
            for offset in (512+core*256,512+core*256+128,512+core*256+136,
                           0xa00+core*512+8,0x2000+core*256+8,0x2800+core*256+8,
                           0x3000+core*64+16):struct.pack_into('<Q',raw,offset,epoch)
            struct.pack_into('<Q',raw,0x3000+core*64+40,epoch^223)
        raw[0x3200:]=host.expected_memory(epoch)
        if case=='invalid-report-restored':struct.pack_into('<Q',raw,0x3200+0x100+6*128+48,0)
        return bytes(raw)
    return report


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('build','bundle','model','survey','fel','output'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--case',action='append')
    a=parser.parse_args();a.output.mkdir(parents=True,exist_ok=bool(a.case))
    cases=('complete-return','transport-failure-retained','invalid-report-restored','changed-input-refused',
           'incomplete-interrupts-refused','nonquiescent-interrupts-refused')
    proof={'schema':'a733-secondary-interrupt-software-v1','physical_access':False,
        'manifest_sha256':host.digest(a.build/'manifest.json'),'host_sources':{n:host.digest(ROOT/n) for n in host.HOST_SOURCES},
        'instruction_evidence':{'path':str(a.model.resolve()),'sha256':host.digest(a.model)},'host_cases':dict.fromkeys(cases,True)}
    fixture=a.output/'MODEL-never-deploy.json';fixture.write_text(json.dumps(proof,indent=2)+'\n')
    instruction=json.loads(a.model.read_text());model_report=a.model.parent/'report-success.bin'
    assert host.digest(model_report)==next(c['report_sha256'] for c in instruction['cases'] if c['case']=='success')
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
            assert host.decode(raw,nonce,smc_return)['interrupt_delivery_completed']
            slot=0x3200+0x100+6*128
            changes=((slot+48,0),(slot+48,2),(slot+56,0),(slot+56,2),
                     (slot+64,0),(slot+72,0),(slot+80,0)) if case=='incomplete-interrupts-refused' else (
                     (slot+88,1),(slot+96,1),(slot+104,1),(slot+112,1),(0x3000+6*64+48,63))
            for offset,value in changes:
                bad=bytearray(raw);struct.pack_into('<Q',bad,offset,value)
                try:host.decode(bad,nonce,smc_return)
                except ValueError:pass
                else:raise AssertionError('Incomplete delivery or nonquiescent interrupt state accepted')
        accepted['cases'][case]={'pass':True,'check_sources':{n:host.digest(ROOT/n) for n in ('checks/secondary-interrupt-host.py','checks/primary-publication-host.py')}}
        accepted_path.write_text(json.dumps(accepted,indent=2)+'\n');print('PASS',case,flush=True)
    assert set(accepted['cases'])==set(cases)
    proof['case_evidence']=accepted['cases'];proof['limits']='Actual host runner; modeled FEL, USB, queue and reports. No physical access.'
    (a.output/'software.json').write_text(json.dumps(proof,indent=2)+'\n')
if __name__=='__main__':main()
