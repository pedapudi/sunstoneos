#!/usr/bin/env python3
"""Mock only the new NPU host profile; never discover hardware or touch queues."""
import argparse,hashlib,importlib.util,json,os,shutil,struct,tempfile
from pathlib import Path
from unittest.mock import patch
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--case',action='append',default=[],help='Run only named transfer cases; packaging validation still runs.')
p.add_argument('--interrupt',action='store_true',help='check the interrupt host (apps/npu-interrupt/host.py) and its admission record')
p.add_argument('--client-logs',type=Path,help='interrupt client logs from checks/npu-interrupt-client.py, for the receipt cases');p.add_argument('--native',type=Path,required=True);p.add_argument('--evidence',type=Path,required=True);a=p.parse_args();a.evidence.mkdir(parents=True,exist_ok=False)
r=Path(__file__).resolve().parents[2];HOST=r/('apps/npu-interrupt/host.py' if a.interrupt else 'apps/npu-finite/host.py')
spec=importlib.util.spec_from_file_location('host',HOST);h=importlib.util.module_from_spec(spec);spec.loader.exec_module(h)
# The interrupt host wraps a private copy of the finite host; check that copy.
if a.interrupt:h=h.finite
IRQ_RECEIPTS='NPU_IRQ_COMPLETIONS=0x0000000000000003\nNPU_IRQ_STOPPED=0x0000000000000001\n'
IRQ_CONDITIONS=('exclusive-level-high-SPI65','single-task-event-acknowledgment','masked-notification-and-serialized-rearm',
 'close-source-disable-before-pending-clear','inactive-pending-clear-before-return','saved-route-restored-before-enable')
def sha(data):return hashlib.sha256(data).hexdigest()
# Startup entry and READY, then each job's publication and completion, in
# counter microseconds, as the image logs them; the host's parse of them.
TIMES=(1000,5628,7000,7900,9000,9850)
assert h.timing('NPU_TIME_US=\n'+''.join(f'0x{x:016x}\n' for x in TIMES))=={'startup_us':4628,'job_us':[900,850],
    'publication_gap_us':2000,'entry_us':1000,'ready_us':5628,'jobs_raw_us':[7000,7900,9000,9850],
    'counters':None,'resolution':'Completion is observed at the service worker\'s next poll, once per 1 ms tick, or on INTID97.'}
# The job counter block: 24 MHz system counter, then per job publication,
# INTID97 and completion ticks, then NPU total and idle cycles.
COUNTERS=(24000000,2400,2640,3120,49200,1200,4800,0,5520,49200,49300)
counters=h.timing('NPU_TIME_US=\n'+''.join(f'0x{x:016x}\n' for x in TIMES)+
    'NPU_JOB_COUNTERS=\n'+''.join(f'0x{x:016x}\n' for x in COUNTERS))['counters']
assert counters=={'frequency':24000000,'jobs':[
    {'start_ticks':2400,'irq_ticks':2640,'done_ticks':3120,'start_to_irq_us':10.0,'start_to_done_us':30.0,
     'total_cycles':49200,'idle_cycles':1200,'active_cycles':48000},
    {'start_ticks':4800,'irq_ticks':0,'done_ticks':5520,'start_to_irq_us':None,'start_to_done_us':30.0,
     'total_cycles':49200,'idle_cycles':49300,'active_cycles':None}]},counters
