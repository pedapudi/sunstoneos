#!/usr/bin/env python3
"""Extend the actual PWM fixture producer with modeled boot-CPU entry authority."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
r=Path(__file__).resolve().parents[1]
def mod(n,p):
 s=importlib.util.spec_from_file_location(n,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def produce(image,out):
 fixture=mod('pwm_fixture',r/'checks/pwm-service-fixture.py');template,admission=fixture.produce(image,out)
 raw=bytearray(template.read_bytes());struct.pack_into('<I',raw,532,2)
 # Real PMIC construction requires inherited PL0/PL1 mux2 with valid pull/drive.
 # Reuse the full nominal profile from the accepted thermal startup producer.
 nominal=Path.home()/'.cache/sunstoneos/evidence/thermal-single-core-20261018/MODEL-session.bin'
 reference_raw=nominal.read_bytes();assert reference_raw[564:570]==bytes((2,1,1,2,1,1))
 raw[564:570]=reference_raw[564:570];template.write_bytes(raw)
 (out/'provider-profile.json').write_text(json.dumps(dict(source=str(nominal),sha256=hashlib.sha256(reference_raw).hexdigest(),fields='PL0/PL1 function, pull, drive',offset=564,bytes=raw[564:570].hex(),physical_access=False),indent=2)+'\n')
 record=json.loads(admission.read_text());path=Path(record['platform']['path']);fact=json.loads(path.read_text())
 reference=json.loads((out/'ddr.json').read_text())['evidence'][0]
 conditions=('primary-cache-transition-and-secondary-off','cold-cache-and-dirty-alias-exclusion','dual-pas-dram','exclusive-cpus-memory-dma','remote-writers-excluded','physical-code-publication','usb-clock-power-reset-ownership','usb-device-mapping','physical-counter-continuity','rom-usb0-pio-takeover','usb0-irq188-189-190-disabled-inactive-clear','usb-supplies-stable-and-writers-excluded')
 fact.update(schema='a733-board-single-core-platform-v1',conditions=dict.fromkeys(conditions,reference))
 data=json.dumps(fact,sort_keys=True).encode();path.write_bytes(data);record['platform']['sha256']=hashlib.sha256(data).hexdigest()
 record['session_template_sha256']=hashlib.sha256(raw).hexdigest();admission.write_text(json.dumps(record,indent=2)+'\n')
 prep=mod('pwm_prepare',r/'apps/pwm/prepare.py');prep.validate_admission(record,record['image_sha256'],record['session_template_sha256'],raw,execution_profile='pwm-single-core-supervised-v1')
 return template,admission
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for n in ('image','output'):p.add_argument('--'+n,type=Path,required=True)
 a=p.parse_args();produce(a.image,a.output)
