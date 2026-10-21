#!/usr/bin/env python3
"""Prepare fixed-strength TNR using actual image/allocator bytes and modeled authority.

The fixture controls only physical authority and prepare's entropy so the final
session exactly matches the already allocated native checkpoint. Production
randomization and every geometry, identity and table check remain unchanged.
No native build, device or queue operation executes here.
"""
import argparse,copy,hashlib,importlib.util,io,json,struct
from pathlib import Path
from unittest.mock import patch
from elftools.elf.elffile import ELFFile
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

def digest(raw):return hashlib.sha256(raw).hexdigest()
def ref(path):return dict(path=str(path.resolve()),sha256=digest(path.read_bytes()))
def write_once(path,data):
    if path.exists():assert path.read_bytes()==data,'Refuse changed fixture artifact: '+str(path)
    else:
        with path.open('xb') as stream:stream.write(data)
def encoded(value):return (json.dumps(value,sort_keys=True,indent=2)+'\n').encode()


def fixture(args,prep):
    """Create a new image-bound input template before native allocation."""
    target=args.output/'MODEL-NOT-PHYSICAL-fixture';target.mkdir(parents=True,exist_ok=False)
    image=args.native/'build-final/sunstone-a733-deinterlace.elf'
    image_raw=image.read_bytes();ih=digest(image_raw);_,_,identity=prep._image_parts(image_raw,mode=3)
    producer=module('tnr_fixture_producer',ROOT/'apps/deinterlace/producer.py')
    source=producer.field_pattern(3);original=args.base_session.read_bytes();board=bytearray(original[:592]);board[48:80]=identity
    authority=dict(identity='deinterlace-tnr-software-only-model',port='no-physical-port',image_sha256=ih,physical_authority=False,modeled=True)
    def record(name,value):write_once(target/name,encoded(value));return ref(target/name)
    note=record('MODEL-NOT-PHYSICAL.json',dict(**authority,purpose=__doc__,base_session=ref(args.base_session)))
    for offset in (80,216,248,280):board[offset:offset+32]=bytes.fromhex(note['sha256'])
    fields={'initial':['all_masters_excluded','prior_transactions_drained','prior_walks_drained','di_dedicated_reset_held_bus_and_module_gated','skipped_masters_physically_quiescent','remote_secure_writers_excluded','di_irq_owned_masked','iommu_irqs_owned_masked'],
        'memory':['private_nc_arenas','no_physical_aliases','iommu_banks_disabled','iommu_clocks_stable','di_device_mapping','dma_visibility_established'],
        'electrical':['dcdc2_shared_800mv','di_operating_point_source_reviewed','stable_di_parent'],
        'supervision':['whole_board_power_removal','permanent_dma_retention','no_reuse_after_fault','one_attempt']}
    facts={}
    for key,required in fields.items():
        record(key+'.json',dict(**authority,schema='a733-deinterlace-'+key+'-v1',reviewed=True,**{k:True for k in required}))
        facts[key]=(target/(key+'.json')).read_bytes()
    owner,token,client=struct.unpack_from('<3Q',original,616)
    raw=producer.extend(bytes(board),source,owner=owner,authority=token,client=client,mode=3,**facts)
    conditions=('source-backed-cpu-dsu-transition','cold-cache-and-dirty-alias-exclusion','dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded','usb-clock-power-reset-ownership','usb-device-mapping','physical-counter-continuity')
    record('platform.json',dict(**authority,schema='a733-smp-observer-platform-v1',entry_conditions_established=True,physical_coherence_result='under-test',conditions={k:note for k in conditions}))
    for name,ranges in [('ddr.json',[[0x41000000,0x42200000]]),('deinterlace-ddr.json',[[0x48000000,0x79000000]])]:
        record(name,dict(**authority,schema='a733-ddr-reservation-v1',exclusive=True,published_cache_disabled=True,remote_writers_excluded=True,ranges=ranges,evidence=[note]))
    write_once(target/'session-template.bin',raw);write_once(target/'input.nv12',source)
    return dict(session=ref(target/'session-template.bin'),input=ref(target/'input.nv12'),image=ref(image),build_id=identity.hex(),physical_authority=False)


def inputs(args,prep):
    target=args.output/'MODEL-NOT-PHYSICAL-fixture';raw=(target/'session-template.bin').read_bytes();source=(target/'input.nv12').read_bytes()
    image=args.native/'build-final/sunstone-a733-deinterlace.elf';manifest=args.native/'build-final/deinterlace-application/manifest.json'
    image_raw=image.read_bytes();ih=digest(image_raw);th=digest(raw);prep.session_check(raw)
    assert struct.unpack_from('<I',raw,876)[0]==3 and len(source)==921600
    prep.upload_bytes(source,raw);_,_,identity=prep._image_parts(image_raw,mode=3);assert raw[48:80]==identity
    mf=json.loads(manifest.read_bytes());assert mf['final_elf']==ih and mf['target']=='sunstone-a733-deinterlace' and mf['execution_profile']=='deinterlace-supervised-v1'
    note=json.loads((target/'MODEL-NOT-PHYSICAL.json').read_bytes())
    authority={k:note[k] for k in ('identity','port','image_sha256','physical_authority','modeled')}
    assert authority['image_sha256']==ih and authority['physical_authority'] is False and authority['modeled'] is True
    facts={key:ref(target/(key+'.json')) for key in ('initial','memory','electrical','supervision')}
    admission=dict(**authority,schema='a733-deinterlace-supervised-entry-v1',reservation=struct.unpack_from('<Q',raw,200)[0],session_template_sha256=th,
        ddr=ref(target/'ddr.json'),deinterlace_ddr=ref(target/'deinterlace-ddr.json'),platform=ref(target/'platform.json'),**facts,
        whole_board_power_removal=True,no_resource_reuse_before_power_removal=True,
        recovery=dict(mode='manual-usb-reconnect',demonstrated_fel=True,explicit_reconnect_before_resumption=True,preserve_microsd=True,one_attempt=True,evidence=[ref(target/'MODEL-NOT-PHYSICAL.json')]))
    proof_raw=args.allocation.read_bytes();table=args.table.read_bytes();proof=json.loads(proof_raw)
    assert proof['image']==ih and proof['session']==th and proof['physical_authority'] is False and proof['modeled'] is True
    write_once(target/'allocation.json',proof_raw);write_once(target/'page-table.bin',table)
    admission.update(allocation=ref(target/'allocation.json'),page_table=ref(target/'page-table.bin'))
    write_once(target/'admission.json',encoded(admission))
    return target,raw,source,image,manifest,image_raw,ih,th,proof,table,admission


