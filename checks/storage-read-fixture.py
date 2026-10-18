#!/usr/bin/env python3
"""Produce explicitly modeled authority with the actual SD0 image identity."""
import hashlib,importlib.util,json,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
FACTS={
 'initial':['cold_sd0_controller','cold_card_protocol','all_sd0_clients_excluded','remote_firmware_excluded','irq193_owned_disabled_drained','no_dma_queue_encryption','no_clock_reset_writers'],
 'memory':['private_pio_buffer','identity_cacheable_inner_shareable','no_aliases','permanent_reservation'],
 'supplies':['dcdc1_dc1sw2_3300000_cldo5_1800000','source_qualified_settling','pf_domain_3300','no_voltage_writers'],
 'topology':['sd0_pf0_6_route','inserted_removable_sd','pf6_active_low_detect','source_qualified_pulls_drives','no_alternate_pad_writers','complete_pin_authority'],
 'supervision':['whole_board_power_removal','permanent_resource_retention','no_buffer_reuse','one_attempt','no_watchdog_arm','no_reset_retry']}
def produce(image,out):
 out.mkdir(parents=True,exist_ok=False);prep=module('sd0_fixture_prepare',ROOT/'apps/storage-read/prepare.py');ih=hashlib.sha256(image.read_bytes()).hexdigest();parts,entry,identity=prep.base.image_parts(image)
 baseline=Path.home()/'.cache/sunstoneos/evidence/board-supervised-20261017/batch1/session-fixture.bin'
 raw=bytearray(baseline.read_bytes());assert len(raw)==592;raw[48:80]=identity;raw.extend(bytes(272));struct.pack_into('<3I',raw,488,0,1<<31,1<<31);struct.pack_into('<5Q',raw,592,0x4153373352444131,1,864,23,91)
 for i in range(7):raw[813+3*i:816+3*i]=bytes((2 if 1<=i<=3 else 0,1,0))
 struct.pack_into('<QQI',raw,840,10000000,5000000,0x00300000)
 target=dict(identity='storage-read-software-only-model',port='no-physical-port',image_sha256=ih)
 def record(name,value):
  data=json.dumps(value,sort_keys=True).encode();p=(out/name).resolve();p.write_bytes(data);return dict(path=str(p),sha256=hashlib.sha256(data).hexdigest())
 evidence=record('MODEL-NOT-PHYSICAL.json',dict(**target,physical_access=False,purpose='Software fixture only; never physical admission'))
 for offset in (80,216,248,280):raw[offset:offset+32]=bytes.fromhex(evidence['sha256'])
 refs={}
 for i,(key,required) in enumerate(FACTS.items()):
  refs[key]=record(key+'.json',dict(**target,schema='a733-storage-read-'+key+'-v1',reviewed=True,physical_access=False,**{k:True for k in required}));raw[632+32*i:664+32*i]=bytes.fromhex(refs[key]['sha256'])
 template=out/'session-template.bin';template.write_bytes(raw);prep.session_check(raw);prep.embedded_reference(image.read_bytes(),raw)
 conditions={'source-backed-cpu-dsu-transition','cold-cache-and-dirty-alias-exclusion','dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded','usb-clock-power-reset-ownership','usb-device-mapping','physical-counter-continuity'}
 platform=record('platform.json',dict(**target,schema='a733-smp-observer-platform-v1',entry_conditions_established=True,physical_coherence_result='under-test',conditions={k:evidence for k in sorted(conditions)}))
 ddr=record('ddr.json',dict(**target,schema='a733-ddr-reservation-v1',exclusive=True,published_cache_disabled=True,remote_writers_excluded=True,ranges=[[0x41000000,0x42200000]],evidence=[evidence]))
 admission=dict(**target,schema='a733-storage-read-supervised-entry-v1',reservation=100,session_template_sha256=hashlib.sha256(raw).hexdigest(),ddr=ddr,platform=platform,**refs,whole_board_power_removal=True,no_resource_reuse_before_power_removal=True,recovery=dict(mode='manual-usb-reconnect',demonstrated_fel=True,explicit_reconnect_before_resumption=True,preserve_microsd=True,one_attempt=True,evidence=[evidence]))
 path=out/'admission.json';path.write_text(json.dumps(admission,indent=2)+'\n');prep.validate_admission(admission,ih,hashlib.sha256(raw).hexdigest(),raw)
 (out/'fixture-inputs.json').write_text(json.dumps(dict(baseline=str(baseline),baseline_sha256=hashlib.sha256(baseline.read_bytes()).hexdigest(),image_sha256=ih,build_id=identity.hex(),physical_access=False),indent=2)+'\n')
 return template,path
if __name__=='__main__':
 import argparse
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();produce(a.image,a.output)
