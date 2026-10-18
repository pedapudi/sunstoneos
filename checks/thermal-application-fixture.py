#!/usr/bin/env python3
"""Produce actual-identity software-only thermal entry records through native ABI."""
import hashlib,importlib.util,json,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def produce(image,out):
 out.mkdir(parents=True,exist_ok=False);prep=module('thermal_fixture_prepare',ROOT/'apps/thermal/prepare.py');ih=hashlib.sha256(image.read_bytes()).hexdigest();parts,entry,identity=prep.base.image_parts(image)
 baseline=Path.home()/'.cache/sunstoneos/evidence/board-supervised-20261017/batch1/session-fixture.bin'
 raw=bytearray(baseline.read_bytes());assert len(raw)==592;raw[48:80]=identity;raw.extend(bytes(216));struct.pack_into('<5Q',raw,592,0x4153373354484131,1,808,23,91)
 struct.pack_into('<2I4B',raw,760,1,12000000,1,31,31,0)
 for i in range(5):struct.pack_into('<3H',raw,772+6*i,1400,1600,1000)
 target=dict(identity='thermal-software-only-model',port='no-physical-port',image_sha256=ih)
 def record(name,value):
  data=json.dumps(value,sort_keys=True).encode();p=(out/name).resolve();p.write_bytes(data);return dict(path=str(p),sha256=hashlib.sha256(data).hexdigest())
 evidence=record('software-only.json',dict(**target,physical_access=False,purpose='Software fixture only; never physical admission'))
 for offset in (80,216,248,280):raw[offset:offset+32]=bytes.fromhex(evidence['sha256'])
 facts={'initial':['exclusive_ths_and_irq71','remote_writers_excluded','no_inherited_thermal_service','security_access_authorized','gic_route_owned','boot_lineage_reviewed'],
 'electrical':['avcc_adc_digital_supplies_stable','sid_shadows_readable','device_mappings_verified','no_physical_aliases'],
 'clocks':['stable_apb1_ref_osc','no_clock_reset_writers','gpadc_consumers_excluded'],
 'supervision':['whole_board_power_removal','permanent_retention','no_reuse','one_attempt']}
 refs={}
 for i,(key,required) in enumerate(facts.items()):
  refs[key]=record(key+'.json',dict(**target,schema='a733-thermal-'+key+'-v1',reviewed=True,**{k:True for k in required}));raw[632+32*i:664+32*i]=bytes.fromhex(refs[key]['sha256'])
 template=out/'session-template.bin';template.write_bytes(raw);prep.session_check(raw)
 conditions={'source-backed-cpu-dsu-transition','cold-cache-and-dirty-alias-exclusion','dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded','usb-clock-power-reset-ownership','usb-device-mapping','physical-counter-continuity'}
 platform=record('platform.json',dict(**target,schema='a733-smp-observer-platform-v1',entry_conditions_established=True,physical_coherence_result='under-test',conditions={k:evidence for k in sorted(conditions)}))
 ddr=record('ddr.json',dict(**target,schema='a733-ddr-reservation-v1',exclusive=True,published_cache_disabled=True,remote_writers_excluded=True,ranges=[[0x41000000,0x42200000]],evidence=[evidence]))
 admission=dict(**target,schema='a733-thermal-supervised-entry-v1',reservation=100,session_template_sha256=hashlib.sha256(raw).hexdigest(),ddr=ddr,platform=platform,**refs,whole_board_power_removal=True,no_resource_reuse_before_power_removal=True,recovery=dict(mode='manual-usb-reconnect',demonstrated_fel=True,explicit_reconnect_before_resumption=True,preserve_microsd=True,one_attempt=True,evidence=[evidence]))
 path=out/'admission.json';path.write_text(json.dumps(admission,indent=2)+'\n');prep.validate_admission(admission,ih,hashlib.sha256(raw).hexdigest(),raw)
 (out/'fixture-inputs.json').write_text(json.dumps(dict(baseline=str(baseline),baseline_sha256=hashlib.sha256(baseline.read_bytes()).hexdigest(),image_sha256=ih,build_id=identity.hex()),indent=2)+'\n')
 return template,path
if __name__=='__main__':
 import argparse
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();produce(a.image,a.output)
