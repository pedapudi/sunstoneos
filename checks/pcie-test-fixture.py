#!/usr/bin/env python3
"""Extend the actual-image PCIe fixture; all endpoint hardware facts are modeled."""
import hashlib,importlib.util,json,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
def produce(image,out):
 old=module('pcie_test_base_fixture',ROOT/'checks/pcie-application-fixture.py')
 template,admission=old.produce(image,out)
 prep=module('pcie_test_fixture_prepare',ROOT/'apps/pcie-test/prepare.py')
 raw=bytearray(template.read_bytes());raw.extend(bytes(48));struct.pack_into('<3Q',raw,592,0x4153373350544131,2,840);struct.pack_into('<2Q',raw,792,1000000,0)
 record=json.loads(admission.read_text());target={k:record[k] for k in ('identity','port','image_sha256')}
 def save(name,value):
  data=json.dumps(value,sort_keys=True).encode();path=(out/name).resolve();path.write_bytes(data);return dict(path=str(path),sha256=hashlib.sha256(data).hexdigest())
 model=save('endpoint-controller-model.json',dict(**target,physical_access=False,purpose='Controller, DMA channel, configuration and ordering are software models; no endpoint exists in this fixture.'))
 proof=dict(**target,schema='a733-pcie-endpoint-test-v1',reviewed=True,bar=0,
  protocol_revision='2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f',function_sha256='cb5cc6761615f0a0ef08344318bddd38a843aacea3398e521ab373f6f08476a8',
  controller=dict(name='SOFTWARE_MODEL_NOT_AN_ENDPOINT',revision='offline-only',sha256=model['sha256']),
  dma_driver=dict(name='SOFTWARE_MODEL_NOT_A_DMA_DRIVER',revision='offline-only',sha256=model['sha256']),
  tx_channel='MODELED_TX_MEMCPY',rx_channel='MODELED_RX_MEMCPY',dma_configuration_sha256=model['sha256'],dma_private=False,evidence=[model],
  **{k:True for k in ('bar_mmio_authorized','endpoint_dma_copy_authorized','single_msi_authorized','trusted_identity_inbound_accepted','tx_dma_memcpy','rx_dma_memcpy','copy_uses_rx','completion_before_outbound_unmap','writes_visible_before_status_msi','relaxed_ordering_disabled','ido_disabled')})
 record['endpoint_test']=save('endpoint-test.json',proof);raw[808:840]=bytes.fromhex(record['endpoint_test']['sha256'])
 template.write_bytes(raw);record['session_template_sha256']=hashlib.sha256(raw).hexdigest();admission.write_text(json.dumps(record,indent=2)+'\n')
 prep.session_check(raw);prep.validate_admission(record,target['image_sha256'],record['session_template_sha256'],raw)
 (out/'fixture-inputs.json').write_text(json.dumps(dict(image_sha256=target['image_sha256'],physical_access=False,scope='Actual linked-image identity and production validators; all hardware/endpoint/ordering facts explicitly modeled.'),indent=2)+'\n')
 return template,admission
if __name__=='__main__':
 import argparse
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();produce(a.image,a.output)