def verify(args,prep,data):
    target,raw,source,image,manifest,image_raw,ih,th,proof,table,admission=data
    output=args.output/'verification.json';assert not output.exists(),'Preserve successful verification; do not replay it'
    try:prep.validate_admission(admission,ih,th,raw)
    except ValueError as error:assert 'Modeled deinterlace evidence' in str(error)
    else:raise AssertionError('Production accepted modeled authority')
    prep.allocation_check(proof,raw,table);refused=[]
    for case in ('legacy-flag-size','short-flag-output','old-flag-iova','flag-tail-hole'):
        bad=copy.deepcopy(proof);badtable=bytearray(table)
        if case=='legacy-flag-size':bad.update(md_bytes=76800,md_rounded=77824)
        elif case=='short-flag-output':bad['allocations']['uncached_pool'][5]['bytes']=86016
        elif case=='old-flag-iova':bad['spans'][4]['iova']=0x10c20000
        else:
            offset=4*(4096+3*1024+((0x10c9a000>>12)&1023));assert any(badtable[offset:offset+4])
            badtable[offset:offset+4]=bytes(4);bad['table_sha256']=digest(badtable)
        try:prep.allocation_check(bad,raw,bytes(badtable))
        except ValueError:refused.append(case)
        else:raise AssertionError('TNR flag geometry accepted: '+case)
    elf=ELFFile(io.BytesIO(image_raw));symbols=[s for s in elf.get_section_by_name('.symtab').iter_symbols() if s.name=='deinterlace_geometry' and s['st_info']['type']=='STT_OBJECT']
    assert len(symbols)==1;sym=symbols[0];section=elf.get_section(sym['st_shndx']);offset=section['sh_offset']+sym['st_value']-section['sh_addr']
    old=bytearray(image_raw);struct.pack_into('<Q',old,offset+8,2)
    try:prep._image_parts(bytes(old),mode=3)
    except ValueError:refused.append('legacy-marker')
    else:raise AssertionError('TNR admitted by pre-TNR image')
    result=dict(passed=True,physical_access=False,physical_authority=False,image_sha256=ih,session_sha256=th,input_sha256=digest(source),
        allocation_sha256=digest(args.allocation.read_bytes()),table_sha256=digest(table),production_refused=True,refused=refused)
    write_once(output,encoded(result));return result


def bundle(args,prep,data):
    target,raw,source,image,manifest,image_raw,ih,th,proof,table,admission=data
    prior=json.loads((args.output/'verification.json').read_bytes())
    assert prior['passed'] and (prior['image_sha256'],prior['session_sha256'],prior['allocation_sha256'],prior['table_sha256'])==(ih,th,digest(args.allocation.read_bytes()),digest(table))
    identities=struct.unpack_from('<3Q',raw,24);assert all(identities)
    def entropy(bits):assert bits==64;return next(values)
    values=iter(identities);out=args.output/'MODEL-NOT-PHYSICAL-bundle'
    with patch.object(prep,'physical_authority',lambda record:None),patch.object(prep.secrets,'randbits',side_effect=entropy) as randomizer:
        prepared=prep.prepare(image,manifest,target/'admission.json',target/'session-template.bin',out,args.clang,args.lld,target/'input.nv12')
    assert randomizer.call_count==3 and (out/'session.bin').read_bytes()==raw
    result=dict(passed=True,physical_access=False,physical_authority=False,scope=__doc__,image_sha256=ih,session_sha256=th,
        input_sha256=digest(source),fixture_entropy=list(identities),session_equals_template=True,launcher_sha256=prep.base.digest(out/'launch.bin'),bundle=str(out.resolve()))
    write_once(args.output/'result.json',encoded(result));return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('native','output'):parser.add_argument('--'+name,type=Path,required=True)
    for name in ('base-session','allocation','table','clang','lld'):parser.add_argument('--'+name,type=Path)
    parser.add_argument('--stage',choices=('fixture','verify','bundle'),required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    prep=module('deinterlace_tnr_preparation',ROOT/'apps/deinterlace/prepare.py')
    if args.stage=='fixture':
        if args.base_session is None:parser.error('Fixture requires the actual accepted --base-session')
        result=fixture(args,prep)
    else:
        if args.allocation is None or args.table is None:parser.error('Actual native --allocation and --table are required')
        data=inputs(args,prep)
        if args.stage=='verify':result=verify(args,prep,data)
        else:
            if args.clang is None or args.lld is None:parser.error('Bundle requires configured --clang and --lld')
            result=bundle(args,prep,data)
    print(json.dumps(result))

if __name__=='__main__':main()
