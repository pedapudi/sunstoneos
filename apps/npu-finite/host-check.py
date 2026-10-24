#!/usr/bin/env python3
"""Mock only the new NPU host profile; never discover hardware or touch queues."""
import argparse,hashlib,importlib.util,json,os,shutil,struct,tempfile
from pathlib import Path
from unittest.mock import patch
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--case',action='append',default=[],help='Run only named transfer cases; packaging validation still runs.');p.add_argument('--native',type=Path,required=True);p.add_argument('--evidence',type=Path,required=True);a=p.parse_args();a.evidence.mkdir(parents=True,exist_ok=False)
r=Path(__file__).resolve().parents[2];spec=importlib.util.spec_from_file_location('host',r/'apps/npu-finite/host.py');h=importlib.util.module_from_spec(spec);spec.loader.exec_module(h)
def sha(data):return hashlib.sha256(data).hexdigest()
def ref(path):return {'path':str(path.resolve()),'sha256':sha(path.read_bytes())}
results=[]
with tempfile.TemporaryDirectory(prefix='npu-host-fixture-') as temporary:
 t=Path(temporary);native=a.native.resolve();image=native/'image.elf';manifest=native/'manifest.json';data=image.read_bytes();parts,address,build,layout,g=h.image_profile(data,json.loads(manifest.read_bytes()));checkpoint=bool(g.get('checkpoint'));execution=h.CHECKPOINT if checkpoint else h.PROFILE
 # The reviewed 608-byte page, converted to the image's reduced record.
 spec=importlib.util.spec_from_file_location('finite_images',r/'checks/npu-image-checks.py');images=importlib.util.module_from_spec(spec);spec.loader.exec_module(images)
 raw=bytearray(images.npu_page((Path.home()/'.cache/sunstoneos/evidence/npu-single-core-20261018/startup-usb-resume/session-fixture.bin').read_bytes(),layout,h.original.SINGLE_CORE_VERSION));raw[48:80]=build;profile=h.original.session_profile(raw,layout);boot,boot_bytes=layout[1:3]
 # Read the report identity from the actual linked monitor initializer.
 # Only contract admission and architectural time are modeled. Stop before seal.
 from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
 from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR
 spec=importlib.util.spec_from_file_location('finite_report_layout',r/'checks/harness.py')
 harness=importlib.util.module_from_spec(spec);spec.loader.exec_module(harness)
 native_image=harness.Image(image,layout_names=('a733_smp_contract',));u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
 u.mem_map(0x02000000,0x07000000);u.mem_map(0x40000000,0x04000000)
 for at,value in native_image.parts:
  if value:u.mem_write(at,value)
 contract=native_image.layouts['a733_smp_contract']
 for key in ('epoch','generation','lease'):
  u.mem_write(0x42004000+contract[key],struct.pack('<Q',profile[key]))
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
 if checkpoint:
  required['clock']={'npu-clock-power-registers-not-accessed'}
  required['power']={'providers-never-constructed','unconsumed-typed-boot-no-provider-grant'}
  required['supervision'].remove('successful-close-stop-quiescence-ROM-retirement');required['supervision'].add('checkpoint-normal-return-retirement')
 boot_hash=sha(raw[boot:boot+boot_bytes]);facts={}
 for name,offset in [('clock',112),('power',144),('supervision',176)]:
  value={**common,'conditions':{k:ref(fact) for k in required[name]},'boot_sha256':boot_hash,'supply_uv':800000,'supervisor':profile['supervisor'],'one_attempt':True};facts[name]=write(name+'.json',value);raw[offset:offset+32]=hashlib.sha256(facts[name].read_bytes()).digest()
 ddr=write('ddr.json',{**common,'schema':'a733-ddr-reservation-v1','ranges':[[0x41000000,0x42200000]],'exclusive':True,'published_cache_disabled':True,'remote_writers_excluded':True,'evidence':[ref(fact)]})
 conditions={'cold-cache-and-dirty-alias-exclusion','dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded','usb-clock-power-reset-ownership','usb-device-mapping','physical-counter-continuity','npu-startup-sram-confinement-drain-ownership','pmic-bus-rail-pin-ownership','primary-cache-transition-and-secondary-off','rom-usb0-pio-takeover','usb0-irq188-189-190-disabled-inactive-clear','usb-supplies-stable-and-writers-excluded'}
 if checkpoint:
  conditions-= {'npu-startup-sram-confinement-drain-ownership','pmic-bus-rail-pin-ownership','usb-device-mapping','rom-usb0-pio-takeover','usb0-irq188-189-190-disabled-inactive-clear'}
  conditions|= {'providers-never-constructed','npu-clock-power-registers-not-accessed'}
 platform=write('platform.json',{**common,**profile,'schema':'a733-npu-checkpoint-platform-v1' if checkpoint else 'a733-npu-convolution-platform-v1','conditions':{k:ref(fact) for k in conditions},'entry_conditions_established':True,'physical_coherence_result':'under-test'})
 launcher=Path.home()/'.cache/sunstoneos/evidence/npu-single-core-20261018/package-final/bundle/launch.bin'
 review=write('root.json',{**common,'schema':'a733-npu-finite-return-review-v1','reviewed':True,'execution_profile':execution,'build_id':build.hex(),'supervisor':profile['supervisor'],'launcher_sha256':sha(launcher.read_bytes()),'host_sources':{n:sha((r/n).read_bytes()) for n in h.HOST_SOURCES},'conditions':{k:ref(fact) for k in ('architecture-reset-reviewed','software-handoff-reviewed','normal-return-retirement-reviewed','failure-retention-reviewed')}})
 entry={**common,**profile,'schema':h.SCHEMA,'execution_profile':execution,'boot_sha256':boot_hash,'ddr':ref(ddr),'platform':ref(platform),'root_review':ref(review),**{k:ref(v) for k,v in facts.items()}}
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
 if checkpoint:
  rejected('checkpoint-cannot-use-numerical-profile',lambda:h.image_profile(data,{**json.loads(manifest.read_bytes()),'execution_profile':h.PROFILE}))
  saved_platform=platform.read_bytes()
  for key in ('remote-writers-excluded','providers-never-constructed','npu-clock-power-registers-not-accessed'):
   changed=json.loads(saved_platform);del changed['conditions'][key];platform.write_text(json.dumps(changed))
   encoded=json.dumps({**entry,'platform':ref(platform)}).encode();mutated=bytearray(raw);mutated[80:112]=hashlib.sha256(encoded).digest()
   rejected('checkpoint-missing-'+key,lambda:h.validate_admission(encoded,mutated,build,layout,sha(data),execution_profile=execution))
  platform.write_bytes(saved_platform)
 saved=(bundle/'request.bin').read_bytes();(bundle/'request.bin').write_bytes(bytes(len(saved)));rejected('changed-packet-refused',lambda:h.validate_bundle(bundle));(bundle/'request.bin').write_bytes(saved)
 def reports(variant):
  status=int(variant=='mismatch');reference=bytes.fromhex(g['reference']);first=hashlib.sha256(reference).digest();second=reference
  if status:first=bytes([first[0]^1])+first[1:]
  if variant=='false-success':second=bytes([second[0]^1])+second[1:]
  text='NPU_STAGE=WAIT_STARTUP\nNPU_STAGE=QUIESCENT\nNPU_FIRST_DIGEST=\n'+''.join(f'0x{x:016x}\n' for x in first)+'NPU_SECOND_OUTPUT=\n'+''.join(f'0x{x:016x}\n' for x in second)+(('FAIL NPU numerical comparison' if status else 'PASS NPU two matched jobs, copied output, REAP, CLOSE, local stop and parked providers')+'\n')
  if checkpoint:
   text='NPU_CHECKPOINT=ENTRY_MEMORY_READY; providers never constructed; jobs=0\n'
   if variant=='false-success':text=text.replace('jobs=0','jobs=1')
  if variant=='duplicate-log':text+=text
  monitor=list(native_report);monitor[3:9]=[5,status,0x4e50554e if checkpoint else 0,11000000,1,12000000]
  if variant=='old-generic-monitor-magic':monitor[0]=0x4153373343525331
  header=[0x534b4552,5,status,len(text),0,0,0,0,11500000,12000000]+[0]*6
  receipt=[0x4e505543484b3031 if checkpoint else 0x4e50555245543031,profile['epoch'],profile['generation'],profile['lease'],status,0x4e50554e if checkpoint else 2 if status else 1,g['clean_start'],g['clean_end'],(g['clean_end']-g['clean_start'])//64,490000000,len(text)]
  if variant=='ordinary-receipt':receipt[0]=0x4e50555245543031;receipt[5]=1;monitor[5]=0
  if variant=='stale-generation':monitor[2]+=1
  if variant=='stale-lease':receipt[3]+=1
  if variant=='unclean-receipt':receipt[8]-=1
  if variant=='pending-monitor':monitor[3]=4
  if variant=='wrong-status':header[2]=1
  if variant=='invalid-time':monitor[8]=490000000
  if variant=='oversized-log':header[3]=20000
  plat=bytearray(16384);struct.pack_into('<16Q',plat,0,*header);plat[256:256+len(text)]=text.encode()
  return struct.pack('<16Q',*monitor),bytes(plat),struct.pack('<11Q',*receipt)
 # Real preload/sealed-source/readback logic; only FEL operations and new-ROM event modeled.
 variants=['nominal','old-generic-monitor-magic','mismatch','stale-generation','stale-lease','unclean-receipt','pending-monitor','wrong-status','invalid-time','oversized-log','false-success','duplicate-log','no-new-ROM','capture-transport-failure','restore-readback-failure','changed-backup','wrong-ROM-entry']
 if checkpoint:variants.append('ordinary-receipt')
 assert not set(a.case)-set(variants),'Unknown selected case'
 for variant in variants:
  if a.case and variant not in a.case:continue
  out=t/variant;out.mkdir();state={'calls':[],'failed':False,'new_ROM':False,'restore_writes':0,'writes':[]};memory=bytearray(h.DRAM[1]);sram=bytearray(0x100000);record={'launch_attempted':False,'dirty':False};p2=h.validate_bundle(bundle);frame=reports(variant)
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
     if variant=='changed-backup':(out/'before-DRAM.bin').write_bytes(b'x'*h.DRAM[1])
     continue
    address=int(args[i],0);i+=1
    if op=='readl':
     assert address!=0x03000024,'Normal boot GPIO is not a forced-FEL precondition'
     rom={0x20:0xeb00008f,0x24:0xe3a00911,0x28:0xe3a0193d,0x3c:0xe3a0605c,0x40:0xea00004d}
     if address in rom:assert all((out/('before-'+name+'.bin')).exists() for name,_,_ in h.base.REGIONS)
     words.append(hex(rom.get(address,0) ^ (1 if variant=='wrong-ROM-entry' and address==0x3c else 0)));continue
    if op=='write':
     source=Path(args[i]);i+=1;data=source.read_bytes()
     state['writes'].append((address,len(data),state['new_ROM']))
     if len(data)==h.DRAM[1]:state['restore_writes']+=1
     if 0<=address<address+len(data)<=len(sram):sram[address:address+len(data)]=data
     if h.DRAM[0]<=address<address+len(data)<=h.DRAM[0]+h.DRAM[1]:memory[address-h.DRAM[0]:address-h.DRAM[0]+len(data)]=data
     continue
    assert op=='read';size=int(args[i]);path=Path(args[i+1]);i+=2
    if variant=='capture-transport-failure' and state['new_ROM'] and address==g['receipt']:state['failed']=True;record['transport_failed']=True;raise ValueError('Modeled transport loss')
    value=readmem(address,size)
    if state['new_ROM'] and size==h.DRAM[1] and variant=='restore-readback-failure':value=bytes([1])+value[1:]
    path.write_bytes(value)
   return '\n'.join(words)
  def wait_return():
   if variant=='no-new-ROM':raise ValueError('Modeled return deadline')
   state['new_ROM']=True
  failure=None
  try:result=h.run_once(transaction,wait_return,lambda:None,bundle,p2,out,record,lambda:None)
  except ValueError as error:failure=error
  normal=variant=='nominal' or variant=='mismatch' and not checkpoint;assert (failure is None)==normal,(variant,failure)
  if normal:
   assert record['complete'] and record['dram_restored'] and record['ownership_release_permitted'] and not record['sram_restored']
   assert result['functional_success']==(variant=='nominal' and not checkpoint) and result['controlled_negative']==(variant=='mismatch')
   assert state['restore_writes']==1
  else:
   assert not record.get('complete') and not record.get('dram_restored')
   if variant=='wrong-ROM-entry':assert 'exe' not in state['calls'] and 'Forced-FEL ROM entry' in str(failure)
   assert state['restore_writes']==int(variant=='restore-readback-failure'),variant
   held=[];h.retain_failure(record,lambda:None,lambda:held.append('quarantine'),failure,hold=lambda:held.append('hold'));assert held==['quarantine','hold'],(variant,held)
  assert all(address>=h.DRAM[0] for address,size,after_ROM in state['writes'] if after_ROM)
  results.append({'name':variant,'passed':True,'restore_writes':state['restore_writes'],'retained':not normal,'functional_success':record.get('npu_result',{}).get('functional_success')})
(a.evidence/'host-result.json').write_text(json.dumps({'passed':True,'physical_access':False,'native_elf_sha256':sha(image.read_bytes()),'host_sha256':sha((r/'apps/npu-finite/host.py').read_bytes()),'checker_sha256':sha(Path(__file__).read_bytes()),'execution_profile':execution,'native_initializer':{'raw':'native-initial-report.bin','sha256':sha(initialized),'magic':hex(native_report[0]),'boundary':'Actual linked monitor report initialization; contract-validator success and counter modeled, stopped before session seal. Completion fields are modeled; final receipt/status writer covered separately.'},'cases':results,'scope':'New host packaging/admission/report validation/sealed preload/DRAM restoration/retention with mocked FEL and ROM enumeration. Temporary modeled evidence uses a nonexistent test port and is removed. Existing launcher bytes reused; no full native admission, hardware or queues.'},indent=2)+'\n');print('PASS new NPU returned-RAM host cases:',len(results))
