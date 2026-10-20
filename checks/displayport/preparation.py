#!/usr/bin/env python3
"""Prepare actual DisplayPort artifacts using explicitly modeled authority only.

The native evidence supplies the actual ELF, original typed session, allocation
JSON, table and complete 2 MiB allocation. This checker substitutes only the
physical-authority predicate; all production image/layout/profile checks run.
No board, USB, queue or main native-image build executes.
"""
import argparse,copy,hashlib,importlib.util,json,shutil,struct
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[2]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','session','allocation','table','allocation_bytes','output','clang','lld'):
        p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--case',choices=('all','bundle'),default='all',help='Bundle resumes packaging without repeating completed refusal cases.')
    p.add_argument('--reuse-launcher',type=Path,help='Reuse the exact already compiled launcher; never rebuild it.')
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    prep=module('displayport_fixture_prepare',ROOT/'apps/displayport/prepare.py')
    producer=module('displayport_fixture_producer',ROOT/'apps/displayport/producer.py')
    original=a.session.read_bytes();prep.session_check(original)
    ih=hashlib.sha256(a.image.read_bytes()).hexdigest();proof=json.loads(a.allocation.read_bytes())
    assert proof['physical_authority'] is False and proof['model'] is True and proof['image_sha256']==ih
    assert proof['session_sha256']==hashlib.sha256(original).hexdigest()
    table=a.table.read_bytes();allocation=a.allocation_bytes.read_bytes()
    prep.allocation_check(proof,original,table,allocation)
    out=a.output/'MODEL-NOT-PHYSICAL-fixture';out.mkdir()
    target=dict(identity='displayport-software-only-model',port='no-physical-port',image_sha256=ih,modeled=True,physical_authority=False)
    def record(name,value):
        data=json.dumps(value,sort_keys=True,indent=2).encode()+b'\n';path=(out/name).resolve();path.write_bytes(data)
        return dict(path=str(path),sha256=hashlib.sha256(data).hexdigest())
    note=record('MODEL-NOT-PHYSICAL.json',dict(**target,purpose=__doc__))
    board=bytearray(original[:592])
    for offset in (80,216,248,280):board[offset:offset+32]=bytes.fromhex(note['sha256'])
    names=('isolation','memory','analog','power','serdes','supervision')
    fields={
        'isolation':('all_masters_excluded','prior_transactions_drained','prior_walks_drained','remote_secure_writers_excluded','display_irqs_owned_masked','iommu_irqs_owned_masked'),
        'memory':('private_nc_arena','no_physical_aliases','iommu_banks_disabled','iommu_clocks_stable','display_device_mapping'),
        'analog':('dp_analog_qualified','source_apb_independently_qualified'),
        'power':('typec24_source_limits_qualified','dp_supplies_stable'),
        'serdes':('comb0_exclusive_from_usb3','four_lane_dp_route_qualified','source_clocks_stable','source_apb_independently_qualified'),
        'supervision':('whole_board_power_removal','permanent_dma_retention','no_reuse_after_fault','one_attempt')}
    refs={};facts=[]
    for name in names:
        value=dict(**target,schema='a733-displayport-'+name+'-v1',reviewed=True,**{key:True for key in fields[name]})
        if name in ('analog','serdes'):value.update(qualified_apb_hz=struct.unpack_from('<I',original,860)[0],apb_evidence=[note])
        refs[name]=record(name+'.json',value);facts.append(Path(refs[name]['path']).read_bytes())
    owner,authority,client=struct.unpack_from('<3Q',original,616)
    profile=dict(clocks=struct.unpack_from('<8I',original,832),typec=struct.unpack_from('<3Q',original,864),
        latch=struct.unpack_from('<2Q2I',original,888),source=struct.unpack_from('<6I',original,912),
        pins=[original[at:at+3] for at in range(936,948,3)])
    raw=producer.extend(bytes(board),owner=owner,authority=authority,client=client,facts=facts,**profile)
    session_refusals=[]
    for name,offset,value in ([('pmic-function',564,0),('pmic-pull',568,3),('pmic-drive',569,4),('fabric-disabled',495,0)] if a.case=='all' else []):
        bad=bytearray(raw);bad[offset]=value
        try:prep.session_check(bad)
        except ValueError:session_refusals.append(name)
        else:raise AssertionError('Unusable provider profile accepted: '+name)
    th=hashlib.sha256(raw).hexdigest();proof['source_session']=proof['session_sha256'];proof['session_sha256']=th
    allocation_ref=record('allocation.json',proof)
    def binary(name,data):
        path=(out/name).resolve();path.write_bytes(data);return dict(path=str(path),sha256=hashlib.sha256(data).hexdigest())
    table_ref=binary('page-table.bin',table);bytes_ref=binary('allocation.bin',allocation)
    conditions=('source-backed-cpu-dsu-transition','cold-cache-and-dirty-alias-exclusion','dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded','usb-clock-power-reset-ownership','usb-device-mapping','physical-counter-continuity')
    platform=record('platform.json',dict(**target,schema='a733-smp-observer-platform-v1',entry_conditions_established=True,physical_coherence_result='under-test',conditions={key:note for key in conditions}))
    def ddr(name,ranges):return record(name,dict(**target,schema='a733-ddr-reservation-v1',exclusive=True,published_cache_disabled=True,remote_writers_excluded=True,ranges=ranges,evidence=[note]))
    admission=dict(**target,schema='a733-displayport-supervised-entry-v1',reservation=struct.unpack_from('<Q',raw,200)[0],session_template_sha256=th,
        ddr=ddr('ddr.json',[[0x41000000,0x42200000]]),displayport_ddr=ddr('displayport-ddr.json',[[0x48000000,0x79000000]]),
        platform=platform,allocation=allocation_ref,page_table=table_ref,allocation_bytes=bytes_ref,**refs,
        whole_board_power_removal=True,no_resource_reuse_before_power_removal=True,
        recovery=dict(mode='manual-usb-reconnect',demonstrated_fel=True,explicit_reconnect_before_resumption=True,preserve_microsd=True,one_attempt=True,evidence=[note]))
    record('admission.json',admission);(out/'session-template.bin').write_bytes(raw)
    try:prep.validate_admission(admission,ih,th,raw)
    except ValueError as error:assert 'Modeled DisplayPort evidence' in str(error)
    else:raise AssertionError('Production accepted modeled authority')
    refused=[]
    for name in (('table-permission','padding','pixel','mapping','extent') if a.case=='all' else ()):
        bad=copy.deepcopy(proof);badtable=bytearray(table);badallocation=bytearray(allocation)
        if name=='table-permission':badtable[16384]^=8
        if name=='padding':badallocation[32768]^=1
        if name=='pixel':badallocation[36864]^=1
        if name=='mapping':bad['spans'][0]['access']=3
        if name=='extent':bad['allocation']['physical']+=4096
        # Preserve internal digest consistency so these cases exercise mapping,
        # sentinel and pattern semantics rather than only stale hashes.
        if name=='table-permission':
            badallocation[:32768]=badtable
            bad['table_sha256']=hashlib.sha256(badtable).hexdigest()
        if name in ('table-permission','padding','pixel'):
            bad['allocation_sha256']=hashlib.sha256(badallocation).hexdigest()
        if name=='pixel':bad['pixel_sha256']=hashlib.sha256(badallocation[36864:36864+1843200]).hexdigest()
        try:prep.allocation_check(bad,raw,bytes(badtable),bytes(badallocation))
        except ValueError:refused.append(name)
        else:raise AssertionError('Invalid allocation accepted: '+name)
    bundle=a.output/'MODEL-NOT-PHYSICAL-bundle'
    original_launcher=prep.build_launcher
    def launcher(out,clang,lld):
        if a.reuse_launcher is None:return original_launcher(out,clang,lld)
        source=a.reuse_launcher
        sources={'launch.c':'apps/displayport/launch.c','launch.S':'platform/a733-smp-observe/launch.S',
            'launch.ld':'platform/a733-smp-observe/launch.ld','boot.h':'platform/a733-smp-cached/boot.h',
            'a733_displayport_application.h':'include/a733_displayport_application.h',
            'a733_board_application.h':'include/a733_board_application.h','a733_pins.h':'include/a733_pins.h',
            'base-supervised-launch.c':'platform/a733-smp-supervised/launch.c'}
        for name,relative in sources.items():assert (source/name).read_bytes()==(ROOT/relative).read_bytes(),name
        assert (source/'displayport-launch-base.c').read_text()==prep.launcher_base_source((ROOT/'platform/a733-smp-supervised/launch.c').read_text())
        resume=json.loads((source/'launcher-build-resume.json').read_bytes())
        assert resume['passed'] is True and prep.base.digest(source/'launch.bin')==resume['sha256'] and 0<(source/'launch.bin').stat().st_size<=0xc00
        names=[*sources,'displayport-launch-base.c','launch.c.o','launch.S.o','launch.c.d','launch.S.d',
            'launch.elf','launch.bin','launcher-build.log','launcher-commands.json','launcher-build-resume.json']
        for name in names:shutil.copy2(source/name,out/name)
        prep.base.save_json(out/'launcher-reuse.json',dict(source=str(source.resolve()),sha256=resume['sha256'],recompiled=False))
    with patch.object(prep,'physical_authority',lambda value:None),patch.object(prep,'build_launcher',launcher):
        prep.prepare(a.image,a.manifest,out/'admission.json',out/'session-template.bin',bundle,a.clang,a.lld)
    result=dict(passed=True,case=a.case,launcher_reused_from=str(a.reuse_launcher) if a.reuse_launcher else None,physical_access=False,physical_authority=False,scope=__doc__,image_sha256=ih,
        original_session_sha256=hashlib.sha256(original).hexdigest(),allocation_source_sha256=hashlib.sha256(a.allocation.read_bytes()).hexdigest(),
        launcher_sha256=prep.base.digest(bundle/'launch.bin'),production_refused=True,allocation_refusals=refused,session_refusals=session_refusals,bundle=str(bundle.resolve()))
    (a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
if __name__=='__main__':main()
