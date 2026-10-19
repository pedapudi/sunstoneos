#!/usr/bin/env python3
"""Mock only the new NPU host profile; never discover hardware or touch queues."""
import argparse,hashlib,importlib.util,json,os,shutil,struct,tempfile
from pathlib import Path
from unittest.mock import patch
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--client-logs',type=Path,required=True);p.add_argument('--native',type=Path,required=True);p.add_argument('--evidence',type=Path,required=True);a=p.parse_args();a.evidence.mkdir(parents=True,exist_ok=False)
r=Path(__file__).resolve().parents[2];spec=importlib.util.spec_from_file_location('host',r/'apps/npu-interrupt/host.py');h=importlib.util.module_from_spec(spec);spec.loader.exec_module(h);h=h.finite
def sha(data):return hashlib.sha256(data).hexdigest()
def ref(path):return {'path':str(path.resolve()),'sha256':sha(path.read_bytes())}
results=[]
with tempfile.TemporaryDirectory(prefix='npu-host-fixture-') as temporary:
 t=Path(temporary);native=a.native.resolve();image=native/'image.elf';manifest=native/'manifest.json';data=image.read_bytes();parts,address,build,layout,g=h.image_profile(data,json.loads(manifest.read_bytes()))
 raw=bytearray((Path.home()/'.cache/sunstoneos/evidence/npu-single-core-20261018/startup-usb-resume/session-fixture.bin').read_bytes());raw[48:80]=build;struct.pack_into('<Q',raw,8,2);profile=h.original.session_profile(raw,layout);boot,boot_bytes,pll,pll_bytes=layout[1:5]
 fact=t/'modeled-fact.txt';fact.write_text('Software-only mocked host evidence. No physical observation or live target.\n')
 # An impossible test port prevents these temporary records matching a live board.
 identity=[0,0,0,0,1];port='mock-npu-port';common={'image_sha256':sha(data),'identity':identity,'port':port,'whole_platform':profile['whole_platform']}
 def write(name,value):path=t/name;path.write_text(json.dumps(value,indent=2)+'\n');return path
 required={'clock':{'independent-dram-phy-sources','exclusive-pll-npu-consumers-and-writers','no-autonomous-parent-selector-writers','enabled-retune-snapshot'},'power':{'dcdc2-stable-800mv','authenticated-pmic-pin-inheritance'},'supervision':{'manual-whole-board-power-removal','stop-after-first-failure','successful-close-stop-quiescence-ROM-retirement','failure-retain-until-whole-power-removal','DRAM-restore-only-after-validated-normal-return','no-old-SRAM-restore'}}
 if struct.unpack_from('<I',raw,pll+24)[0]==3:required['clock'].remove('enabled-retune-snapshot');required['clock'].update({'live-disabled-pll-quiescent-on-snapshot','native-dram-continuations-and-shared-sram-abandonment','supervised-cross-pll-disturbance-risk'})
 boot_hash,pll_hash=sha(raw[boot:boot+boot_bytes]),sha(raw[pll:pll+pll_bytes]);facts={}
 for name,offset in [('clock',112),('power',144),('supervision',176)]:
  value={**common,'conditions':{k:ref(fact) for k in required[name]},'pll_sha256':pll_hash,'boot_sha256':boot_hash,'supply_uv':800000,'supervisor':profile['supervisor'],'one_attempt':True};facts[name]=write(name+'.json',value);raw[offset:offset+32]=hashlib.sha256(facts[name].read_bytes()).digest()
 ddr=write('ddr.json',{**common,'schema':'a733-ddr-reservation-v1','ranges':[[0x41000000,0x42200000]],'exclusive':True,'published_cache_disabled':True,'remote_writers_excluded':True,'evidence':[ref(fact)]})
 conditions={'cold-cache-and-dirty-alias-exclusion','dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded','usb-clock-power-reset-ownership','usb-device-mapping','physical-counter-continuity','npu-startup-sram-confinement-drain-ownership','pmic-bus-rail-pin-ownership','primary-cache-transition-and-secondary-off','rom-usb0-pio-takeover','usb0-irq188-189-190-disabled-inactive-clear','usb-supplies-stable-and-writers-excluded'}
 platform=write('platform.json',{**common,**profile,'schema':'a733-npu-convolution-platform-v1','conditions':{k:ref(fact) for k in conditions},'entry_conditions_established':True,'physical_coherence_result':'under-test'})
 launcher=Path.home()/'.cache/sunstoneos/evidence/npu-single-core-20261018/package-final/bundle/launch.bin'
 review=write('root.json',{**common,'schema':'a733-npu-finite-return-review-v1','reviewed':True,'execution_profile':h.PROFILE,'build_id':build.hex(),'supervisor':profile['supervisor'],'launcher_sha256':sha(launcher.read_bytes()),'host_sources':{n:sha((r/n).read_bytes()) for n in h.HOST_SOURCES},'conditions':{k:ref(fact) for k in ('architecture-reset-reviewed','software-handoff-reviewed','normal-return-retirement-reviewed','failure-retention-reviewed')}})
 entry={**common,**profile,'schema':h.SCHEMA,'execution_profile':h.PROFILE,'boot_sha256':boot_hash,'pll_sha256':pll_hash,'ddr':ref(ddr),'platform':ref(platform),'root_review':ref(review),**{k:ref(v) for k,v in facts.items()}}
 irq=write('interrupt.json',{**common,'schema':'a733-npu-interrupt-entry-v1','build_id':build.hex(),'interrupt':97,'cpu':0,'conditions':{k:ref(fact) for k in ('exclusive-level-high-SPI65','single-task-event-acknowledgment','masked-notification-and-serialized-rearm','close-source-disable-before-pending-clear','inactive-pending-clear-before-return','saved-route-restored-before-enable')}});entry['interrupt']=ref(irq)
 admission=write('admission.json',entry);raw[80:112]=hashlib.sha256(admission.read_bytes()).digest();session=t/'session.bin';session.write_bytes(raw)
 # Launcher compilation is unchanged and uses preserved accepted bytes here.
 def launcher_copy(output,*args):shutil.copy2(launcher,output/'launch.bin')
 with patch.object(h.original,'build_launcher',launcher_copy):h.prepare(image,manifest,session,admission,t/'bundle','mock-compiler','mock-linker','mock-objcopy')
 bundle=t/'bundle';prepared=h.validate_bundle(bundle);results.append({'name':'original-producers-valid-returning-bundle','passed':True})
 def rejected(name,fn):
  try:fn()
  except (ValueError,KeyError):results.append({'name':name,'passed':True});return
  raise AssertionError(name+' accepted')
 missing={k:v for k,v in entry.items() if k!='interrupt'}
 for name,value in [('missing-IRQ-admission',missing),('polled-entry-refused',{**entry,'execution_profile':'npu-finite-return-v1'})]:
  encoded=json.dumps(value).encode();mutated=bytearray(raw);mutated[80:112]=hashlib.sha256(encoded).digest()
  rejected(name,lambda:h.validate_admission(encoded,mutated,build,layout,sha(data)))
 old_irq=irq.read_bytes();bad=json.loads(old_irq);bad['interrupt']=98;irq.write_text(json.dumps(bad));bad_entry={**entry,'interrupt':ref(irq)}
 encoded=json.dumps(bad_entry).encode();mutated=bytearray(raw);mutated[80:112]=hashlib.sha256(encoded).digest()
 rejected('other-route-refused',lambda:h.validate_admission(encoded,mutated,build,layout,sha(data)));irq.write_bytes(old_irq)
 for variant in ('matched','mismatch','missing-completion','missing-stop','wrong-completion','duplicate-completion'):
  text=(a.client_logs/('mismatch.log' if variant=='mismatch' else 'matched.log')).read_text();status=int(variant=='mismatch')
  if variant=='missing-completion':text=text.replace('NPU_IRQ_COMPLETIONS=0x0000000000000003\n','')
  if variant=='missing-stop':text=text.replace('NPU_IRQ_STOPPED=0x0000000000000001\n','')
  if variant=='wrong-completion':text=text.replace('NPU_IRQ_COMPLETIONS=0x0000000000000003','NPU_IRQ_COMPLETIONS=0x0000000000000002')
  if variant=='duplicate-completion':text='NPU_IRQ_COMPLETIONS=0x0000000000000002\n'+text
  monitor=[0x4153373343525331,profile['epoch'],profile['generation'],5,status,0,11000000,1,12000000]+[0]*7
  header=[0x534b4552,5,status,len(text),0,0,0,0,11500000,12000000]+[0]*6
  receipt=[0x4e50555245543031,profile['epoch'],profile['generation'],profile['lease'],status,2 if status else 1,g['clean_start'],g['clean_end'],(g['clean_end']-g['clean_start'])//64,490000000,len(text)]
  platform=bytearray(16384);struct.pack_into('<16Q',platform,0,*header);platform[256:256+len(text)]=text.encode()
  frames=struct.pack('<16Q',*monitor),bytes(platform),struct.pack('<11Q',*receipt)
  for name,value in zip(('monitor','platform','receipt'),frames):(a.evidence/(variant+'-'+name+'.bin')).write_bytes(value)
  if variant in ('matched','mismatch'):
   value=h.validate_reports(*frames,prepared);assert value['functional_success']==(variant=='matched') and value['interrupt_completions']==3 and value['interrupt_stopped'];results.append({'name':variant+'-producer-log-report','passed':True})
  else:rejected(variant+'-receipt-refused',lambda:h.validate_reports(*frames,prepared))
 shutil.copytree(bundle,a.evidence/'bundle')
 (a.evidence/'session-provenance.json').write_text(json.dumps({'source':str(Path.home()/'.cache/sunstoneos/evidence/npu-single-core-20261018/startup-usb-resume/session-fixture.bin'),'current_native_elf_sha256':sha(data),'current_build_id':build.hex(),'fixture_admission':'Software-only impossible port; not physical authority','producer_logs':str(a.client_logs)},indent=2)+'\n')
(a.evidence/'host-result.json').write_text(json.dumps({'passed':True,'physical_access':False,'cases':results,'scope':'New interrupt profile/admission and native-client-produced IRQ receipt logs. Returned monitor/receipt envelopes and admission facts modeled. Existing polled transport/restore evidence reused, no transport/board/queue access.'},indent=2)+'\n')
print('PASS private IRQ profile, producer-derived session packaging and native-client receipt host cases')
