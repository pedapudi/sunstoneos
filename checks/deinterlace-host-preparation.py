#!/usr/bin/env python3
"""Package deinterlace inputs from direct device conditions and actual native allocations.

Geometry, ownership, permissions and full IOMMU table checks use production code.
No native workload or device access executes.
"""
import argparse,copy,hashlib,importlib.util,json,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

def direct_inputs(value):
    """Flatten typed fixture inputs and omit release-process metadata."""
    if isinstance(value,list):return [direct_inputs(item) for item in value]
    if not isinstance(value,dict):return value
    if set(value)=={'path','sha256'}:
        data=Path(value['path']).read_bytes()
        assert hashlib.sha256(data).hexdigest()==value['sha256']
        try:return direct_inputs(json.loads(data))
        except (ValueError,UnicodeDecodeError):return dict(value)
    omitted={'schema','reviewed','image_sha256','session_template_sha256','evidence','physical_access',
             'physical_authority','modeled','ddr','platform','recovery'}
    return {key:direct_inputs(item) for key,item in value.items() if key not in omitted}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('native','output'):p.add_argument('--'+name,type=Path,required=True)
    for name in ('allocation','table','clang','lld'):p.add_argument('--'+name,type=Path)
    p.add_argument('--stage',choices=('fixture','bundle'),required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=a.stage=='bundle')
    prep=module('deinterlace_fixture_prepare',ROOT/'apps/deinterlace/prepare.py')
    producer=module('deinterlace_fixture_producer',ROOT/'apps/deinterlace/producer.py')
    fixture=a.output/'MODEL-NOT-PHYSICAL-fixture';fixture.mkdir(exist_ok=a.stage=='bundle')
    image=a.native/'build/sunstone-a733-deinterlace.elf';manifest=a.native/'build/deinterlace-application/manifest.json'
    source=(a.native/'input.nv12').read_bytes()
    original=(a.native/'session.bin').read_bytes();board=bytearray(original[:592])
    ih=hashlib.sha256(image.read_bytes()).hexdigest()
    target=dict(identity='deinterlace-software-only-model',port='no-physical-port',image_sha256=ih,
        physical_authority=False,modeled=True)
    def record(name,value):
        data=json.dumps(value,sort_keys=True,indent=2).encode()+b'\n';path=(fixture/name).resolve();path.write_bytes(data)
        return dict(path=str(path),sha256=hashlib.sha256(data).hexdigest())
    note=record('MODEL-NOT-PHYSICAL.json',dict(**target,purpose=__doc__))
    for offset in (80,216,248,280):board[offset:offset+32]=bytes.fromhex(note['sha256'])
    fields={'initial':['all_masters_excluded','prior_transactions_drained','prior_walks_drained','di_inherited_powered_reset_deasserted','skipped_masters_physically_quiescent','remote_secure_writers_excluded','di_irq_owned_masked','iommu_irqs_owned_masked'],
        'memory':['private_nc_arenas','no_physical_aliases','iommu_banks_disabled','iommu_clocks_stable','di_device_mapping','dma_visibility_established'],
        'electrical':['dcdc2_shared_800mv','di_operating_point_source_reviewed','stable_di_parent'],
        'supervision':['whole_board_power_removal','permanent_dma_retention','no_reuse_after_fault','one_attempt']}
    refs={};facts={}
    for key,required in fields.items():
        refs[key]=record(key+'.json',dict(**target,schema='a733-deinterlace-'+key+'-v1',reviewed=True,**{k:True for k in required}))
        facts[key]=Path(refs[key]['path']).read_bytes()
    owner,authority,client=struct.unpack_from('<3Q',original,616)
    raw=producer.extend(bytes(board),source,owner=owner,authority=authority,client=client,**facts)
    th=hashlib.sha256(raw).hexdigest()
    conditions=('source-backed-cpu-dsu-transition','cold-cache-and-dirty-alias-exclusion','dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded','usb-clock-power-reset-ownership','usb-device-mapping','physical-counter-continuity')
    platform=record('platform.json',dict(**target,schema='a733-smp-observer-platform-v1',entry_conditions_established=True,physical_coherence_result='under-test',conditions={k:note for k in conditions}))
    def ddr(name,ranges):return record(name,dict(**target,schema='a733-ddr-reservation-v1',exclusive=True,published_cache_disabled=True,remote_writers_excluded=True,ranges=ranges,evidence=[note]))
    admission=dict(**target,schema='a733-deinterlace-supervised-entry-v1',reservation=struct.unpack_from('<Q',raw,200)[0],session_template_sha256=th,
        ddr=ddr('ddr.json',[[0x41000000,0x42200000]]),deinterlace_ddr=ddr('deinterlace-ddr.json',[[0x48000000,0x79000000]]),platform=platform,
        **refs,whole_board_power_removal=True,no_resource_reuse_before_power_removal=True,
        recovery=dict(mode='manual-usb-reconnect',demonstrated_fel=True,explicit_reconnect_before_resumption=True,preserve_microsd=True,one_attempt=True,evidence=[note]))
    (fixture/'session-template.bin').write_bytes(raw)
    if a.stage=='fixture':
        print(json.dumps(dict(session=str((fixture/'session-template.bin').resolve()),
            session_sha256=th,physical_access=False,physical_authority=False)));return
    if any(value is None for value in (a.allocation,a.table,a.clang,a.lld)):
        p.error('Bundle stage needs --allocation --table --clang --lld from actual native preflight.')
    proof=json.loads(a.allocation.read_bytes())
    assert proof['physical_authority'] is False and proof['modeled'] is True
    assert proof['image']==ih
    proof['source_session']=proof['session'];proof['session']=th
    assert hashlib.sha256(source).digest()==raw[768:800]
    allocation=record('allocation.json',proof)
    table=a.table.read_bytes();tablepath=fixture/'page-table.bin';tablepath.write_bytes(table)
    table_ref=dict(path=str(tablepath.resolve()),sha256=hashlib.sha256(table).hexdigest())
    admission.update(allocation=allocation,page_table=table_ref)
    record('admission.json',admission)
    admission=direct_inputs(admission)
    record('admission.json',admission)
    prep.validate_admission(admission,ih,th,raw)
    bad_conditions=copy.deepcopy(admission);bad_conditions['initial']['all_masters_excluded']=False
    try:prep.validate_admission(bad_conditions,ih,th,raw)
    except ValueError:pass
    else:raise AssertionError('Missing direct ownership condition accepted')
    refused=[]
    prep.allocation_check(proof,raw,table)
    for change in ('alias','permission','guard','table-hole','session'):
        bad=copy.deepcopy(proof);badtable=bytearray(table)
        if change=='alias':bad['allocations']['boot_pool'][1]['physical']=bad['allocations']['boot_pool'][0]['physical']
        if change=='permission':bad['spans'][0]['access']=3
        if change=='guard':bad['spans'][1]['physical']-=4096
        if change=='table-hole':badtable[0]=1
        if change=='session':
            wrong=bytearray(raw);struct.pack_into('<I',wrong,864,260)
        try:prep.allocation_check(bad,wrong if change=='session' else raw,bytes(badtable))
        except ValueError:refused.append(change)
        else:raise AssertionError('Bad allocation accepted: '+change)
    bundle=a.output/'MODEL-NOT-PHYSICAL-bundle'
    prep.prepare(image,None,fixture/'admission.json',fixture/'session-template.bin',bundle,a.clang,a.lld,a.native/'input.nv12')
    result=dict(passed=True,physical_access=False,physical_authority=False,scope=__doc__,image_sha256=ih,
        source_sha256=hashlib.sha256(source).hexdigest(),allocation_source=str(a.allocation),
        allocation_source_sha256=hashlib.sha256(a.allocation.read_bytes()).hexdigest(),
        launcher_sha256=prep.base.digest(bundle/'launch.bin'),direct_inputs_accepted=True,allocation_refusals=refused,bundle=str(bundle.resolve()))
    (a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
if __name__=='__main__':main()
