#!/usr/bin/env python3
"""Compose the new blend bundle using actual saved allocations and retained launcher.

Only physical authority is modeled. No native instructions, launcher compilation,
USB or queue executes. Source comparison gates reuse of the compiled launcher.
"""
import argparse,gzip,hashlib,importlib.util,json,pickle,shutil,struct
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def sha(data):return hashlib.sha256(data).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('native','entry','output'):p.add_argument('--'+n,type=Path,required=True)
 a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
 prep=module('g2d_blend_preparation',ROOT/'apps/g2d/prepare.py');helpers=module('g2d_blend_layout',ROOT/'checks/jpeg-encode-native.py')
 image=a.entry/'image.elf';im=helpers.Image(image);source=(a.entry/'input.argb').read_bytes();raw=(a.entry/'session.bin').read_bytes()
 state=pickle.loads(gzip.decompress((a.native/'copied.pkl.gz').read_bytes()));result=json.loads((a.native/'result.json').read_text())
 assert result['passed'] and result['submissions']==1 and state['image']==sha(image.read_bytes()) and state['session']==sha(raw)
 def read(address,size):
  for base,data in state['memory']:
   if base<=address and address-base+size<=len(data):return data[address-base:address-base+size]
  raise AssertionError(('outside retained checkpoint',hex(address),size))
 def get(address,size=8):return int.from_bytes(read(address,size),'little')
 base=im.address('g2d');f=im.layouts['g2d_application'];tf=im.layouts['a733_iommu_tables'];sf=im.layouts['a733_iommu_span'];tables=base+f['tables'];spans=[]
 for i in range(get(tables+tf['count'],4)):
  at=tables+tf['spans']+i*im.sizes['a733_iommu_span'];spans.append({k:get(at+sf[k],4 if k=='access' else 8) for k in ('iova','physical','bytes','access')})
 table=(a.native/'tables.bin').read_bytes();assert table==read(get(base+f['table']),32768)
 proof=dict(schema='a733-g2d-native-allocation-model-v1',modeled=True,physical_authority=False,image=state['image'],session=state['session'],case='source-over-solid',allocate=1,allocations=result['allocations'],spans=spans,table_physical=get(base+f['table']),table_bytes=32768,table_sha256=sha(table),rounded=get(base+f['rounded']),source_physical=get(base+f['input']),destination_physical=get(base+f['output']),rcq_physical=get(base+f['commands']),rcq_bytes=4096)
 prep.allocation_check(proof,raw,table)
 fixture=a.output/'MODEL-NOT-PHYSICAL-fixture';fixture.mkdir();entry_note=json.loads((a.entry/'MODEL-NOT-PHYSICAL.json').read_text());target={k:entry_note[k] for k in ('identity','port','image_sha256','modeled','physical_authority')}
 def record(name,value):
  data=(json.dumps(value,sort_keys=True,indent=2)+'\n').encode();path=fixture/name;path.write_bytes(data);return dict(path=str(path.resolve()),sha256=sha(data))
 note=record('MODEL-NOT-PHYSICAL.json',dict(**target,purpose=__doc__))
 refs={}
 for n in ('initial','memory','electrical','supervision'):
  path=a.entry/(n+'.json');refs[n]=dict(path=str(path.resolve()),sha256=sha(path.read_bytes()))
 platform=record('platform.json',dict(**target,schema='a733-smp-observer-platform-v1',entry_conditions_established=True,physical_coherence_result='under-test',conditions={n:note for n in ('source-backed-cpu-dsu-transition','cold-cache-and-dirty-alias-exclusion','dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded','usb-clock-power-reset-ownership','usb-device-mapping','physical-counter-continuity')}))
 def ddr(name,ranges):return record(name,dict(**target,schema='a733-ddr-reservation-v1',exclusive=True,published_cache_disabled=True,remote_writers_excluded=True,ranges=ranges,evidence=[note]))
 allocation=record('allocation.json',proof);tablepath=fixture/'page-table.bin';tablepath.write_bytes(table)
 admission=dict(**target,schema='a733-g2d-supervised-entry-v1',reservation=struct.unpack_from('<Q',raw,200)[0],session_template_sha256=sha(raw),ddr=ddr('ddr.json',[[0x41000000,0x42200000]]),g2d_ddr=ddr('g2d-ddr.json',[[0x48000000,0x79000000]]),platform=platform,allocation=allocation,page_table=dict(path=str(tablepath.resolve()),sha256=sha(table)),**refs,whole_board_power_removal=True,no_resource_reuse_before_power_removal=True,recovery=dict(mode='manual-usb-reconnect',demonstrated_fel=True,explicit_reconnect_before_resumption=True,preserve_microsd=True,one_attempt=True,evidence=[note]))
 admission_path=fixture/'admission.json';record('admission.json',admission)
 try:prep.validate_admission(admission,state['image'],sha(raw),raw)
 except ValueError as e:assert 'Modeled G2D evidence' in str(e)
 else:raise AssertionError('Modeled authority accepted')
 # These are all sources consumed by the unchanged launcher builder. Its header
 # is already the exact changed mode3 header compiled for the root SRAM cases.
 inputs={'launch.c':ROOT/'apps/g2d/launch.c','launch.S':ROOT/'platform/a733-smp-observe/launch.S','launch.ld':ROOT/'platform/a733-smp-observe/launch.ld','boot.h':ROOT/'platform/a733-smp-cached/boot.h','a733_g2d_application.h':ROOT/'include/a733_g2d_application.h','a733_board_application.h':ROOT/'include/a733_board_application.h','base-supervised-launch.c':ROOT/'platform/a733-smp-supervised/launch.c'}
 assert all((a.entry/n).read_bytes()==p.read_bytes() for n,p in inputs.items())
 assert sha((a.entry/'launch.bin').read_bytes())==json.loads((a.entry/'provenance.json').read_text())['launcher_sha256']
 calls=[]
 def reuse_launcher(out,clang,lld):
  calls.append(str(out))
  names=[*inputs,'g2d-launch-base.c','launch.bin','launch.elf','launch.c.o','launch.S.o','launch.c.d','launch.S.d','launcher-build.log','launcher-commands.json']
  for n in names:shutil.copy2(a.entry/n,out/n)
 bundle=a.output/'MODEL-NOT-PHYSICAL-bundle';manifest=a.entry.parent/'build/g2d-application/manifest.json'
 ids=iter(struct.unpack_from('<3Q',raw,24))
 with patch.object(prep,'physical_authority',lambda value:None),patch.object(prep,'build_launcher',reuse_launcher),patch.object(prep.secrets,'randbits',lambda bits:next(ids)):
  prep.prepare(image,manifest,admission_path,a.entry/'session.bin',bundle,'retained-compiler-not-invoked','retained-linker-not-invoked',a.entry/'input.argb')
 assert calls==[str(bundle)]
 assert (bundle/'session.bin').read_bytes()==raw
 assert (bundle/'contract.bin').read_bytes()==(a.entry/'contract.bin').read_bytes()
 assert (bundle/'request.bin').read_bytes()==(a.entry/'request.bin').read_bytes()
 (a.output/'result.json').write_text(json.dumps(dict(passed=True,physical_access=False,production_refused=True,allocation_source=str(a.native/'copied.pkl.gz'),allocation_proof=str(fixture/'allocation.json'),launcher_reused=True,entry_identity_preserved=True,launcher_sha256=sha((a.entry/'launch.bin').read_bytes()),bundle=str(bundle),scope=__doc__),indent=2)+'\n')
if __name__=='__main__':main()
