#!/usr/bin/env python3
"""Produce actual-identity SPI software-only records through production validators."""
import hashlib,importlib.util,json,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def produce(image,out,interrupt=False,dma=False):
 out.mkdir(parents=True,exist_ok=False);prep=module('spi_fixture_prepare',ROOT/'apps/spi/prepare.py');ih=hashlib.sha256(image.read_bytes()).hexdigest();parts,entry,identity=prep.base.image_parts(image)
 baseline=Path.home()/'.cache/sunstoneos/evidence/board-supervised-20261017/batch1/session-fixture.bin'
 raw=bytearray(baseline.read_bytes());assert len(raw)==592;raw[48:80]=identity;raw.extend(bytes(240));struct.pack_into('<6Q',raw,592,0x4153373353504131,2 if interrupt else 1,832,23,91,5000000)
 # Same enabled fabric profile as accepted shared-provider fixtures.
 struct.pack_into('<6I',raw,488,0,0x83000002,0x84000001,24000000,200000000,200000000)
 raw[768:780]=bytes([15,0,0]*4);raw[780:792]=bytes([5,0,0]*4)
 struct.pack_into('<2I',raw,792,2,8);raw[800:802]=bytes([0x9f,0])
 target=dict(identity='spi-software-only-model',port='no-physical-port',image_sha256=ih)
 def record(name,value):
  data=json.dumps(value,sort_keys=True).encode();p=(out/name).resolve();p.write_bytes(data);return dict(path=str(p),sha256=hashlib.sha256(data).hexdigest())
 evidence=record('software-only.json',dict(**target,physical_access=False,purpose='Software fixture only; not an actual endpoint or physical admission'))
 for offset in (80,216,248,280):raw[offset:offset+32]=bytes.fromhex(evidence['sha256'])
 refs={}
 for i,(key,required) in enumerate(prep.FACTS.items()):
  value=dict(**target,schema='a733-spi-'+key+'-v1',reviewed=True,**{k:True for k in required})
  if key=='initial':value.update(request_hex='9f00',response_bytes=4096 if dma else 8,peer_description='Software-only modeled peer; opcode has no physical meaning',protocol_description='Modeled exchange only',effects_description='No physical effects')
  if key=='electrical':value.update(circuit_description='Software-only no circuit',signal_compatibility_description='Software-only no signals')
  refs[key]=record(key+'.json',value);raw[640+32*i:672+32*i]=bytes.fromhex(refs[key]['sha256'])
 if dma:
  config=struct.pack('<12I4QIHHQ',0,0x83000002,0x84000001,24000000,200000000,200000000,0,0,3,0,3,1,31,101,0x04601000,101,0x5f,0xffff,3,5000000)
  required=('cpu_dma0_users_stopped','remote_mcu_users_stopped','other_fifo_producers_stopped','prior_bus_writes_drained',
   'irq143_owned_masked_inactive','manual_power_removal_retains_all_dma_resources','four_private_dma_pages_exclusive')
  refs['dma']=record('dma.json',dict(**target,schema='a733-spi-dma-authority-v1',reviewed=True,configuration_hex=config.hex(),**{k:True for k in required}))
  producer=module('spi_fixture_dma_producer',ROOT/'apps/spi/dma-session.py')
  raw=bytearray(producer.produce(raw,bytes(raw[800:802]),(out/'dma.json').read_bytes()))
 template=out/'session-template.bin';template.write_bytes(raw);prep.session_check(raw)
 conditions={'source-backed-cpu-dsu-transition','cold-cache-and-dirty-alias-exclusion','dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded','usb-clock-power-reset-ownership','usb-device-mapping','physical-counter-continuity'}
 platform=record('platform.json',dict(**target,schema='a733-smp-observer-platform-v1',entry_conditions_established=True,physical_coherence_result='under-test',conditions={k:evidence for k in sorted(conditions)}))
 ddr=record('ddr.json',dict(**target,schema='a733-ddr-reservation-v1',exclusive=True,published_cache_disabled=True,remote_writers_excluded=True,ranges=[[0x41000000,0x42200000]],evidence=[evidence]))
 admission=dict(**target,schema='a733-spi-supervised-entry-v1',reservation=100,session_template_sha256=hashlib.sha256(raw).hexdigest(),ddr=ddr,platform=platform,**refs,whole_board_power_removal=True,no_resource_reuse_before_power_removal=True,recovery=dict(mode='manual-usb-reconnect',demonstrated_fel=True,explicit_reconnect_before_resumption=True,preserve_microsd=True,one_attempt=True,evidence=[evidence]))
 if dma:
  # Synthetic validator input only: no real port, peer, or physical authority.
  # The explicit fixture marker accompanies the nominal booleans being tested.
  ids=struct.unpack_from('<3Q',raw,24)
  security=dict(**target,schema='a733-dma-nonsecure-access-v1',physical_authority=True,modeled=False,reviewed=True,
   software_fixture_only=True,epoch=ids[0],generation=ids[1],lease=ids[2],controller=0x04601000,service_channels=3,
   dma_evidence=101,dma_handoff=101,nonsecure_channels=3,configuration_sha256=hashlib.sha256(raw[832:928]).hexdigest(),
   session_template_sha256=hashlib.sha256(raw).hexdigest(),evidence=[evidence],**{k:True for k in prep.security_authority.FACTS})
  admission['dma_nonsecure_access']=record('MODEL-ONLY-security.json',security)
 path=out/'admission.json';path.write_text(json.dumps(admission,indent=2)+'\n');prep.validate_admission(admission,ih,hashlib.sha256(raw).hexdigest(),raw)
 (out/'fixture-inputs.json').write_text(json.dumps(dict(baseline=str(baseline),baseline_sha256=hashlib.sha256(baseline.read_bytes()).hexdigest(),image_sha256=ih,build_id=identity.hex(),physical_access=False),indent=2)+'\n')
 return template,path
if __name__=='__main__':
 import argparse
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--dma',action='store_true');a=p.parse_args();produce(a.image,a.output,dma=a.dma)
