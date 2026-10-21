#!/usr/bin/env python3
"""Freeze real runtime inputs and capture the actual native JPEG result records."""
import argparse,hashlib,importlib.util,json,re,struct,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
def fixture(image,session,out,prep):
    out.mkdir(parents=True,exist_ok=False);raw=bytearray(session);ih=hashlib.sha256(image.read_bytes()).hexdigest()
    # Use the actual nominal PMIC producer for a complete provider configuration.
    source=ROOT/'checks/a733-pmic.c';match=re.search(r'const struct a733_pmic_config config=\{[^{}]*\{\{([^}]+)\},\{([^}]+)\}\}\};',source.read_text());assert match
    raw[564:570]=bytes(int(v) for group in match.groups() for v in group.split(','))
    target=dict(identity='jpeg-runtime-software-only-model',port='no-physical-port',image_sha256=ih)
    def record(name,value):
        data=json.dumps(value,sort_keys=True).encode();path=(out/name).resolve();path.write_bytes(data)
        return dict(path=str(path),sha256=hashlib.sha256(data).hexdigest())
    evidence=record('MODEL-NOT-PHYSICAL.json',dict(**target,physical_access=False,purpose='Modeled authority only; cannot establish physical entry facts'))
    for offset in (80,216,248,280):raw[offset:offset+32]=bytes.fromhex(evidence['sha256'])
    facts={'initial':['all_masters_excluded','prior_transactions_drained','prior_walks_drained','decoder_off_reset_held','skipped_masters_physically_quiescent','remote_secure_writers_excluded','irq81_owned_masked','iommu_irqs_owned_masked'],
      'memory':['private_nc_arenas','no_physical_aliases','iommu_banks_disabled','iommu_clocks_stable','immutable_rom_mapping'],
      'electrical':['dcdc2_shared_800mv','ve_operating_point_qualified','stable_ve_pll'],
      'supervision':['whole_board_power_removal','permanent_dma_retention','no_reuse','one_attempt']}
    refs={}
    for i,(key,required) in enumerate(facts.items()):
        refs[key]=record(key+'.json',dict(**target,schema='a733-jpeg-'+key+'-v1',reviewed=True,physical_access=False,**{k:True for k in required}));raw[640+32*i:672+32*i]=bytes.fromhex(refs[key]['sha256'])
    conditions=('source-backed-cpu-dsu-transition','cold-cache-and-dirty-alias-exclusion','dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded','usb-clock-power-reset-ownership','usb-device-mapping','physical-counter-continuity')
    platform=record('platform.json',dict(**target,schema='a733-smp-observer-platform-v1',entry_conditions_established=True,physical_coherence_result='under-test',conditions={k:evidence for k in conditions}))
    ddr=record('ddr.json',dict(**target,schema='a733-ddr-reservation-v1',exclusive=True,published_cache_disabled=True,remote_writers_excluded=True,ranges=[[0x41000000,0x42200000]],evidence=[evidence]))
    admission=dict(**target,schema='a733-jpeg-supervised-entry-v1',reservation=100,session_template_sha256=hashlib.sha256(raw).hexdigest(),ddr=ddr,platform=platform,**refs,whole_board_power_removal=True,no_resource_reuse_before_power_removal=True,recovery=dict(mode='manual-usb-reconnect',demonstrated_fel=True,explicit_reconnect_before_resumption=True,preserve_microsd=True,one_attempt=True,evidence=[evidence]))
    (out/'session-template.bin').write_bytes(raw);(out/'admission.json').write_text(json.dumps(admission,indent=2)+'\n')
    prep.session_check(raw);prep.validate_admission(admission,ih,hashlib.sha256(raw).hexdigest(),raw)
    (out/'fixture-producer.json').write_text(json.dumps(dict(physical_access=False,pmic_source=str(source),pmic_sha256=hashlib.sha256(source.read_bytes()).hexdigest()),indent=2)+'\n')
    return raw

