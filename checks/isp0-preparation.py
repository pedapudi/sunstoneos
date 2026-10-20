#!/usr/bin/env python3
"""Prepare the actual ISP0 image with explicit MODEL-NOT-PHYSICAL authority.

Only physical_authority is substituted. Typed facts, source, allocator rows,
full page-table bytes, image geometry and retained launcher remain production code.
No native image build, device, queue or proprietary tool executes.
"""
import argparse,copy,hashlib,importlib.util,json,struct
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','session','input','allocation','table','output','clang','lld'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--allocation-case',action='append',choices=('alias','permission','guard','table-hole','geometry','prefetch'))
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    prep=module('isp0_fixture_prepare',ROOT/'apps/isp0/prepare.py')
    producer=module('isp0_fixture_producer',ROOT/'apps/isp0/producer.py')
    fixture=a.output/'MODEL-NOT-PHYSICAL-fixture';fixture.mkdir()
    image=a.image;manifest=a.manifest;source=a.input.read_bytes()
    original=a.session.read_bytes();board=bytearray(original[:592])
    ih=hashlib.sha256(image.read_bytes()).hexdigest()
    target=dict(identity='isp0-software-only-model',port='no-physical-port',image_sha256=ih,
        physical_authority=False,modeled=True)
    def record(name,value):
        data=json.dumps(value,sort_keys=True,indent=2).encode()+b'\n';path=(fixture/name).resolve();path.write_bytes(data)
        return dict(path=str(path),sha256=hashlib.sha256(data).hexdigest())
    note=record('MODEL-NOT-PHYSICAL.json',dict(**target,purpose=__doc__))
    for offset in (80,216,248,280):board[offset:offset+32]=bytes.fromhex(note['sha256'])
    fields={'initial':['all_masters_excluded','prior_transactions_drained','prior_walks_drained','isp0_inherited_powered_reset_deasserted','skipped_masters_physically_quiescent','remote_secure_writers_excluded','isp0_vipp_dma_irqs_owned_masked','iommu_irqs_owned_masked'],
        'memory':['private_nc_arenas','no_physical_aliases','iommu_banks_disabled','iommu_clocks_stable','isp0_master1_read_master2_write_mapping'],
        'electrical':['dcdc2_shared_800mv','isp0_operating_point_source_reviewed','stable_isp0_parent'],
        'supervision':['whole_board_power_removal','permanent_dma_retention','no_reuse_after_fault','one_attempt']}
    refs={};facts={}
    for key,required in fields.items():
        refs[key]=record(key+'.json',dict(**target,schema='a733-isp0-'+key+'-v1',reviewed=True,**{k:True for k in required}))
        facts[key]=Path(refs[key]['path']).read_bytes()
    owner,authority,client=struct.unpack_from('<3Q',original,616)
    raw=producer.extend(bytes(board),source,owner=owner,authority=authority,client=client,**facts)
    th=hashlib.sha256(raw).hexdigest()
    proof=json.loads(a.allocation.read_bytes())
    assert proof['physical_authority'] is False and proof['modeled'] is True and proof['image']==ih
    assert proof['session']==hashlib.sha256(original).hexdigest()
    assert hashlib.sha256(source).digest()==original[768:800]
    proof['source_session']=proof['session'];proof['session']=th
    allocation=record('allocation.json',proof)
    table=a.table.read_bytes();tablepath=fixture/'page-table.bin';tablepath.write_bytes(table)
    table_ref=dict(path=str(tablepath.resolve()),sha256=hashlib.sha256(table).hexdigest())
    conditions=('source-backed-cpu-dsu-transition','cold-cache-and-dirty-alias-exclusion','dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded','usb-clock-power-reset-ownership','usb-device-mapping','physical-counter-continuity')
    platform=record('platform.json',dict(**target,schema='a733-smp-observer-platform-v1',entry_conditions_established=True,physical_coherence_result='under-test',conditions={k:note for k in conditions}))
    def ddr(name,ranges):return record(name,dict(**target,schema='a733-ddr-reservation-v1',exclusive=True,published_cache_disabled=True,remote_writers_excluded=True,ranges=ranges,evidence=[note]))
    admission=dict(**target,schema='a733-isp0-supervised-entry-v1',reservation=struct.unpack_from('<Q',raw,200)[0],session_template_sha256=th,
        ddr=ddr('ddr.json',[[0x41000000,0x42200000]]),platform=platform,
        allocation=allocation,page_table=table_ref,**refs,whole_board_power_removal=True,no_resource_reuse_before_power_removal=True,
        recovery=dict(mode='manual-usb-reconnect',demonstrated_fel=True,explicit_reconnect_before_resumption=True,preserve_microsd=True,one_attempt=True,evidence=[note]))
    record('admission.json',admission);(fixture/'session-template.bin').write_bytes(raw)
    try:prep.validate_admission(admission,ih,th,raw)
    except ValueError as error:assert 'Modeled ISP0 evidence' in str(error)
    else:raise AssertionError('Production accepted modeled physical authority')
    prep.allocation_check(proof,raw,table,image.read_bytes())
    refused=[]
    for change in a.allocation_case or ('alias','permission','guard','table-hole','geometry','prefetch'):
        bad=copy.deepcopy(proof);badtable=bytearray(table);wrong=bytearray(raw)
        if change=='alias':bad['allocations']['boot_pool'][1]['physical']=bad['allocations']['boot_pool'][0]['physical']
        if change=='permission':bad['spans'][0]['access']=3
        if change=='guard':bad['spans'][1]['physical']-=4096
        if change=='table-hole':badtable[4]=1
        if change=='geometry':struct.pack_into('<I',wrong,848,641)
        if change=='prefetch':bad['spans'][0]['bytes']-=4096
        try:prep.allocation_check(bad,bytes(wrong),bytes(badtable),image.read_bytes())
        except ValueError:refused.append(change)
        else:raise AssertionError('Invalid allocation accepted: '+change)
    bundle=a.output/'MODEL-NOT-PHYSICAL-bundle'
    with patch.object(prep,'physical_authority',lambda value:None):
        prep.prepare(image,manifest,fixture/'admission.json',fixture/'session-template.bin',bundle,a.clang,a.lld,a.input)
    result=dict(passed=True,physical_access=False,physical_authority=False,scope=__doc__,image_sha256=ih,
        source_sha256=hashlib.sha256(source).hexdigest(),allocation_source=str(a.allocation),
        allocation_source_sha256=hashlib.sha256(a.allocation.read_bytes()).hexdigest(),
        launcher_sha256=prep.base.digest(bundle/'launch.bin'),production_refused=True,allocation_refusals=refused,bundle=str(bundle.resolve()))
    (a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
if __name__=='__main__':main()
