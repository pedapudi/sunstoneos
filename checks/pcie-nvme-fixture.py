#!/usr/bin/env python3
"""Produce software-only PCIe NVMe session and admission records for the linked image.

The base PCIe fixture supplies the board session and the four PCIe evidence
records; this adds the NVMe fields and a reviewed-record stand-in. Every
controller and read authority here is explicitly modeled.
"""
import hashlib,importlib.util,json,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
LBA,NAMESPACE,TIMEOUT_US=8,1,1000000
def produce(image,out,lba=LBA):
 base=module('pcie_nvme_base_fixture',ROOT/'checks/pcie-application-fixture.py')
 template,admission=base.produce(image,out)
 prep=module('pcie_nvme_fixture_prepare',ROOT/'apps/pcie-nvme/prepare.py')
 raw=bytearray(template.read_bytes());raw.extend(bytes(prep.SESSION_BYTES-len(raw)))
 struct.pack_into('<3Q',raw,592,0x41533733504e4131,1,prep.SESSION_BYTES)
 struct.pack_into('<2Q2I',raw,792,TIMEOUT_US,lba,0,NAMESPACE)
 record=json.loads(admission.read_text());target={k:record[k] for k in ('identity','port','image_sha256')}
 vendor,device=struct.unpack_from('<2H',raw,788)
 def save(name,value):
  data=json.dumps(value,sort_keys=True).encode();path=(out/name).resolve();path.write_bytes(data);return dict(path=str(path),sha256=hashlib.sha256(data).hexdigest())
 model=save('nvme-controller-model.json',dict(**target,physical_access=False,purpose='The NVMe controller, its namespace and its DMA are software models; no device exists in this fixture.'))
 proof=dict(**target,schema='a733-pcie-nvme-read-v1',reviewed=True,vendor=vendor,device=device,function=0,namespace_id=NAMESPACE,lba=lba,
  controller=dict(model='SOFTWARE_MODEL_NOT_A_CONTROLLER',serial='offline-only',firmware='none'),evidence=[model],
  **{k:True for k in prep.NVME_FACTS})
 record['nvme']=save('nvme-read.json',proof);raw[816:848]=bytes.fromhex(record['nvme']['sha256'])
 template.write_bytes(raw);record['session_template_sha256']=hashlib.sha256(raw).hexdigest();admission.write_text(json.dumps(record,indent=2)+'\n')
 prep.session_check(raw);prep.validate_admission(record,target['image_sha256'],record['session_template_sha256'],raw)
 (out/'fixture-inputs.json').write_text(json.dumps(dict(image_sha256=target['image_sha256'],physical_access=False,scope='Actual linked-image identity and production validators; every controller, namespace and DMA fact is modeled.'),indent=2)+'\n')
 return template,admission
if __name__=='__main__':
 import argparse
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();produce(a.image,a.output)