def run(image,manifest,inputs,native,out,clang,lld):
    out.mkdir(parents=True,exist_ok=False);host=module('jpeg_runtime_host',ROOT/'tools/check-a733-jpeg-supervised.py');prep=host.preparation;results={}
    for name in ('partial-mcu','partial-mcu-oracle','vga-oracle'):
        source=inputs/name;case=out/name;case.mkdir()
        raw=fixture(image,(source/'session.bin').read_bytes(),case/'MODEL-NOT-PHYSICAL-fixture',prep)
        bundle=case/'MODEL-NOT-PHYSICAL-bundle';reference=source/'reference.yu12' if (source/'reference.yu12').stat().st_size else None
        prep.prepare(image,manifest,case/'MODEL-NOT-PHYSICAL-fixture/admission.json',case/'MODEL-NOT-PHYSICAL-fixture/session-template.bin',bundle,clang,lld,source/'input.jpg',reference)
        frozen=host.freeze_bundle(bundle);session=frozen['_frozen']['session.bin']
        frame=bytearray((native/(name+'-frame.bin')).read_bytes());struct.pack_into('<2Q',frame,16,*struct.unpack_from('<2Q',session,24))
        sent=[];record={};capture=case/'capture';capture.mkdir()
        result=host.capture(lambda packet,timeout:bytes(frame),session,capture,record,lambda:None,time.monotonic()+10,
          upload=frozen['_upload'],send_upload=sent.append,reference=frozen['_reference'])
        assert sent==[frozen['_upload']] and record['jpeg_result']=='observed-success' and result['oracle_verified']==bool(reference)
        assert (capture/'output.yu12').read_bytes()==frame[144:]
        refused=[]
        for change in ('dimensions','digest','generation','old-frame','oracle-mode'):
            bad=bytearray(frame);oracle=frozen['_reference']
            if change=='dimensions':struct.pack_into('<I',bad,128,0xffffffff)
            if change=='digest':bad[96]^=1
            if change=='generation':bad[24]^=1
            if change=='old-frame':struct.pack_into('<I',bad,4,1)
            if change=='oracle-mode':oracle=b'bad' if oracle is None else None
            try:host.decoder.decode(bad,session,oracle)
            except ValueError:refused.append(change)
            else:raise AssertionError(change)
        upload=bytearray(frozen['_upload']);upload[-1]^=1
        try:host.capture(lambda *a:bytes(frame),session,capture,{},lambda:None,time.monotonic()+10,
              upload=upload,send_upload=lambda data:(_ for _ in ()).throw(AssertionError('Bad upload transmitted')),reference=frozen['_reference'])
        except ValueError:refused.append('upload-corruption-before-send')
        else:raise AssertionError('Bad upload accepted')
        device_capture=case/'capture-device';device_capture.mkdir()
        class Device:
            # Small frames arrive in 37-byte fragments; a VGA frame in 64 KiB ones.
            def __init__(self):self.writes=[];self.remaining=bytes(frame);self.fragment=37 if len(frame)<65536 else 65536
            def write(self,endpoint,data,timeout):
                assert endpoint==2;self.writes.append(bytes(data));return len(data)
            def read(self,endpoint,maximum,timeout):
                assert endpoint==0x81;n=min(maximum,self.fragment,len(self.remaining));value=self.remaining[:n];self.remaining=self.remaining[n:];return value
        device=Device();observed={}
        result=host.capture_device(device,lambda:None,time.monotonic()+10,session,device_capture,observed,lambda:None,
          reference=frozen['_reference'],upload=frozen['_upload'])
        assert b''.join(device.writes[:-1])==frozen['_upload'] and device.writes[-1]==b'\0' and not device.remaining
        assert observed['jpeg_result']=='observed-success' and len(observed['jpeg_fragments'])>1
        results[name]=dict(passed=True,oracle=bool(reference),dimensions=[result['width'],result['height']],refused=refused,prepared_sha256=frozen['_prepared_sha256'],launcher_sha256=frozen['files']['launch.bin'])
    (out/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),cases=results,scope='Full real preparation/admission/launcher/bundle freeze with explicitly modeled authority. Host uploads frozen bytes and durably captures actual native serializer output; only randomized epoch/generation rebound. Normal and oracle modes, dimensions, identity, digest and corruption refusals covered.'),indent=2)+'\n')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('image','manifest','inputs','native','output','clang','lld'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();run(a.image,a.manifest,a.inputs,a.native,a.output,a.clang,a.lld)
