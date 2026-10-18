#!/usr/bin/env python3
"""Produce explicitly modeled authority with the actual PWM image identity."""
import hashlib,importlib.util,json,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
FACTS={
 'initial':['cold_pwm0_controller','all_pwm0_clients_excluded','remote_firmware_excluded','irq67_owned_disabled_drained','no_clock_reset_writers','exclusive_pd5_route'],
 'memory':['identity_device_mappings','no_aliases','permanent_reservation'],
 'power':['bldo1_pd_1800000','source_qualified_settling','fan_5v_topology_reviewed','q2_r89_loading_reviewed','no_external_gate_driver','fan_operation_permitted','no_voltage_pad_writers'],
 'supervision':['whole_board_power_removal','permanent_resource_retention','one_attempt','no_watchdog_arm','no_reset_retry','no_claim_of_fault_fan_off','continuous_output_until_power_removal_on_fault']}
LED_FACTS=['pl7_gpio_route','complete_latch_authority','bldo1_1800000_dcdc1_3300000','r_apbs0_stable_profile','no_optical_off_claim','remote_gpio_writers_excluded']
def produce(image,out):
 out.mkdir(parents=True,exist_ok=False);prep=module('pwm_fixture_prepare',ROOT/'apps/pwm/prepare.py');ih=hashlib.sha256(image.read_bytes()).hexdigest();parts,entry,identity=prep.base.image_parts(image)
 baseline=Path.home()/'.cache/sunstoneos/evidence/board-supervised-20261017/batch1/session-fixture.bin'
 providers=module('pwm_nominal_providers',ROOT/'checks/a733-provider-fixture.py')
 raw=providers.nominal_provider_session(baseline.read_bytes(),usb_reference_mode=1);assert len(raw)==592;raw[48:80]=identity;raw.extend(bytes(280));struct.pack_into('<5Q',raw,592,0x4153373350574131,2,872,23,91)
 raw[760:768]=bytes((0,0,0,7,0,1,0,0));struct.pack_into('<IIQ',raw,768,0,24000000,2000000)
 target=dict(identity='pwm-service-software-only-model',port='no-physical-port',image_sha256=ih)
 def record(name,value):
  data=json.dumps(value,sort_keys=True).encode();p=(out/name).resolve();p.write_bytes(data);return dict(path=str(p),sha256=hashlib.sha256(data).hexdigest())
 evidence=record('MODEL-NOT-PHYSICAL.json',dict(**target,physical_access=False,purpose='Software fixture only; never physical admission'))
 for offset in (80,216,248,280):raw[offset:offset+32]=bytes.fromhex(evidence['sha256'])
 refs={}
 for i,(key,required) in enumerate(FACTS.items()):
  refs[key]=record(key+'.json',dict(**target,schema='a733-pwm-'+key+'-v1',reviewed=True,physical_access=False,**{k:True for k in required}));raw[632+32*i:664+32*i]=bytes.fromhex(refs[key]['sha256'])
 led_fields=LED_FACTS
 refs['led']=record('led.json',dict(**target,schema='a733-pwm-led-v1',reviewed=True,physical_access=False,**{k:True for k in led_fields}))
 struct.pack_into('<3Q2I',raw,784,1,101,91,0x3fff,0);raw[816:819]=bytes((0,0,0));struct.pack_into('<2I',raw,824,0,24000000)
 raw[832:864]=bytes.fromhex(refs['led']['sha256']);struct.pack_into('<2I',raw,864,100,0)
 template=out/'session-template.bin';template.write_bytes(raw);prep.session_check(raw);prep.embedded_reference(image.read_bytes(),raw)
 conditions={'source-backed-cpu-dsu-transition','cold-cache-and-dirty-alias-exclusion','dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded','usb-clock-power-reset-ownership','usb-device-mapping','physical-counter-continuity'}
 platform=record('platform.json',dict(**target,schema='a733-smp-observer-platform-v1',entry_conditions_established=True,physical_coherence_result='under-test',conditions={k:evidence for k in sorted(conditions)}))
 ddr=record('ddr.json',dict(**target,schema='a733-ddr-reservation-v1',exclusive=True,published_cache_disabled=True,remote_writers_excluded=True,ranges=[[0x41000000,0x42200000]],evidence=[evidence]))
 admission=dict(**target,schema='a733-pwm-supervised-entry-v1',reservation=100,session_template_sha256=hashlib.sha256(raw).hexdigest(),ddr=ddr,platform=platform,**refs,whole_board_power_removal=True,no_resource_reuse_before_power_removal=True,recovery=dict(mode='manual-usb-reconnect',demonstrated_fel=True,explicit_reconnect_before_resumption=True,preserve_microsd=True,one_attempt=True,evidence=[evidence]))
 path=out/'admission.json';path.write_text(json.dumps(admission,indent=2)+'\n');prep.validate_admission(admission,ih,hashlib.sha256(raw).hexdigest(),raw)
 (out/'fixture-inputs.json').write_text(json.dumps(dict(baseline=str(baseline),baseline_sha256=hashlib.sha256(baseline.read_bytes()).hexdigest(),image_sha256=ih,build_id=identity.hex(),provider_source=str(providers.SOURCE),provider_source_sha256=providers.SOURCE_SHA256,physical_access=False),indent=2)+'\n')
 return template,path
if __name__=='__main__':
 import argparse
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();produce(a.image,a.output)