# The board's form of an unrecorded completion: no job time, no refusal.
board=h.timing('NPU_TIME_US=\n'+''.join(f'0x{x:016x}\n' for x in (108189479,108193503,75005,0,77045,0)))
assert board['startup_us']==4024 and board['job_us']==[None,None] and board['publication_gap_us']==2040
assert h.timing('NPU_STAGE=OPEN\n') is None
def ref(path):return {'path':str(path.resolve()),'sha256':sha(path.read_bytes())}
results=[]
with tempfile.TemporaryDirectory(prefix='npu-host-fixture-') as temporary:
 t=Path(temporary);native=a.native.resolve();image=native/'image.elf';manifest=native/'manifest.json';data=image.read_bytes();parts,address,build,layout,g=h.image_profile(data,json.loads(manifest.read_bytes()));checkpoint=bool(g.get('checkpoint'));thermal=bool(g.get('thermal'));restart=bool(g.get('restart'));execution=h.geometry_profile(g)
 completions=4 if restart else 3
 IRQ_RECEIPTS=f'NPU_IRQ_COMPLETIONS=0x{completions:016x}\nNPU_IRQ_STOPPED=0x0000000000000001\n'
 if restart:
  assert a.interrupt
  restart_conditions=h.validate_admission.__globals__['RESTART_CONDITIONS']
 TAIL=min(100,len(bytes.fromhex(g['reference']))//2)
 # The reviewed 608-byte page, converted to the image's reduced record.
 spec=importlib.util.spec_from_file_location('finite_images',r/'checks/npu-image-checks.py');images=importlib.util.module_from_spec(spec);spec.loader.exec_module(images)
 base_layout=(456,224,232,*layout[3:]) if thermal else layout
 raw=bytearray(images.npu_page((Path.home()/'.cache/sunstoneos/evidence/npu-single-core-20261018/startup-usb-resume/session-fixture.bin').read_bytes(),base_layout,h.original.SINGLE_CORE_VERSION))
 if thermal:raw+=bytes(160);struct.pack_into('<Q',raw,16,616)
 raw[48:80]=build;profile=h.original.session_profile(raw,layout);boot,boot_bytes=layout[1:3]
 # Read the report identity from the actual linked monitor initializer.
 # Only contract admission and architectural time are modeled. Stop before seal.
 from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
 from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
 spec=importlib.util.spec_from_file_location('finite_report_layout',r/'checks/harness.py')
 harness=importlib.util.module_from_spec(spec);spec.loader.exec_module(harness)
 native_image=harness.Image(image,layout_names=('a733_smp_contract','npu_convolution','a733_npu_native','a733_npu_memory','a733_npu_reserved','device_memory_extent'));u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
 u.mem_map(0x02000000,0x07000000);u.mem_map(0x40000000,0x04000000)
 for at,value in native_image.parts:
  if value:u.mem_write(at,value)
 contract=native_image.layouts['a733_smp_contract']
 for key in ('epoch','generation','lease'):
  u.mem_write(0x42004000+contract[key],struct.pack('<Q',profile[key]))
 if g.get('stopped_capture'):
  # Independently resolve the audited fixed reads through actual DWARF fields.
  import io
  from elftools.elf.elffile import ELFFile
  elf=ELFFile(io.BytesIO(data));app_fields=[]
  for cu in elf.get_dwarf_info().iter_CUs():
   if not cu.get_top_DIE().attributes['DW_AT_name'].value.decode().endswith('/apps/npu-convolution/native.c'):continue
   for die in cu.iter_DIEs():
    if die.tag=='DW_TAG_variable' and die.attributes.get('DW_AT_name') and die.attributes['DW_AT_name'].value==b'app':
     app_fields.append({x.attributes['DW_AT_name'].value.decode():int(x.attributes['DW_AT_data_member_location'].value) for x in die.get_DIE_from_attribute('DW_AT_type').iter_children() if x.tag=='DW_TAG_member' and 'DW_AT_name' in x.attributes})
  assert len(app_fields)==1
  reserved=native_image.address('app',unit='apps/npu-convolution/native.c')+app_fields[0]['reserved'];layouts=native_image.layouts
  convolution=reserved+layouts['a733_npu_reserved']['convolution'];cl=layouts['npu_convolution']
  memory=convolution+cl['native']+layouts['a733_npu_native']['memory'];ml=layouts['a733_npu_memory']
  expected=[('lengths-buffers',convolution+cl['model_bytes'],cl['stream_bytes']+16-cl['model_bytes']),
   ('extent',reserved+layouts['a733_npu_reserved']['extent'],24),('memory',memory+ml['count'],ml['image']+24-ml['count'])]
  assert expected==list(h.stopped.METADATA)
  assert cl['buffers']-cl['model_bytes']==32 and cl['stream_bytes']-cl['model_bytes']==352
  results.append({'name':'actual-ELF-DWARF-binds-stopped-metadata-ranges','passed':True})
  assert h.stopped.layout('0'*64,None,None,None,None) is None
 observed={'seal':False}
 def initialize(cpu,at,size,cookie):
  if at==native_image.address('a733_smp_contract_valid'):
   u.reg_write(UC_ARM64_REG_X0,1);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR));return
  if at==native_image.address('npu_single_core_seal'):
   observed['seal']=True;u.emu_stop();return
  word=int.from_bytes(u.mem_read(at,4),'little')
  if word&0xffffffe0==0xd53be020:
   u.reg_write(UC_ARM64_REG_X0+(word&31),11000000);u.reg_write(UC_ARM64_REG_PC,at+4)
 u.hook_add(UC_HOOK_CODE,initialize);u.reg_write(UC_ARM64_REG_SP,0x43ffe000);u.reg_write(UC_ARM64_REG_LR,0x43fff000)
 u.emu_start(native_image.address('sd_single_core_monitor_init'),0x43fff000,count=1000000)
 assert observed['seal'];initialized=bytes(u.mem_read(0x42000000,128));native_report=struct.unpack('<16Q',initialized)
 assert native_report[1:4]==(profile['epoch'],profile['generation'],1)
 (a.evidence/'native-initial-report.bin').write_bytes(initialized)
 results.append({'name':'actual-native-report-initializer','passed':True,'magic':hex(native_report[0]),'sha256':sha(initialized)})
 fact=t/'modeled-fact.txt';fact.write_text('Software-only mocked host evidence. No physical observation or live target.\n')
 # An impossible test port prevents these temporary records matching a live board.
 identity=[0,0,0,0,1];port='mock-npu-port';common={'image_sha256':sha(data),'identity':identity,'port':port,'whole_platform':profile['whole_platform']}
 def write(name,value):path=t/name;path.write_text(json.dumps(value,indent=2)+'\n');return path
 required={'clock':{'independent-dram-phy-sources','exclusive-pll-npu-consumers-and-writers','no-autonomous-parent-selector-writers','supervised-cross-pll-disturbance-risk'},'power':{'dcdc2-stable-800mv','authenticated-pmic-pin-inheritance'},'supervision':{'manual-whole-board-power-removal','stop-after-first-failure','successful-close-stop-quiescence-ROM-retirement','failure-retain-until-whole-power-removal','DRAM-restore-only-after-validated-normal-return','no-old-SRAM-restore'}}
 thermal_facts={}
 if thermal:
  required['supervision'].remove('successful-close-stop-quiescence-ROM-retirement')
  required['supervision'].add('matched-close-local-stop-or-proved-never-started-with-configured-THS-stop-and-provider-park')
  at=boot+232;struct.pack_into('<2Q',raw,at,0x12345,0x23456);struct.pack_into('<2IB',raw,at+144,1,12000000,1)
  conditions={
   'initial':('exclusive_ths_and_irq71','remote_writers_excluded','no_inherited_thermal_service','security_access_authorized','gic_route_owned','boot_lineage_reviewed'),
   'electrical':('avcc_adc_digital_supplies_stable','sid_shadows_readable','device_mappings_verified','no_physical_aliases'),
   'clocks':('stable_apb1_ref_osc','no_clock_reset_writers','gpadc_consumers_excluded'),
   'supervision':('whole_board_power_removal','failure_permanent_retention','no_reuse','one_attempt','configured_sensor_stop_retains_clocks','normal_return_after_provider_quiescence')}
  for index,(name,keys) in enumerate(conditions.items()):
   path=write('thermal-'+name+'.json',{**common,'schema':'a733-thermal-'+name+'-v1','reviewed':True,'owner':0x12345,'authority':0x23456,**dict.fromkeys(keys,True)})
   thermal_facts[name]=ref(path);raw[at+16+32*index:at+48+32*index]=hashlib.sha256(path.read_bytes()).digest()
 if checkpoint:
  required['clock']=({'read-only-NPU-clock-guard-before-MBUS-normalization','MBUS-source-restore-with-idle-masters-and-exclusive-SRAM-window'} if g.get('checkpoint_mbus') else {'npu-clock-power-registers-not-accessed'})
  required['power']={'providers-never-constructed','unconsumed-typed-boot-no-provider-grant'}
  required['supervision'].remove('successful-close-stop-quiescence-ROM-retirement');required['supervision'].add('checkpoint-normal-return-retirement')
 boot_hash=sha(raw[boot:boot+boot_bytes]);facts={}
 for name,offset in [('clock',112),('power',144),('supervision',176)]:
  value={**common,'conditions':{k:ref(fact) for k in required[name]},'boot_sha256':boot_hash,'supply_uv':800000,'supervisor':profile['supervisor'],'one_attempt':True};facts[name]=write(name+'.json',value);raw[offset:offset+32]=hashlib.sha256(facts[name].read_bytes()).digest()
 ddr=write('ddr.json',{**common,'schema':'a733-ddr-reservation-v1','ranges':[[0x41000000,0x42200000]],'exclusive':True,'published_cache_disabled':True,'remote_writers_excluded':True,'evidence':[ref(fact)]})
 conditions={'cold-cache-and-dirty-alias-exclusion','dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded','usb-clock-power-reset-ownership','physical-counter-continuity','npu-startup-sram-confinement-drain-ownership','pmic-bus-rail-pin-ownership','primary-cache-transition-and-secondary-off','usb-supplies-stable-and-writers-excluded'}
 if checkpoint:
  conditions-= {'npu-startup-sram-confinement-drain-ownership','pmic-bus-rail-pin-ownership'}
  conditions|= {'providers-never-constructed','read-only-NPU-clock-guard-before-MBUS-normalization' if g.get('checkpoint_mbus') else 'npu-clock-power-registers-not-accessed'}
 platform=write('platform.json',{**common,**profile,'schema':'a733-npu-checkpoint-platform-v1' if checkpoint else 'a733-npu-convolution-platform-v1','conditions':{k:ref(fact) for k in conditions},'entry_conditions_established':True,'physical_coherence_result':'under-test'})
 launcher=Path.home()/'.cache/sunstoneos/evidence/npu-single-core-20261018/package-final/bundle/launch.bin'
 review=write('root.json',{**common,'schema':'a733-npu-finite-return-review-v1','reviewed':True,'execution_profile':execution,'build_id':build.hex(),'supervisor':profile['supervisor'],'launcher_sha256':sha(launcher.read_bytes()),'host_sources':{n:sha((r/n).read_bytes()) for n in h.HOST_SOURCES},'conditions':{k:ref(fact) for k in ('architecture-reset-reviewed','software-handoff-reviewed','normal-return-retirement-reviewed','failure-retention-reviewed')}})
 entry={**common,**profile,'schema':h.SCHEMA,'execution_profile':execution,'boot_sha256':boot_hash,'ddr':ref(ddr),'platform':ref(platform),'root_review':ref(review),**{k:ref(v) for k,v in facts.items()}}
 if thermal:entry['thermal']=thermal_facts
 if a.interrupt:
  irq=write('interrupt.json',{**common,'schema':'a733-npu-interrupt-entry-v1','build_id':build.hex(),'interrupt':97,'cpu':0,'conditions':{k:ref(fact) for k in IRQ_CONDITIONS}})
  entry['interrupt']=ref(irq)
 if restart:
  restart_record=write('restart.json',{**common,**profile,'schema':'a733-npu-normal-restart-entry-v1',
   'build_id':build.hex(),'reviewed':True,'restart_qualification':'under-test','conditions':{k:ref(fact) for k in restart_conditions}})
  entry['restart']=ref(restart_record)
 admission=write('admission.json',entry);raw[80:112]=hashlib.sha256(admission.read_bytes()).digest();session=t/'session.bin';session.write_bytes(raw)
 boot_path=t/'typed-boot.bin';boot_path.write_bytes(raw[boot:boot+boot_bytes])
 constructed=t/'constructed-session.bin';h.construct_session(image,manifest,boot_path,admission,constructed)
 assert constructed.read_bytes()==session.read_bytes();results.append({'name':'explicit-session-construction','passed':True})
 for label,change in [('boot-hash',lambda:boot_path.write_bytes(bytes(boot_bytes))),
                      ('build',lambda:write('admission.json',{**entry,'image_sha256':'f'*64})),
                      ('identity',lambda:write('admission.json',{**entry,'npu_owner':entry['npu_owner']+1}))]:
  saved_boot=boot_path.read_bytes();saved_entry=admission.read_bytes();change();target=t/(label+'.bin')
  try:h.construct_session(image,manifest,boot_path,admission,target)
  except ValueError:assert not target.exists()
  else:raise AssertionError(label+' admitted')
  boot_path.write_bytes(saved_boot);admission.write_bytes(saved_entry);results.append({'name':'session-'+label+'-refused-before-output','passed':True})
 try:h.construct_session(image,manifest,boot_path,admission,constructed)
 except ValueError:assert constructed.read_bytes()==session.read_bytes()
 else:raise AssertionError('existing output overwritten')
 results.append({'name':'session-existing-output-preserved','passed':True})
 # Launcher compilation is unchanged and uses preserved accepted bytes here.
 def launcher_copy(output,*args):shutil.copy2(launcher,output/'launch.bin')
 with patch.object(h.original,'build_launcher',launcher_copy):h.prepare(image,manifest,session,admission,t/'bundle','mock-compiler','mock-linker','mock-objcopy')
 bundle=t/'bundle';prepared=h.validate_bundle(bundle);results.append({'name':'original-producers-valid-returning-bundle','passed':True})
 def rejected(name,fn):
  try:fn()
  except (ValueError,KeyError):results.append({'name':name,'passed':True});return
  raise AssertionError(name+' accepted')
 for name,key,value in [('retained-profile-refused','schema','a733-npu-supervised-entry-v1'),('other-image-refused','image_sha256','f'*64),('other-profile-refused','execution_profile','npu-single-core-supervised-v1')]:
  modified={**entry,key:value};encoded=(json.dumps(modified)+'\n').encode();mutated=bytearray(raw);mutated[80:112]=hashlib.sha256(encoded).digest();rejected(name,lambda:h.validate_admission(encoded,mutated,build,layout,sha(data),execution_profile=execution))
 saved=fact.read_bytes();fact.write_bytes(saved+b'changed');h.validate_bundle(bundle);fact.write_bytes(saved);results.append({'name':'frozen-entry-independent-of-external-file-mutation','passed':True})
 frozen_fact=bundle/prepared['entry_evidence'][str(fact.resolve())]['name'];saved=frozen_fact.read_bytes();frozen_fact.write_bytes(saved+b'changed');rejected('changed-frozen-entry-refused',lambda:h.validate_bundle(bundle));frozen_fact.write_bytes(saved)
 # Negative ELF contains a defined function renamed to an existing USB
 # filename string. The real image's STT_FILE entries alone remain accepted.
 import io
 from elftools.elf.elffile import ELFFile
 elf=ELFFile(io.BytesIO(data));table=elf.get_section_by_name('.symtab');syms=list(table.iter_symbols())
 usb_name=next(x['st_name'] for x in syms if x['st_info']['type']=='STT_FILE' and 'usb' in x.name.lower())
 index=next(i for i,x in enumerate(syms) if x['st_info']['type']=='STT_FUNC' and x.name=='platform_hex')
 altered=bytearray(data);struct.pack_into('<I',altered,table['sh_offset']+index*table['sh_entsize'],usb_name)
 rejected('linked-native-usb-function-refused',lambda:h.image_profile(bytes(altered),{**json.loads(manifest.read_bytes()),'final_elf':sha(altered)}))
 saved_platform=platform.read_bytes()
 for key in ('usb-supplies-stable-and-writers-excluded',)+(() if checkpoint else ('npu-startup-sram-confinement-drain-ownership','pmic-bus-rail-pin-ownership')):
  changed=json.loads(saved_platform);del changed['conditions'][key];platform.write_text(json.dumps(changed))
  encoded=json.dumps({**entry,'platform':ref(platform)}).encode();mutated=bytearray(raw);mutated[80:112]=hashlib.sha256(encoded).digest()
  rejected('finite-missing-'+key,lambda:h.validate_admission(encoded,mutated,build,layout,sha(data),execution_profile=execution))
 platform.write_bytes(saved_platform)
 if checkpoint:
  rejected('checkpoint-cannot-use-numerical-profile',lambda:h.image_profile(data,{**json.loads(manifest.read_bytes()),'execution_profile':h.PROFILE}))
  saved_platform=platform.read_bytes()
  for key in ('remote-writers-excluded','providers-never-constructed','read-only-NPU-clock-guard-before-MBUS-normalization' if g.get('checkpoint_mbus') else 'npu-clock-power-registers-not-accessed'):
   changed=json.loads(saved_platform);del changed['conditions'][key];platform.write_text(json.dumps(changed))
   encoded=json.dumps({**entry,'platform':ref(platform)}).encode();mutated=bytearray(raw);mutated[80:112]=hashlib.sha256(encoded).digest()
   rejected('checkpoint-missing-'+key,lambda:h.validate_admission(encoded,mutated,build,layout,sha(data),execution_profile=execution))
  platform.write_bytes(saved_platform)
 if thermal:
  for label,at,value in [('owner',boot+232,profile['npu_owner']),('APB-rate',boot+232+148,1),('padding',boot+232+153,1)]:
   bad=bytearray(raw);struct.pack_into('<Q' if label=='owner' else '<I' if label=='APB-rate' else '<B',bad,at,value)
   changed={**entry,'boot_sha256':sha(bad[boot:])};power=json.loads(facts['power'].read_bytes());power['boot_sha256']=changed['boot_sha256'];other=write('bad-power.json',power);changed['power']=ref(other);bad[144:176]=hashlib.sha256(other.read_bytes()).digest()
   encoded=json.dumps(changed).encode();bad[80:112]=hashlib.sha256(encoded).digest()
   rejected('thermal-'+label+'-refused',lambda:h.validate_admission(encoded,bad,build,layout,sha(data),execution_profile=execution))
  changed={**entry,'thermal':{k:v for k,v in thermal_facts.items() if k!='electrical'}};encoded=json.dumps(changed).encode();bad=bytearray(raw);bad[80:112]=hashlib.sha256(encoded).digest()
  rejected('missing-thermal-electrical-authority',lambda:h.validate_admission(encoded,bad,build,layout,sha(data),execution_profile=execution))
 saved=(bundle/'request.bin').read_bytes();(bundle/'request.bin').write_bytes(bytes(len(saved)));rejected('changed-packet-refused',lambda:h.validate_bundle(bundle));(bundle/'request.bin').write_bytes(saved)
 def reports(variant):
  status=int(variant in ('mismatch','unwritten-second','unwritten-tail') or variant.startswith('capture-') or thermal and variant=='restore-readback-failure');reference=bytes.fromhex(g['reference']);first=hashlib.sha256(reference).digest();second=reference
  if status:first=bytes([first[0]^1])+first[1:]
  if variant=='false-success':second=bytes([second[0]^1])+second[1:]
  # The second job wrote nothing, or only its first TAIL bytes (100, or half
  # of a shorter reference such as ShuffleNetV2's two bytes), after a
  # matching first job; the copy holds the SUBMIT pattern there.
  if variant=='unwritten-second':first=hashlib.sha256(reference).digest();second=h.output_poison(len(reference))
  if variant=='unwritten-tail':first=hashlib.sha256(reference).digest();second=reference[:TAIL]+h.output_poison(len(reference))[TAIL:]
  jobs=2;reason=0
  if thermal and variant in ('thermal-zero','thermal-one','thermal-one-mismatch'):
   jobs=int(variant!='thermal-zero');reason=2;status=1 if variant=='thermal-one-mismatch' else 2
   if jobs==0:first=bytes(32);second=bytes(len(reference))
   elif status==1:second=bytes([reference[0]^1])+reference[1:];first=hashlib.sha256(second).digest()
  text='NPU_STAGE=WAIT_STARTUP\nNPU_STAGE=QUIESCENT\nNPU_FIRST_DIGEST=\n'+''.join(f'0x{x:016x}\n' for x in first)+'NPU_SECOND_OUTPUT=\n'+''.join(f'0x{x:016x}\n' for x in second)+(('PASS NPU two matched jobs across normal local restart and parked providers' if restart and not status else 'REFUSED NPU thermal admission; configured sensor disabled and providers parked' if status==2 else 'FAIL NPU numerical comparison' if status else 'PASS NPU two matched jobs, copied output, REAP, CLOSE, local stop and parked providers')+'\n')
  # The image's startup and job times precede the thermal markers.
  text=text.replace('NPU_STAGE=WAIT_STARTUP\n','NPU_STAGE=WAIT_STARTUP\nNPU_TIME_US=\n'+''.join(f'0x{x:016x}\n' for x in TIMES),1)
  if thermal:text=text.replace('NPU_STAGE=QUIESCENT',f'NPU_THERMAL_REASON=0x{reason:016x}\nNPU_JOBS=0x{jobs:016x}\nNPU_STAGE=QUIESCENT')
  if restart:text=text.replace('NPU_STAGE=WAIT_STARTUP\n',
   'NPU_STAGE=WAIT_STARTUP\nNPU_STAGE=OPEN\nNPU_STAGE=SUBMIT job=0x0000000000000001\nNPU_STAGE=FIRST_CLOSE\nNPU_STAGE=RESTART_OPEN\nNPU_STAGE=SUBMIT job=0x0000000000000002\nNPU_STAGE=CLOSE\nNPU_STAGE=WAIT_LOCAL_STOP\n',1)
  if a.interrupt:text=text.replace('NPU_STAGE=QUIESCENT\n',IRQ_RECEIPTS+'NPU_STAGE=QUIESCENT\n',1)
  if checkpoint:
   text='NPU_CHECKPOINT=ENTRY_MEMORY_READY; providers never constructed; jobs=0\n'
   if g.get('checkpoint_mbus'):text+='NPU_MBUS=\n0x0000000084000000\n0x0000000000000000\n0x0000000081000000\n'
   if variant=='false-success':text=text.replace('jobs=0','jobs=1')
   if variant=='missing-MBUS':text=text.split('NPU_MBUS=')[0]
   if variant=='truncated-MBUS':text=text.rsplit('0x0000000081000000\n',1)[0]
   if variant=='bad-MBUS-result':text=text.replace('NPU_MBUS=\n0x0000000084000000\n0x0000000000000000','NPU_MBUS=\n0x0000000084000000\n0x0000000000000001')
   if variant=='duplicate-MBUS':text+=text[text.index('NPU_MBUS='):]
  if variant=='duplicate-log':text+=text
  monitor=list(native_report);monitor[3:9]=[5,status,0x4e50554e if checkpoint else 0,11000000,1,12000000]
  if variant=='old-generic-monitor-magic':monitor[0]=0x4153373343525331
  header=[0x534b4552,5,status,len(text),0,0,0,0,11500000,12000000]+[0]*6
  receipt=[0x4e505543484b3031 if checkpoint else 0x4e50555448533031 if thermal else 0x4e50555245543031,profile['epoch'],profile['generation'],profile['lease'],status,0x4e50554e if checkpoint else status+1,g['clean_start'],g['clean_end'],(g['clean_end']-g['clean_start'])//64,490000000,len(text)]
  if thermal:receipt += [reason,jobs,12000000 if variant=='thermal-late-drain' else 130000000]
  if variant=='thermal-fault-reason':receipt[11]=4
  if variant=='ordinary-receipt':receipt[0]=0x4e50555245543031;receipt[5]=1;monitor[5]=0
  if variant=='stale-generation':monitor[2]+=1
  if variant=='stale-lease':receipt[3]+=1
  if variant=='unclean-receipt':receipt[8]-=1
  if variant=='pending-monitor':monitor[3]=4
  if variant=='wrong-status':header[2]=1
  if variant=='invalid-time':monitor[8]=490000000
  if variant=='oversized-log':header[3]=20000
  plat=bytearray(16384);struct.pack_into('<16Q',plat,0,*header);plat[256:256+len(text)]=text.encode()
  return struct.pack('<16Q',*monitor),bytes(plat),struct.pack('<14Q' if thermal else '<11Q',*receipt)
 # Real preload/sealed-source/readback logic; only FEL operations and new-ROM event modeled.
 variants=['nominal','old-generic-monitor-magic','mismatch','unwritten-second','unwritten-tail','stale-generation','stale-lease','unclean-receipt','pending-monitor','wrong-status','invalid-time','oversized-log','false-success','duplicate-log','no-new-ROM','capture-transport-failure','restore-readback-failure','changed-backup','wrong-ROM-entry']
 if g.get('stopped_capture'):variants+=['capture-metadata-invalid','capture-parent-short','capture-extent-transport-failure','capture-durability-failure','capture-output-differs']
 if checkpoint or thermal:variants.append('ordinary-receipt')
 if g.get('checkpoint_mbus'):variants+=['missing-MBUS','truncated-MBUS','bad-MBUS-result','duplicate-MBUS']
 if thermal:variants+=['thermal-zero','thermal-one','thermal-one-mismatch','thermal-late-drain','thermal-fault-reason']
 assert not set(a.case)-set(variants),'Unknown selected case'
 for variant in variants:
  if a.case and variant not in a.case:continue
  out=t/variant;out.mkdir();state={'calls':[],'failed':False,'new_ROM':False,'restore_writes':0,'writes':[]};memory=bytearray(h.DRAM[1]);sram=bytearray(0x100000);record={'mode':'kernel','launch_attempted':False,'dirty':False};p2=h.validate_bundle(bundle);frame=reports(variant)
  def readmem(address,size):
   if h.DRAM[0]<=address<address+size<=h.DRAM[0]+h.DRAM[1]:return bytes(memory[address-h.DRAM[0]:address-h.DRAM[0]+size])
   if 0<=address<address+size<=len(sram):return bytes(sram[address:address+size])
   return bytes(size)
  def transaction(*args,pass_fds=()):
   assert not state['failed'],'Request after transport failure';state['calls'].append(str(args[0]))
   i=0;words=[]
   while i<len(args):
    op=args[i];i+=1
    if op=='version':assert state['new_ROM'];words.append('soc=00001903(A733)');continue
    if op=='exe':
     i+=1
     for address,data in zip((0x42000000,g['log'],g['receipt']),frame):memory[address-h.DRAM[0]:address-h.DRAM[0]+len(data)]=data
     plan=g.get('stopped_capture')
     if plan and struct.unpack('<16Q',frame[0])[4]==1:
      header=bytearray(368);struct.pack_into('<3QI',header,0,2056,432,400,2)
      for index,(offset,size,device,role) in enumerate(h.stopped.SLOTS):struct.pack_into('<QQQII',header,32+32*index,h.stopped.PARENT+offset,h.stopped.PARENT+offset,size,device,role)
      struct.pack_into('<2Q',header,352,136,208)
      if variant=='capture-metadata-invalid':struct.pack_into('<Q',header,32,0x03600000)
      extent=struct.pack('<3Q',h.stopped.PARENT,h.stopped.PARENT,h.stopped.BYTES)
      arena=h.stopped.ARENA;mem=struct.pack('<5Q2I',6,arena,20480,arena,20480,(arena>>8)|1,1)
      for item,value in zip(plan['metadata'],(header,extent,mem)):
       offset=item['address']-h.DRAM[0];memory[offset:offset+len(value)]=value
      parent=bytearray([0xa5])*h.stopped.BYTES;reference=bytes.fromhex(g['reference']);parent[0x83000:0x83000+len(reference)]=reference
      if variant=='capture-output-differs':parent[0x83000]^=1
      offset=h.stopped.PARENT-h.DRAM[0];memory[offset:offset+len(parent)]=parent
     if variant=='changed-backup':(out/'before-DRAM.bin').write_bytes(b'x'*h.DRAM[1])
     continue
    address=int(args[i],0);i+=1
    if op=='readl':
     assert address!=0x03000024,'Normal boot GPIO is not a forced-FEL precondition'
     rom={0x20:0xeb00008f,0x24:0xe3a00911,0x28:0xe3a0193d,0x3c:0xe3a0605c,0x40:0xea00004d}
     if address in rom:assert all((out/('before-'+name+'.bin')).exists() for name,_,_ in h.base.REGIONS)
     words.append(hex(rom.get(address,0) ^ (1 if variant=='wrong-ROM-entry' and address==0x3c else 0)));continue
    if op=='write':
     if state['new_ROM'] and g.get('stopped_capture') and struct.unpack('<16Q',frame[0])[4]==1:assert record.get('stopped_capture',{}).get('complete')
     source=Path(args[i]);i+=1;data=source.read_bytes()
     state['writes'].append((address,len(data),state['new_ROM']))
     if len(data)==h.DRAM[1]:state['restore_writes']+=1
     if 0<=address<address+len(data)<=len(sram):sram[address:address+len(data)]=data
     if h.DRAM[0]<=address<address+len(data)<=h.DRAM[0]+h.DRAM[1]:memory[address-h.DRAM[0]:address-h.DRAM[0]+len(data)]=data
     continue
    assert op=='read';size=int(args[i]);path=Path(args[i+1]);i+=2
    if variant=='capture-transport-failure' and state['new_ROM'] and address==g['receipt']:state['failed']=True;record['transport_failed']=True;raise ValueError('Modeled transport loss')
    if path.name.startswith('npu-stopped-'):
     assert record.get('npu_result',{}).get('normal_return') and record['npu_result']['status']==1
     state.setdefault('capture_reads',[]).append([address,size])
     assert not state['restore_writes'] and 'controlled' not in record
     if variant=='capture-extent-transport-failure' and path.name=='npu-stopped-extent.bin':
      path.write_bytes(b'partial');state['failed']=True;record['transport_failed']=True;raise ValueError('Modeled capture transport loss')
    value=readmem(address,size)
    if variant=='capture-parent-short' and path.name=='npu-stopped-parent.bin':value=value[:-1]
    if state['new_ROM'] and size==h.DRAM[1] and variant=='restore-readback-failure':value=bytes([1])+value[1:]
    path.write_bytes(value)
   return '\n'.join(words)
  def wait_return():
   if variant=='no-new-ROM':raise ValueError('Modeled return deadline')
   state['new_ROM']=True
  failure=None
  real_sync=h.base.sync_file
  def sync(path):
   if variant=='capture-durability-failure' and path.name=='npu-stopped-lengths-buffers.bin':raise OSError('Modeled capture durability loss')
   return real_sync(path)
  try:
   with patch.object(h.base,'sync_file',sync):result=h.run_once(transaction,wait_return,lambda:None,bundle,p2,out,record,lambda:None)
  except (ValueError,OSError) as error:failure=error
  normal=variant in ('nominal','capture-output-differs') or variant in ('mismatch','unwritten-second','unwritten-tail') and not checkpoint or thermal and variant in ('thermal-zero','thermal-one','thermal-one-mismatch');assert (failure is None)==normal,(variant,failure)
  if normal:
   assert record['complete'] and record['dram_restored'] and record['ownership_release_permitted'] and not record['sram_restored']
   assert result['functional_success']==(variant=='nominal' and not checkpoint) and result['controlled_negative']==(variant in ('mismatch','unwritten-second','unwritten-tail','thermal-one-mismatch','capture-output-differs'))
   if not checkpoint:
    size=len(bytes.fromhex(g['reference']));pattern=result['output_pattern']
    assert not pattern['first_unwritten']
    if variant=='unwritten-second':assert pattern['second_unwritten'] and pattern['second_pattern_ranges']==[[0,size]],pattern
    elif variant=='unwritten-tail':
     assert not pattern['second_unwritten'] and pattern['second_pattern_ranges'][-1]==[TAIL,size],pattern
     assert pattern['second_pattern_bytes']-(size-TAIL)<=4,pattern
    else:assert not pattern['second_unwritten'],pattern
   assert state['restore_writes']==1
   if result['status']:
    kernel=h.module('check_controlled_outcome',r/'tools/check-a733-kernel.py');assert kernel.controlled_return(record)
   else:assert 'controlled' not in record
  else:
   assert not record.get('complete') and not record.get('dram_restored') and 'controlled' not in record
   if variant=='wrong-ROM-entry':assert 'exe' not in state['calls'] and 'Forced-FEL ROM entry' in str(failure)
   assert state['restore_writes']==int(variant=='restore-readback-failure'),variant
   held=[];h.retain_failure(record,lambda:None,lambda:held.append('quarantine'),failure,hold=lambda:held.append('hold'));assert held==['quarantine','hold'],(variant,held)
  if g.get('stopped_capture'):
   capture=record.get('stopped_capture')
   if variant in ('mismatch','capture-output-differs'):
    assert capture and capture['complete'] and len(state['capture_reads'])==5
    assert capture['output_matches_returned_copy']==(variant=='mismatch')
    for item in capture['files']:assert sha((out/item['name']).read_bytes())==item['sha256']
   elif variant in ('nominal','invalid-time','capture-transport-failure'):assert capture is None and not state.get('capture_reads')
   elif variant.startswith('capture-') and variant!='capture-transport-failure':
    assert capture and not capture['complete'] and not state['restore_writes']
    assert len(state['capture_reads'])=={'capture-metadata-invalid':3,'capture-parent-short':4,'capture-extent-transport-failure':2,'capture-durability-failure':1}[variant]
   if state.get('capture_reads'):
    expected=[[item['address'],item['bytes']] for item in g['stopped_capture']['metadata']]+[[h.stopped.PARENT,h.stopped.BYTES],list(h.stopped.SRAM)]
    assert state['capture_reads']==expected[:len(state['capture_reads'])]
  assert all(address>=h.DRAM[0] for address,size,after_ROM in state['writes'] if after_ROM)
  results.append({'name':variant,'passed':True,'restore_writes':state['restore_writes'],'retained':not normal,'functional_success':record.get('npu_result',{}).get('functional_success'),'capture_reads':state.get('capture_reads',[]),'stopped_capture':record.get('stopped_capture')})
  if record.get('stopped_capture'):
   preserved=a.evidence/variant;preserved.mkdir()
   for path in out.glob('npu-stopped-*.bin'):shutil.copyfile(path,preserved/path.name)
 if a.interrupt:
  # The interrupt record is required and binds the route; a polled entry is refused.
  for name,value in [('missing-IRQ-admission',{k:v for k,v in entry.items() if k!='interrupt'}),
                     ('polled-entry-refused',{**entry,'execution_profile':'npu-finite-return-v1'})]:
   encoded=json.dumps(value).encode();mutated=bytearray(raw);mutated[80:112]=hashlib.sha256(encoded).digest()
   rejected(name,lambda:h.validate_admission(encoded,mutated,build,layout,sha(data),execution_profile=execution))
  saved_irq=irq.read_bytes();bad=json.loads(saved_irq);bad['interrupt']=98;irq.write_text(json.dumps(bad))
  encoded=json.dumps({**entry,'interrupt':ref(irq)}).encode();mutated=bytearray(raw);mutated[80:112]=hashlib.sha256(encoded).digest()
  rejected('other-route-refused',lambda:h.validate_admission(encoded,mutated,build,layout,sha(data),execution_profile=execution));irq.write_bytes(saved_irq)
  if restart:
   for name,changed in [('missing-restart-admission',{k:v for k,v in entry.items() if k!='restart'})]:
    encoded=json.dumps(changed).encode();mutated=bytearray(raw);mutated[80:112]=hashlib.sha256(encoded).digest()
    rejected(name,lambda:h.validate_admission(encoded,mutated,build,layout,sha(data),execution_profile=execution))
   saved_restart=restart_record.read_bytes()
   for key,value in [('npu_owner',profile['npu_owner']+1),('generation',profile['generation']+1),
                     ('restart_qualification','qualified'),('reviewed',False),('conditions',{})]:
    changed=json.loads(saved_restart);changed[key]=value;restart_record.write_text(json.dumps(changed))
    encoded=json.dumps({**entry,'restart':ref(restart_record)}).encode();mutated=bytearray(raw);mutated[80:112]=hashlib.sha256(encoded).digest()
    rejected('restart-'+key+'-refused',lambda:h.validate_admission(encoded,mutated,build,layout,sha(data),execution_profile=execution))
   restart_record.write_bytes(saved_restart)
   rejected('restart-image-relabel-refused',lambda:h.image_profile(data,{**json.loads(manifest.read_bytes()),'execution_profile':h.PROFILE}))
  # Receipts in logs that the actual client produced (checks/npu-interrupt-client.py).
  for variant in ('matched','mismatch','missing-completion','missing-stop','wrong-completion','duplicate-completion',*(('missing-restart-stage','reordered-restart-stage') if restart else ())) if a.client_logs else ():
   text=(a.client_logs/('mismatch.log' if variant=='mismatch' else 'matched.log')).read_text();status=int(variant=='mismatch')
   if variant=='missing-completion':text=text.replace(f'NPU_IRQ_COMPLETIONS=0x{completions:016x}\n','')
   if variant=='missing-stop':text=text.replace('NPU_IRQ_STOPPED=0x0000000000000001\n','')
   if variant=='wrong-completion':text=text.replace(f'NPU_IRQ_COMPLETIONS=0x{completions:016x}','NPU_IRQ_COMPLETIONS=0x0000000000000002')
   if variant=='missing-restart-stage':text=text.replace('NPU_STAGE=FIRST_CLOSE\n','')
   if variant=='reordered-restart-stage':text=text.replace('NPU_STAGE=FIRST_CLOSE\nNPU_STAGE=RESTART_OPEN\n','NPU_STAGE=RESTART_OPEN\nNPU_STAGE=FIRST_CLOSE\n')
   if variant=='duplicate-completion':text='NPU_IRQ_COMPLETIONS=0x0000000000000002\n'+text
   monitor=list(native_report);monitor[3:9]=[5,status,0,11000000,1,12000000]
   header=[0x534b4552,5,status,len(text),0,0,0,0,11500000,12000000]+[0]*6
   receipt=[0x4e50555245543031,profile['epoch'],profile['generation'],profile['lease'],status,status+1,g['clean_start'],g['clean_end'],(g['clean_end']-g['clean_start'])//64,490000000,len(text)]
   plat=bytearray(16384);struct.pack_into('<16Q',plat,0,*header);plat[256:256+len(text)]=text.encode()
   frames=struct.pack('<16Q',*monitor),bytes(plat),struct.pack('<11Q',*receipt)
   if variant in ('matched','mismatch'):
    value=h.validate_reports(*frames,prepared)
    assert value['functional_success']==(variant=='matched') and value['interrupt_completions']==completions and value['interrupt_stopped']
    assert value['timing']['counters'] is not None,value['timing']
    if restart:assert value['normal_restart'] and 'startup_us' not in value['timing'] and 'initial_entry_to_restart_ready_us' in value['timing']
    results.append({'name':variant+'-producer-log-report','passed':True})
   else:rejected(variant+'-receipt-refused',lambda:h.validate_reports(*frames,prepared))
(a.evidence/'host-result.json').write_text(json.dumps({'passed':True,'physical_access':False,'native_elf_sha256':sha(image.read_bytes()),'host_sha256':sha(HOST.read_bytes()),'checker_sha256':sha(Path(__file__).read_bytes()),'stopped_capture_sha256':sha((r/'apps/npu-finite/stopped-capture.py').read_bytes()),'execution_profile':execution,'native_initializer':{'raw':'native-initial-report.bin','sha256':sha(initialized),'magic':hex(native_report[0]),'boundary':'Actual linked monitor report initialization; contract-validator success and counter modeled, stopped before session seal. Completion fields are modeled; final receipt/status writer covered separately.'},'cases':results,'scope':'New host packaging/admission/report validation/sealed preload/DRAM restoration/retention with mocked FEL and ROM enumeration. Temporary modeled evidence uses a nonexistent test port and is removed. Existing launcher bytes reused; no full native admission, hardware or queues.'},indent=2)+'\n');print('PASS new NPU returned-RAM host cases:',len(results))
